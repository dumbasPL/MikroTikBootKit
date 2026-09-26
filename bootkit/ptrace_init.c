/*
 * ptrace_init.c - boot-time LD_PRELOAD drop for MikroTik RouterOS (7.x, i386)
 *
 * Boot the stock initramfs with:   rdinit=/ptrace_init
 *
 *   /ptrace_init (PID 1)
 *       |-- fork -> tracer: PTRACE_SEIZE(1) and watch the init's syscalls;
 *       |           when it mounts a tmpfs on /ram, make the embedded probe
 *       |           available as /ram/ldpreload.so, detach and exit
 *       `-- execve("/init")   (the real init), with LD_PRELOAD pointing at
 *                             the probe
 *
 * The probe is embedded in this binary.  PID 1 also writes a copy of it to
 * /ptrace_init.so on the initramfs (rootfs) and keeps an open fd; when the
 * /ram tmpfs appears, the tracer bind-mounts the file from that fd over
 * /ram/ldpreload.so.  The bind mount keeps the rootfs inode, which matters on
 * 7.24.4: that kernel refuses PROT_EXEC mappings of tmpfs files (the ldso
 * would silently skip the probe), but rootfs inodes stay exec-mappable.
 *
 * Only the init is traced (no fork following): it is the process that mounts
 * the tmpfs on /ram, early in boot_stage_mount_system(), long before
 * sysinit/mode/loader exist.  Every dynamically linked binary started after
 * that point loads the probe; with debug=1 its constructor prints the
 * binary's name to /dev/console.
 *
 * Logging and the serial console are controlled at boot time by the
 * bootloader's debug= setting (\BOOTKIT.CFG): it adds bootkit_debug=1 to the
 * kernel command line and console=ttyS0,115200n8.  Production (debug=0)
 * prints only errors here and only the important licence/patch lines in the
 * probe; debug=1 prints every step.  DEBUG=1 ./build.sh sets the default for
 * newly installed configs.
 *
 * Details, and the target behaviours this relies on: docs/ptrace-init-preload.md
 * Build: ./build.sh      (DEBUG=1 for a test build)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/mount.h>
#include <sys/stat.h>

extern char **environ;

static int stash_fd = -1;

/* the LD_PRELOAD probe (preload.c), embedded by build.sh */
#include "preload_so.h"

#ifndef __WALL
#define __WALL 0x40000000
#endif
#ifndef PTRACE_O_TRACESYSGOOD
#define PTRACE_O_TRACESYSGOOD 0x00000001
#endif
#ifndef PTRACE_O_TRACEEXEC
#define PTRACE_O_TRACEEXEC 0x00000010
#endif
#ifndef PTRACE_EVENT_EXEC
#define PTRACE_EVENT_EXEC 4
#define PTRACE_EVENT_STOP 128
#endif
#ifndef PTRACE_GET_SYSCALL_INFO
#define PTRACE_GET_SYSCALL_INFO 0x420e
#endif

/* kernel's struct ptrace_syscall_info (5.3+) */
struct syscall_info {
	unsigned char op;
	unsigned char pad[3];
	unsigned int arch;
	unsigned long long instruction_pointer;
	unsigned long long stack_pointer;
	union {
		struct {
			unsigned long long nr;
			unsigned long long args[6];
		} entry;
		struct {
			long long rval;
			unsigned char is_error;
			unsigned char pad2[7];
		} exit;
	} u;
};
#define SYSCALL_INFO_ENTRY 1
#define SYSCALL_INFO_EXIT  2

/* ------------------------------------------------------------------ config */

#define REAL_INIT     "/init"			/* the stock init */

/* Put into the environment of the stock init; the probe is dropped there by
 * the tracer as soon as the init has mounted the tmpfs. */
#define PRELOAD_PATH  "/ram/ldpreload.so"

/* PID 1 keeps a copy of the probe on the initramfs (rootfs/ramfs) and the
 * tracer inherits an open fd to it.  On 7.24.4 the kernel refuses PROT_EXEC
 * mappings of tmpfs files, but a rootfs inode stays exec-mappable, so the
 * probe is bind-mounted from that fd (/proc/self/fd/N) over PRELOAD_PATH
 * instead of being written onto the tmpfs. */
