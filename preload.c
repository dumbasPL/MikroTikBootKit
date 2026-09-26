/*
 * preload.c - LD_PRELOAD probe for RouterOS (i386)
 *
 * Ordinary C, but linked without libc (-nostdlib, see build.sh): the shared
 * object has no DT_NEEDED entry and leaves open/write/readlink/getpid/
 * snprintf/strlen/... undefined, so the dynamic linker binds them to the libc
 * already present in the process (RouterOS's /lib/libc.so) when the library
 * is preloaded.
 *
 * When it is loaded, its ELF constructor writes one line to /dev/console
 * naming the executable that loaded it.  On top of that:
 *
 *   /nova/bin/mode    the embedded keygen (keygen.c, an embeddable copy of
 *                     MikroTikPatch's) generates and signs the licence blob
 *                     with the custom key pair, and the licence public key
 *                     the stock verifier builds on the stack is replaced in
 *                     memory, so mode accepts the licence and raises the
 *                     level to 6.
 *   /nova/bin/keyman  the licence public key is replaced in memory too.
 *   everything else   only the console line.  In particular the loader is
 *                     never touched: RouterOS cross-checks its embedded key
 *                     against other state at boot and a modified loader
 *                     aborts the system supervisor (/nova/bin/sys2).
 *
 * The in-memory patch is the runtime equivalent of MikroTikPatch's
 * ReplaceKeyArch(): the x86 binaries store the 32-byte public key as eight
 * `mov dword [ebp-x], imm32` immediates in .text (up to 6 bytes between the
 * 4-byte chunks).  The chunks are replaced in place and the bytes of the
 * instructions in between are left alone.  The affected pages are made
 * writable for the patch and restored to r-x afterwards; only the private
 * (COW) text mapping is touched, so the file on disk is untouched.
 *
 * The key material comes from keys.env (see build.sh; the local one or
 * MikroTikPatch's): the same custom pair the reference build bakes into its
 * images, so the licence this probe signs verifies against the key it patches
 * in.  Build by build.sh into preload.so; embedded in ptrace_init.
 */

#define _GNU_SOURCE

/*
 * The embedded keygen: keygen.c is a copy of MikroTikPatch's standalone
 * keygen.c trimmed to the generation role (no CLI, no mode2 hand-over, no
 * exit()); its kg_generate()/kg_error() are called below.  The licence key
 * pair is selected with -DKEYGEN_LICENSE_PUBLIC_HEX /
 * -DKEYGEN_LICENSE_PRIVATE_HEX (see build.sh).
 */
#include "keygen.c"

#include <stdarg.h>
#include <sys/mman.h>

/* The stock RouterOS licence public key, as MIKRO_LICENSE_PUBLIC_KEY in
 * MikroTikPatch's keys.env; build.sh overrides it when the file is present. */
#ifndef STOCK_LICENSE_PUBLIC_HEX
#define STOCK_LICENSE_PUBLIC_HEX \
	"8E1067E4305FCDC0CFBF95C10F96E5DFE8C49AEF486BD1A4E2E96C27F01E3E32"
#endif

/* up to this many bytes may sit between two 4-byte key chunks */
#define KEY_GAP_MAX 6
#define KEY_CHUNKS  8

/* ----------------------------------------------------------------- logging */

static void console_log(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	size_t n;
	int fd;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	n = strlen(line);
	fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (fd < 0)
		fd = STDERR_FILENO;
	if (n > 0)
		write(fd, line, n);
	if (fd != STDERR_FILENO)
		close(fd);
}

/* ------------------------------------------------------- in-memory key patch */

/* Match chunks idx..7 of oldk in buf at offsets >= after, with at most
 * KEY_GAP_MAX bytes between consecutive chunks (same greedy search as
 * MikroTikPatch's matchGreedy).  ends[] receives the offset of each chunk;
 * returns the end offset of the last chunk, or 0 when there is no match. */
static size_t match_key(const unsigned char *buf, size_t len, size_t after,
			int idx, const unsigned char *oldk, size_t *ends)
{
	int gap;

	if (idx == KEY_CHUNKS)
		return ends[KEY_CHUNKS - 1] + 4;
	if (after + 4 > len)
		return 0;
	for (gap = KEY_GAP_MAX; gap >= 0; gap--) {
		size_t p = after + (size_t)gap;

		if (p + 4 > len)
			continue;
		if (memcmp(buf + p, oldk + 4 * idx, 4) != 0)
			continue;
		ends[idx] = p;
		if (match_key(buf, len, p + 4, idx + 1, oldk, ends) != 0)
			return ends[KEY_CHUNKS - 1] + 4;
	}
	return 0;
}

/* Replace every chunked copy of oldk in buf with newk.  Returns the number of
 * sites patched. */
