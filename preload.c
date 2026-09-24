/*
 * preload.c - LD_PRELOAD probe for RouterOS (i386)
 *
 * Ordinary C, but linked without libc (-nostdlib, see build.sh): the shared
 * object has no DT_NEEDED entry and leaves open/write/readlink/getpid/
 * snprintf/strlen undefined, so the dynamic linker binds them to the libc
 * already present in the process (RouterOS's /lib/libc.so) when the library
 * is preloaded.
 *
 * When it is loaded, its ELF constructor writes one line to /dev/console
 * naming the executable that loaded it, so the console shows every binary
 * that honoured LD_PRELOAD.  Built by build.sh into preload.so and embedded
 * in ptrace_init.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

__attribute__((constructor))
static void preload_report(void)
{
	char exe[512];
	char line[640];
	ssize_t n;
	int fd;

	n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n > 0)
		exe[n] = 0;
	else
		strcpy(exe, "?");

	snprintf(line, sizeof(line), "[ldpreload] loaded by %s (pid=%d)\n",
		 exe, (int)getpid());

	fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (fd < 0)
		fd = 2;
	write(fd, line, strlen(line));
	if (fd != 2)
		close(fd);
}