#define PRELOAD_SRC       "/ptrace_init.so"

/* verbose logging default, overridden by the bootloader's debug= setting */
#ifndef BOOTKIT_DEBUG_DEFAULT
#define BOOTKIT_DEBUG_DEFAULT 0
#endif

static int g_verbose = BOOTKIT_DEBUG_DEFAULT;

/* ----------------------------------------------------------------- logging */

/* one write per line, so messages from the tracer and the probe do not
 * interleave on the console */
static void logv(const char *fmt, va_list ap)
{
	char buf[640];
	int n, fd;

	memcpy(buf, "[ptrace-init] ", 14);
	n = vsnprintf(buf + 14, sizeof(buf) - 14, fmt, ap);
	if (n < 0)
		return;
	if (n > (int)sizeof(buf) - 16)
		n = sizeof(buf) - 16;
	buf[14 + n++] = '\n';

	fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (fd < 0)
		fd = 2;
	write(fd, buf, (size_t)(14 + n));
	if (fd != 2)
		close(fd);
}

/* informational, only with boot-time debug=1 (see \BOOTKIT.CFG) */
static void logmsg(const char *fmt, ...)
{
	va_list ap;

	if (!g_verbose)
		return;
	va_start(ap, fmt);
	logv(fmt, ap);
	va_end(ap);
}

/* errors are always printed */
static void logerr(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	logv(fmt, ap);
	va_end(ap);
}

/*
 * The bootloader appends bootkit_debug=1 to the kernel command line when the
 * config says debug=1.  /proc is not mounted yet when we start, so mount a
 * private one, read the command line and take it down again.  Returns 0/1,
 * or -1 when the command line cannot be read (use the build default then).
 */
static int read_boot_debug(void)
{
	static const char marker[] = "bootkit_debug=1";
	char buf[2048];
	int fd, n, r = -1;

	fd = open("/proc/cmdline", O_RDONLY);
	if (fd >= 0) {
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n > 0) {
			buf[n] = 0;
			return strstr(buf, marker) != NULL;
		}
		return -1;
	}

	if (mkdir("/bootkit-proc", 0755) != 0 && errno != EEXIST)
		return -1;
	if (mount("proc", "/bootkit-proc", "proc", 0, NULL) != 0)
		return -1;
	fd = open("/bootkit-proc/cmdline", O_RDONLY);
	if (fd >= 0) {
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n > 0) {
			buf[n] = 0;
			r = strstr(buf, marker) != NULL;
		}
	}
	umount("/bootkit-proc");
	rmdir("/bootkit-proc");
	return r;
}

/* ------------------------------------------------------ tracee memory I/O */

/* PTRACE_PEEKDATA: one 4-byte word, 32-bit addresses */

static int peek_word(pid_t pid, unsigned long long addr, unsigned long *out)
{
	if (addr + sizeof(unsigned long) > 0x100000000ULL)
		return -1;
	errno = 0;
	*out = (unsigned long)ptrace(PTRACE_PEEKDATA, pid,
				     (void *)(uintptr_t)addr, 0);
	return errno ? -1 : 0;
}

/* read a NUL-terminated string */
static int read_cstr(pid_t pid, unsigned long long addr, char *out, size_t max)
{
	size_t off = 0;

	while (off + 1 < max) {
		unsigned long word;
		unsigned char *p = (unsigned char *)&word;
		size_t i, n = sizeof(word);

		if (peek_word(pid, addr + off, &word) != 0)
			return -1;
		if (n > max - off - 1)
			n = max - off - 1;
		for (i = 0; i < n; i++) {
			out[off++] = (char)p[i];
			if (!p[i])
				return (int)off - 1;
		}
	}
	out[max - 1] = 0;
	return (int)max - 1;
}

/* --------------------------------------------------------------- preload */

/* write the embedded probe to path (mode 0755) */
static int write_preload(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	size_t off = 0;

	if (fd < 0)
		return -1;
	while (off < preload_so_len) {
		ssize_t w = write(fd, preload_so + off, preload_so_len - off);

		if (w <= 0) {
			int e = errno;

			close(fd);
			errno = e;
			return -1;
		}
		off += (size_t)w;
	}
	close(fd);
	return 0;
}