static int replace_key(unsigned char *buf, size_t len,
		       const unsigned char *oldk, const unsigned char *newk)
{
	static size_t ends[KEY_CHUNKS];
	size_t i = 0;
	int hits = 0;

	while (i + 4 <= len) {
		size_t end;
		int c;

		if (memcmp(buf + i, oldk, 4) != 0) {
			i++;
			continue;
		}
		ends[0] = i;
		end = match_key(buf, len, i + 4, 1, oldk, ends);
		if (end == 0) {
			i++;
			continue;
		}
		for (c = 0; c < KEY_CHUNKS; c++)
			memcpy(buf + ends[c], newk + 4 * c, 4);
		hits++;
		i = end;
	}
	return hits;
}

static const char *parse_hex_ul(const char *s, unsigned long *out)
{
	unsigned long v = 0;

	while ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f')) {
		v = v * 16 +
		    (unsigned long)(*s <= '9' ? *s - '0' : *s - 'a' + 10);
		s++;
	}
	*out = v;
	return s;
}

/* Patch the licence public key in the mapped text of the running main
 * executable (exe = its /proc/self/exe path).  Returns the number of sites
 * patched, or -1 when the key material or /proc/self/maps is unavailable. */
static int patch_licence_key(const char *exe)
{
	static char maps[65536];
	unsigned char oldk[32], newk[32];
	size_t off = 0, exelen = strlen(exe);
	ssize_t n;
	char *line;
	int fd, hits = 0, regions = 0;
	int denied = 0;

	if (!hex_decode(oldk, sizeof(oldk), STOCK_LICENSE_PUBLIC_HEX) ||
	    !hex_decode(newk, sizeof(newk), license_public_hex))
		return -1;

	fd = open("/proc/self/maps", O_RDONLY);
	if (fd < 0)
		return -1;
	while (off < sizeof(maps) - 1 &&
	       (n = read(fd, maps + off, sizeof(maps) - 1 - off)) > 0)
		off += (size_t)n;
	close(fd);
	maps[off] = '\0';

	for (line = maps; line != NULL && *line != '\0'; ) {
		char *nl = strchr(line, '\n');
		char *p, *path;
		unsigned long start, end;

		if (nl != NULL)
			*nl = '\0';

		p = (char *)parse_hex_ul(line, &start);
		if (*p != '-')
			goto next;
		p = (char *)parse_hex_ul(p + 1, &end);
		while (*p == ' ')
			p++;
		if (*p == '\0' || p[2] != 'x')	/* not executable */
			goto next;
		path = strchr(p, '/');
		if (path == NULL)
			goto next;
		if (strncmp(path, exe, exelen) != 0)
			goto next;
		if (path[exelen] != '\0' && path[exelen] != ' ')
			goto next;
		if (end > start) {
			regions++;
			/* try rw first: some kernels reject rwx (W^X) */
			if (mprotect((void *)start, end - start,
				     PROT_READ | PROT_WRITE) != 0 &&
			    mprotect((void *)start, end - start,
				     PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
				denied++;
				goto next;
			}
			hits += replace_key((unsigned char *)start,
					    end - start, oldk, newk);
			mprotect((void *)start, end - start,
				 PROT_READ | PROT_EXEC);
		}
next:
		line = nl ? nl + 1 : NULL;
	}
	if (denied > 0)
		console_log("[ldpreload] %s: %d/%d text region(s) could not be made writable\n",
			    exe, denied, regions);
	return hits;
}

/* ---------------------------------------------------------------- keygen */

/* Run the embedded keygen in this process: generate the software id if
 * needed, derive the licence value for the mode recorded in the blob, sign
 * it with the custom private key and store the blob.  A still-valid licence
 * that is already installed is kept (see kg_generate()). */
static void run_embedded_keygen(void)
{
	char system_id[KG_SYSTEM_ID_MAX];
	int changed = 0;

	if (kg_generate(system_id, sizeof(system_id), &changed) == 0)
		console_log("[ldpreload] mode: licence %s (system-id %s)\n",
			    changed ? "generated" : "already installed",
			    system_id);
	else
		console_log("[ldpreload] mode: keygen failed: %s\n",
			    kg_error());
}

/* ------------------------------------------------------------------- init */

__attribute__((constructor))
static void preload_init(void)
{
	char exe[512];
	const char *base;
	ssize_t n;

	n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n > 0)
		exe[n] = '\0';
	else
		strcpy(exe, "?");

	base = strrchr(exe, '/');
	base = (base != NULL) ? base + 1 : exe;

	console_log("[ldpreload] loaded by %s (pid=%d)\n", exe,
		    (int)getpid());

	if (strcmp(base, "mode") == 0) {
		int hits = patch_licence_key(exe);

		console_log("[ldpreload] mode: licence key patched (%d site%s)\n",
			    hits, hits == 1 ? "" : "s");
		run_embedded_keygen();
	} else if (strcmp(base, "keyman") == 0) {
		int hits = patch_licence_key(exe);

		console_log("[ldpreload] keyman: licence key patched (%d site%s)\n",
			    hits, hits == 1 ? "" : "s");
	}
}
