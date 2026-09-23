/*
 * ptrace_init.c - boot-time exec hijack for MikroTik RouterOS (7.x, i386)
 *
 * Boot the stock initramfs with:   rdinit=/ptrace_init
 *
 *   /ptrace_init (PID 1)
 *       |-- fork -> tracer: PTRACE_SEIZE(1), follow fork/exec events; at the
 *       |           execve of "mode" patch the path to /proc/<tracer-pid>/exe
 *       `-- execve("/init")   (the real init)
 *
 * Exec'd this way (as "mode"): write /flash/rw/disk/flag.txt and then
 * execve("/nova/bin/mode").
 *
 * Details, and the target behaviours this relies on: docs/ptrace-init-hijack.md
 * Build: ./build.sh
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
#include <time.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <limits.h>

#ifndef __WALL
#define __WALL 0x40000000
#endif
#ifndef PTRACE_O_TRACESYSGOOD
#define PTRACE_O_TRACESYSGOOD 0x00000001
#endif
#ifndef PTRACE_O_TRACEFORK
#define PTRACE_O_TRACEFORK 0x00000002
#endif
#ifndef PTRACE_O_TRACEVFORK
#define PTRACE_O_TRACEVFORK 0x00000004
#endif
#ifndef PTRACE_O_TRACECLONE
#define PTRACE_O_TRACECLONE 0x00000008
#endif
#ifndef PTRACE_O_TRACEEXEC
#define PTRACE_O_TRACEEXEC 0x00000010
#endif
#ifndef PTRACE_O_TRACEEXIT
#define PTRACE_O_TRACEEXIT 0x00000040
#endif
#ifndef PTRACE_EVENT_FORK
#define PTRACE_EVENT_FORK 1
#define PTRACE_EVENT_VFORK 2
#define PTRACE_EVENT_CLONE 3
#define PTRACE_EVENT_EXEC 4
#define PTRACE_EVENT_EXIT 6
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

#define ORIG_MODE     "/nova/bin/mode"		/* the real licence daemon */
#define FLAG_PATH     "/flash/rw/disk/flag.txt"	/* written by the payload */
#define REAL_INIT     "/init"			/* the stock init */

/* ----------------------------------------------------------------- logging */

/* one write per line, so messages from the tracer and the payload do not
 * interleave on the console */