/* create an empty regular file (bind-mount target) */
static int touch_file(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);

	if (fd < 0)
		return -1;
	close(fd);
	return 0;
}

/* stash a copy of the probe on the initramfs; run as PID 1 before the real
 * init is exec'd. */
static void stash_preload(void)
{
	if (write_preload(PRELOAD_SRC) != 0)
		logerr("cannot stash %s: %s", PRELOAD_SRC, strerror(errno));
}

/* tmpfs was mounted on /ram: make the probe available at PRELOAD_PATH by
 * bind-mounting the rootfs copy (exec-mappable everywhere, unlike a copy on
 * the tmpfs), then detach the init and exit. */
static void drop_preload(void)
{
	char src[64];

	if (touch_file(PRELOAD_PATH) != 0) {
		logerr("cannot create %s: %s", PRELOAD_PATH, strerror(errno));
		return;
	}
	if (stash_fd < 0) {
		logerr("%s is not open, cannot bind %s", PRELOAD_SRC,
		       PRELOAD_PATH);
		return;
	}
	snprintf(src, sizeof(src), "/proc/self/fd/%d", stash_fd);
	if (mount(src, PRELOAD_PATH, NULL, MS_BIND, NULL) != 0)
		logerr("cannot bind %s -> %s: %s", src, PRELOAD_PATH,
		       strerror(errno));
	else
		logmsg("bind-mounted %s -> %s, detaching", src, PRELOAD_PATH);
}

/* tmpfs was mounted on /ram: drop the probe, detach the init and exit */
static void finish(pid_t pid)
{
	drop_preload();
	ptrace(PTRACE_DETACH, pid, 0, 0);
}

/* ------------------------------------------------------------ tracer core */

static void tracer_main(pid_t parent, int ready_fd)
{
	long opts = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEEXEC;
	int mount_armed = 0;	/* mount() entry seen, check the exit stop */
	int root_armed = 0;	/* mount("tmpfs", "/newroot") seen */

	if (ptrace(PTRACE_SEIZE, parent, 0, (void *)opts) < 0) {
		logerr("PTRACE_SEIZE(%d) failed: %s", (int)parent,
		       strerror(errno));
		if (ready_fd >= 0) {
			char c = 'x';
			write(ready_fd, &c, 1);
			close(ready_fd);
		}
		return;
	}
	logmsg("tracing pid %d", (int)parent);
	if (ready_fd >= 0) {
		char c = 'R';
		write(ready_fd, &c, 1);
		close(ready_fd);
	}

	for (;;) {
		int status;
		unsigned ev;
		int sig;

		if (waitpid(parent, &status, __WALL) < 0) {
			if (errno == EINTR)
				continue;
			logerr("waitpid: %s", errno == ECHILD ?
			       "init is gone" : strerror(errno));
			return;
		}
		if (WIFEXITED(status) || WIFSIGNALED(status)) {
			logmsg("init exited, tracer exits");
			return;
		}

		ev = (unsigned)status >> 16;
		sig = WSTOPSIG(status);

		/* event stops (exec, group-stop): keep tracing */
		if (ev == PTRACE_EVENT_EXEC || ev == PTRACE_EVENT_STOP) {
			ptrace(PTRACE_SYSCALL, parent, 0, 0);
			continue;
		}

		if (sig == (SIGTRAP | 0x80)) {
			/* syscall stop (PTRACE_O_TRACESYSGOOD) */
			struct syscall_info si;
			long r;

			memset(&si, 0, sizeof(si));
			r = ptrace(PTRACE_GET_SYSCALL_INFO, parent,
				   (void *)sizeof(si), &si);
			if (r >= 0 && si.op == SYSCALL_INFO_ENTRY) {
				if (si.u.entry.nr == SYS_mount) {
					/* mount(source, target, fstype, flags, data) */
					char target[256], fstype[64];

					if (read_cstr(parent, si.u.entry.args[1],
						      target, sizeof(target)) > 0 &&
					    read_cstr(parent, si.u.entry.args[2],
						      fstype, sizeof(fstype)) > 0 &&
					    !strcmp(fstype, "tmpfs")) {
						if (!strcmp(target, "/ram")) {
							mount_armed = 1;
							logmsg("pid %d mounts tmpfs on /ram",
							       (int)parent);
						} else if (!strcmp(target, "/newroot")) {
							root_armed = 1;
						}
					}
				}
			} else if (r >= 0 && si.op == SYSCALL_INFO_EXIT &&
				   (mount_armed || root_armed)) {
				long long rv = si.u.exit.rval;

				if (root_armed) {
					root_armed = 0;
					if (rv == 0) {
						/* the init is about to move this
						 * tmpfs to "/" and chroot into it;
						 * follow it now, while /newroot is
						 * still visible from here */
						if (chdir("/newroot") != 0 ||
						    chroot(".") != 0 ||
						    chdir("/") != 0)
							logerr("cannot follow into /newroot: %s",
							       strerror(errno));
						else
							logmsg("following the init into /newroot");
					}
				}
				if (mount_armed) {
					mount_armed = 0;
					if (rv == 0) {
						/* the tmpfs is live in our
						 * namespace too - drop the probe
						 * and let go */
						finish(parent);
						return;
					}
					logerr("pid %d: mount(tmpfs, /ram) failed: %lld",
					       (int)parent, rv);
				}
			}
			ptrace(PTRACE_SYSCALL, parent, 0, 0);
			continue;
		}

		/* any other stop: forward real signals, swallow SIGTRAP */
		ptrace(PTRACE_SYSCALL, parent, 0,
		       (void *)(long)(sig == SIGTRAP ? 0 : sig));
	}
}

/* ------------------------------------------------------------ init part */

static void init_main(int argc, char **argv)
{
	int fds[2];
	pid_t t;

	/* would re-exec ourselves forever */
	if (argc > 0 && argv && argv[0] && !strcmp(argv[0], REAL_INIT)) {
		logerr("started as %s, halting", REAL_INIT);
		for (;;)
			pause();
	}

	/* boot-time debug= setting (kernel command line), and pass it down to
	 * the probe in the environment of the stock init and its children */
	{
		int dbg = read_boot_debug();

		if (dbg >= 0)
			g_verbose = dbg;
		setenv("BOOTKIT_DEBUG", g_verbose ? "1" : "0", 1);
	}

	stash_preload();
	/* O_CLOEXEC: the tracer (which never execs) keeps the inherited copy,
	 * while PID 1's copy closes when it execs the stock init, so the fd
	 * does not leak into RouterOS */
	stash_fd = open(PRELOAD_SRC, O_RDONLY | O_CLOEXEC);
	if (stash_fd < 0)
		logerr("cannot open %s: %s", PRELOAD_SRC, strerror(errno));

	if (pipe(fds) != 0) {
		logerr("pipe: %s", strerror(errno));
		fds[0] = fds[1] = -1;
	}
	t = fork();
	if (t == 0) {
		if (fds[0] >= 0)
			close(fds[0]);
		tracer_main(getppid(), fds[1]);
		_exit(0);
	}
	if (t > 0 && fds[0] >= 0) {
		struct pollfd p;
		char c;

		close(fds[1]);
		p.fd = fds[0];
		p.events = POLLIN;
		if (poll(&p, 1, 5000) > 0)
			(void)read(fds[0], &c, 1);
		close(fds[0]);
	} else if (t > 0) {
		usleep(200000);		/* no pipe: give the tracer a moment */
	}

	logmsg("exec %s (LD_PRELOAD=%s)", REAL_INIT, PRELOAD_PATH);
	/* put the preload into the environment the stock init inherits */
	setenv("LD_PRELOAD", PRELOAD_PATH, 1);
	execve(REAL_INIT, (char *[]){ (char *)REAL_INIT, NULL }, environ);
	logerr("execve(%s): %s, halting", REAL_INIT, strerror(errno));
	for (;;)
		pause();
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
	if (getpid() != 1) {
		fprintf(stderr, "ptrace_init: must run as PID 1 "
			"(boot with rdinit=/ptrace_init)\n");
		return 1;
	}
	init_main(argc, argv);
	return 0;
}