static void logmsg(const char *fmt, ...)
{
	char buf[640];
	va_list ap;
	int n, fd;

	memcpy(buf, "[ptrace-init] ", 14);
	va_start(ap, fmt);
	n = vsnprintf(buf + 14, sizeof(buf) - 14, fmt, ap);
	va_end(ap);
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

/* ----------------------------------------------------------- tracee table */

#define MAXT 256
struct tracee {
	pid_t pid;
	pid_t parent;
	int event_only;		/* PTRACE_CONT: no syscall stops */
};

static struct tracee tt[MAXT];
static int ntt;
static pid_t root_pid;		/* the seized init */
static pid_t hijacked;		/* exec redirected */
static int shutdown_flag;	/* detach and exit */

static struct tracee *t_find(pid_t p)
{
	int i;
	for (i = 0; i < ntt; i++)
		if (tt[i].pid == p)
			return &tt[i];
	return NULL;
}

static struct tracee *t_add(pid_t p, pid_t par)
{
	struct tracee *t = t_find(p);

	if (t) {
		if (par)
			t->parent = par;
		return t;
	}
	if (ntt >= MAXT)
		return NULL;
	t = &tt[ntt++];
	t->pid = p;
	t->parent = par;
	t->event_only = 0;
	return t;
}

static void t_del(pid_t p)
{
	int i;
	for (i = 0; i < ntt; i++)
		if (tt[i].pid == p) {
			tt[i] = tt[--ntt];
			return;
		}
}

/* ------------------------------------------------------ tracee memory I/O */

/* PTRACE_PEEKDATA/POKEDATA: one 4-byte word, 32-bit addresses */

static int peek_word(pid_t pid, unsigned long long addr, unsigned long *out)
{
	if (addr + sizeof(unsigned long) > 0x100000000ULL)
		return -1;
	errno = 0;
	*out = (unsigned long)ptrace(PTRACE_PEEKDATA, pid,
				     (void *)(uintptr_t)addr, 0);
	return errno ? -1 : 0;
}

static int poke_word(pid_t pid, unsigned long long addr, unsigned long word)
{
	if (addr + sizeof(unsigned long) > 0x100000000ULL)
		return -1;
	return ptrace(PTRACE_POKEDATA, pid, (void *)(uintptr_t)addr,
		      (void *)word) < 0 ? -1 : 0;
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

/* write len bytes (read-modify-write for the tail word) */
static int write_bytes(pid_t pid, unsigned long long addr, const void *buf,
		       size_t len)
{
	const unsigned char *src = buf;
	size_t off = 0;

	while (off < len) {
		unsigned long word;
		size_t n = len - off;

		if (n >= sizeof(word)) {
			memcpy(&word, src + off, sizeof(word));
			if (poke_word(pid, addr + off, word) != 0)
				return -1;
			off += sizeof(word);
		} else {
			if (peek_word(pid, addr + off, &word) != 0)
				return -1;
			memcpy((unsigned char *)&word, src + off, n);
			if (poke_word(pid, addr + off, word) != 0)
				return -1;
			off += n;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ misc */

static void mkdir_p(const char *path)
{
	char tmp[PATH_MAX];
	size_t i, len;

	snprintf(tmp, sizeof(tmp), "%s", path);
	len = strlen(tmp);
	for (i = 1; i < len; i++) {
		if (tmp[i] == '/') {
			tmp[i] = 0;
			mkdir(tmp, 0755);
			tmp[i] = '/';
		}
	}
	mkdir(tmp, 0755);
}

/* ---------------------------------------------------------------- hijack */

/* Redirect the pending execve to /proc/<our pid>/exe, a magic link to this
 * binary in the initramfs.  The new path must fit in the old one. */
static void hijack(pid_t pid, unsigned long long addr, const char *origpath)
{
	char newpath[32];
	size_t origlen = strlen(origpath);

	snprintf(newpath, sizeof(newpath), "/proc/%d/exe", (int)getpid());
	if (strlen(newpath) > origlen) {
		logmsg("hijack: %s does not fit in %s, skipping",
		       newpath, origpath);
		return;
	}
	if (write_bytes(pid, addr, newpath, strlen(newpath) + 1) != 0) {
		logmsg("hijack: cannot patch path at %llx", addr);
		return;
	}
	hijacked = pid;
	logmsg("hijacked pid %d: %s -> %s", (int)pid, origpath, newpath);
}

/* ------------------------------------------------------------ tracer core */

/* make every tracee stop soon (shutdown) */
static void interrupt_all(void)
{
	int i;
	for (i = 0; i < ntt; i++)
		ptrace(PTRACE_INTERRUPT, tt[i].pid, 0, 0);
}

static void tracer_main(pid_t parent, int ready_fd)
{
	long opts = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEFORK |
		    PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE |
		    PTRACE_O_TRACEEXEC | PTRACE_O_TRACEEXIT;

	if (ptrace(PTRACE_SEIZE, parent, 0, (void *)opts) < 0) {
		logmsg("PTRACE_SEIZE(%d) failed: %s", (int)parent,
		       strerror(errno));
		if (ready_fd >= 0) {
			char c = 'x';
			write(ready_fd, &c, 1);
			close(ready_fd);
		}
		return;
	}
	root_pid = parent;
	t_add(parent, 0);
	logmsg("tracing pid %d", (int)parent);
	if (ready_fd >= 0) {
		char c = 'R';
		write(ready_fd, &c, 1);
		close(ready_fd);
	}

	for (;;) {
		int status;
		pid_t pid;
		struct tracee *t;
		unsigned ev;
		int sig;

		pid = waitpid(-1, &status, __WALL);
		if (pid < 0) {
			if (errno == ECHILD) {
				logmsg("no tracees left, tracer exits");
				return;
			}
			continue;	/* EINTR and friends */
		}
		if (WIFEXITED(status) || WIFSIGNALED(status)) {
			t_del(pid);
			continue;
		}

		ev = (unsigned)status >> 16;
		sig = WSTOPSIG(status);
		t = t_find(pid);
		if (!t)
			t = t_add(pid, 0);
		if (!t) {
			ptrace(PTRACE_DETACH, pid, 0, 0);
			continue;
		}

		if (shutdown_flag) {
			ptrace(PTRACE_DETACH, pid, 0, 0);
			t_del(pid);
			if (ntt == 0) {
				logmsg("all tracees detached, tracer exits");
				return;
			}
			continue;
		}

		if (ev == PTRACE_EVENT_FORK || ev == PTRACE_EVENT_VFORK ||
		    ev == PTRACE_EVENT_CLONE) {
			unsigned long msg = 0;

			ptrace(PTRACE_GETEVENTMSG, pid, 0, &msg);
			t_add((pid_t)msg, pid);
			ptrace(t->event_only ? PTRACE_CONT : PTRACE_SYSCALL,
			       pid, 0, 0);
			continue;
		}
		if (ev == PTRACE_EVENT_EXEC) {
			if (pid == hijacked) {
				logmsg("pid %d runs the payload, detaching",
				       (int)pid);
				ptrace(PTRACE_DETACH, pid, 0, 0);
				t_del(pid);
				shutdown_flag = 1;
				interrupt_all();
				if (ntt == 0)
					return;
				continue;
			}
			ptrace(t->event_only ? PTRACE_CONT : PTRACE_SYSCALL,
			       pid, 0, 0);
			continue;
		}
		if (ev == PTRACE_EVENT_EXIT || ev == PTRACE_EVENT_STOP) {
			ptrace(t->event_only ? PTRACE_CONT : PTRACE_SYSCALL,
			       pid, 0, 0);
			continue;
		}

		if (sig == (SIGTRAP | 0x80)) {
			/* syscall stop (PTRACE_O_TRACESYSGOOD) */
			struct syscall_info si;
			long r;

			memset(&si, 0, sizeof(si));
			r = ptrace(PTRACE_GET_SYSCALL_INFO, pid,
				   (void *)sizeof(si), &si);
			if (r >= 0 && si.op == SYSCALL_INFO_ENTRY) {
				/* execve 11/59, execveat 358/322 (i386/x86_64) */
				unsigned long long nr = si.u.entry.nr;
				int is_exec = (nr == 11 || nr == 59);
				int is_execat = (nr == 322 || nr == 358);

				if (is_exec || is_execat) {
					unsigned long long a =
					    si.u.entry.args[is_exec ? 0 : 1];
					char path[PATH_MAX];

					if (a && read_cstr(pid, a, path,
							   sizeof(path)) > 0) {
						const char *base =
						    strrchr(path, '/');
						base = base ? base + 1 : path;

						if (!strcmp(base, "sysinit")) {
							/* it forks mode */
							logmsg("sysinit is pid %d",
							       (int)pid);
							if (t_find(root_pid))
								t_find(root_pid)->event_only = 1;
						} else if (!strcmp(base, "mode")) {
							if (!hijacked)
								hijack(pid, a, path);
						} else if (pid == root_pid) {
							/* init re-execs itself */
						} else {
							/* loader etc.: untraced */
							ptrace(PTRACE_DETACH, pid, 0, 0);
							t_del(pid);
							continue;
						}
					}
				}
			}
			ptrace(PTRACE_SYSCALL, pid, 0, 0);
			continue;
		}

		/* any other stop: forward real signals, swallow SIGTRAP */
		ptrace(t->event_only ? PTRACE_CONT : PTRACE_SYSCALL, pid, 0,
		       (void *)(long)(sig == SIGTRAP ? 0 : sig));
	}
}

/* ------------------------------------------------------------ init part */

static void init_main(int argc, char **argv, char **envp)
{
	int fds[2];
	pid_t t;

	/* would re-exec ourselves forever */
	if (argc > 0 && argv && argv[0] && !strcmp(argv[0], REAL_INIT)) {
		logmsg("started as %s, halting", REAL_INIT);
		for (;;)
			pause();
	}

	if (pipe(fds) != 0) {
		logmsg("pipe: %s", strerror(errno));
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

	logmsg("exec %s", REAL_INIT);
	execve(REAL_INIT, (char *[]){ (char *)REAL_INIT, NULL }, envp);
	logmsg("execve(%s): %s, halting", REAL_INIT, strerror(errno));
	for (;;)
		pause();
}

/* --------------------------------------------------- payload (as "mode") */

static void mode_main(int argc, char **argv, char **envp)
{
	char self[PATH_MAX] = "?";
	char dir[PATH_MAX];
	ssize_t n;
	int fd;

	(void)argc;

	n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (n > 0)
		self[n] = 0;		/* for the flag text */

	/* /flash/rw/disk normally exists */
	snprintf(dir, sizeof(dir), "%s", FLAG_PATH);
	{
		char *slash = strrchr(dir, '/');
		if (slash) {
			*slash = 0;
			mkdir_p(dir);
		}
	}
	fd = open(FLAG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		dprintf(fd,
			"ptrace_init hijack: OK\n"
			"exe=%s\n"
			"pid=%d\n"
			"ppid=%d\n"
			"time=%ld\n",
			self, (int)getpid(), (int)getppid(), (long)time(NULL));
		close(fd);
		logmsg("mode hijack: wrote %s", FLAG_PATH);
	} else {
		logmsg("mode hijack: cannot write %s: %s", FLAG_PATH,
		       strerror(errno));
	}

	/* the real licence daemon */
	execv(ORIG_MODE, argv);
	logmsg("mode hijack: execv(%s): %s", ORIG_MODE, strerror(errno));
	_exit(127);
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv, char **envp)
{
	/* PID 1 is the wrapper, anything else is the payload */
	if (getpid() == 1) {
		init_main(argc, argv, envp);
		return 0;
	}
	mode_main(argc, argv, envp);
	return 127;
}
