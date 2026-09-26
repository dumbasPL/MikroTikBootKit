/*
 * preload.c - LD_PRELOAD probe for RouterOS (i386)
 *
 * Ordinary C, but linked without libc (-nostdlib, see build.sh): the shared
 * object has no DT_NEEDED entry and leaves open/write/readlink/getpid/
 * snprintf/strlen/... undefined, so the dynamic linker binds them to the libc
 * already present in the process (RouterOS's /lib/libc.so) when the library
 * is preloaded.
 *
 * The boot-time "debug=" setting in \BOOTKIT.CFG (passed down as
 * BOOTKIT_DEBUG by ptrace_init) decides how much is logged.  In production
 * (debug=0) only the important lines are printed to /dev/console: the
 * licence state and the three in-memory patches plus errors.  With debug=1
 * the constructor also logs one line per process loaded, naming the
 * executable and its pid.  On top of that:
 *
 *   /nova/bin/mode    the embedded keygen (keygen.c, an embeddable copy of
 *                     MikroTikPatch's) generates and signs the licence blob
 *                     with the custom key pair, and the licence public key
 *                     the stock verifier builds on the stack is replaced in
 *                     memory, so mode accepts the licence and raises the
 *                     level to 6.
 *   /nova/bin/keyman  the licence public key is replaced in memory too.
 *   /nova/bin/loader  the memcmp GOT slot is redirected to a stub that
 *                     always returns 0 (the same trick the recovered
 *                     reference tool uses): the loader's licence verifier
 *                     ends in memcmp(hash, witness) == 0, so it accepts any
 *                     blob and never flags the custom licence.  The loader's
 *                     text and key material are left untouched.
 *   everything else   nothing (debug=1: only the console line)
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
#include <stdlib.h>
#include <sys/mman.h>

/*
 * Verbose per-process logging (the "loaded by" line) is off in production
 * builds; ptrace_init passes the boot-time "debug=" setting in the
 * BOOTKIT_DEBUG environment variable, the build sets the fallback.
 */
#ifndef BOOTKIT_DEBUG_DEFAULT
#define BOOTKIT_DEBUG_DEFAULT 0
#endif

static int verbose_log(void)
{
	static int v = -1;

	if (v < 0) {
		const char *e = getenv("BOOTKIT_DEBUG");

		v = e ? (e[0] == '1') : BOOTKIT_DEBUG_DEFAULT;
	}
	return v;
}

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

/* ------------------------------------------------- memcmp PLT/GOT redirect */

/*
 * The loader's calls to memcmp are redirected to a stub that always reports
 * "equal".  This is what the recovered reference tool does (its stub patches
 * the loader's memcmp GOT entry, 0x805d014 in 7.23.7): the loader's licence
 * verifier ends in `memcmp(hash, witness, 16) == 0`, and forcing it true makes
 * the verifier accept any blob, so the loader never flags the custom-signed
 * licence.  The GOT slot is found from the executable's own PLT/JMP relocs
 * (dynsym/dynstr), so no hard-coded address is needed.
 *
 * The redirect is per-process (only the loader's GOT slot is written) and the
 * stub is a private symbol, so every other binary keeps the libc memcmp.
 */

/* the loader's memcmp calls land here: everything compares "equal" */
static int stub_memcmp(const void *a, const void *b, size_t n)
{
	(void)a;
	(void)b;
	(void)n;
	return 0;
}

typedef struct {
	unsigned char ident[16];
	unsigned short type, machine;
	unsigned int version, entry, phoff, shoff, flags;
	unsigned short ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed)) elf32_ehdr;

typedef struct {
	unsigned int type, offset, vaddr, paddr, filesz, memsz, flags, align;
} __attribute__((packed)) elf32_phdr;

typedef struct { unsigned int tag, val; } __attribute__((packed)) elf32_dyn;
typedef struct { unsigned int offset, info; } __attribute__((packed)) elf32_rel;
typedef struct {
	unsigned int name, value, size;
	unsigned char info, other;
	unsigned short shndx;
} __attribute__((packed)) elf32_sym;

#define PT_LOAD     1
#define PT_DYNAMIC  2
#define DT_NULL     0
#define DT_PLTRELSZ 2
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_REL      17
#define DT_RELSZ    18
#define DT_PLTREL   20
#define DT_JMPREL   23

/* PROT_* flags of the mapping that contains addr, -1 when not found */
static int region_prot(const char *maps, unsigned long addr)
{
	const char *p = maps;

	while (p != NULL && *p != '\0') {
		const char *nl = strchr(p, '\n');
		unsigned long start, end;
		const char *q = parse_hex_ul(p, &start);

		if (*q == '-') {
			q = parse_hex_ul(q + 1, &end);
			while (*q == ' ')
				q++;
			if (addr >= start && addr < end) {
				int prot = 0;

				if (q[0] == 'r')
					prot |= PROT_READ;
				if (q[1] == 'w')
					prot |= PROT_WRITE;
				if (q[2] == 'x')
					prot |= PROT_EXEC;
				return prot;
			}
		}
		p = nl ? nl + 1 : NULL;
	}
	return -1;
}

/* lowest mapping of the executable (its ELF header page), 0 when absent */
static unsigned long exe_map_lowest(const char *maps, const char *exe)
{
	size_t exelen = strlen(exe);
	unsigned long lowest = 0;
	const char *p = maps;

	while (p != NULL && *p != '\0') {
		const char *nl = strchr(p, '\n');
		const char *eol = nl ? nl : p + strlen(p);
		unsigned long start, end;
		const char *q = parse_hex_ul(p, &start);

		if (*q == '-') {
			q = parse_hex_ul(q + 1, &end);
			while (*q == ' ' && q < eol)
				q++;
			if (exelen > 0) {
				const char *path = NULL;
				const char *s = q;

				while (s < eol) {
					if (*s == '/') {
						path = s;
						break;
					}
					s++;
				}
				if (path != NULL &&
				    (size_t)(eol - path) >= exelen &&
				    strncmp(path, exe, exelen) == 0 &&
				    ((size_t)(eol - path) == exelen ||
				     path[exelen] == ' ') &&
				    (lowest == 0 || start < lowest))
					lowest = start;
			}
		}
		p = nl ? nl + 1 : NULL;
	}
	return lowest;
}

/* replace every reloc table entry for "memcmp" with the stub address */
static int patch_rel_table(unsigned long addr, unsigned long size,
			   unsigned long symtab, unsigned long strtab,
			   unsigned long l_addr, const char *maps)
{
	elf32_rel *rel = (elf32_rel *)addr;
	int count = (int)(size / sizeof(elf32_rel));
	int i, hits = 0;

	for (i = 0; i < count; i++) {
		unsigned int symidx = rel[i].info >> 8;
		elf32_sym *sym;
		const char *name;
		unsigned long slot, page;
		int prot;

		if (symidx == 0)
			continue;
		sym = (elf32_sym *)(symtab + symidx * sizeof(elf32_sym));
		if (sym->name == 0)
			continue;
		name = (const char *)(strtab + sym->name);
		if (strcmp(name, "memcmp") != 0)
			continue;
		slot = l_addr + rel[i].offset;
		page = slot & ~0xfffUL;
		prot = region_prot(maps, slot);
		if (prot < 0)
			continue;
		if (mprotect((void *)page, 0x1000, PROT_READ | PROT_WRITE) != 0)
			continue;
		*(unsigned int *)(uintptr_t)slot =
			(unsigned int)(uintptr_t)stub_memcmp;
		mprotect((void *)page, 0x1000, prot);
		hits++;
	}
	return hits;
}

/* Redirect the executable's memcmp GOT slot(s) to stub_memcmp().  Returns
 * the number of slots patched, or -1 when the executable cannot be parsed. */
static int patch_memcmp_got(const char *exe)
{
	static char maps[32768];
	elf32_ehdr *eh;
	elf32_phdr *ph;
	elf32_dyn *dyn;
	unsigned long base, first_load = 0, dyn_vaddr = 0, l_addr;
	unsigned long strtab = 0, symtab = 0, jmprel = 0, pltrelsz = 0;
	unsigned long rel = 0, relsz = 0;
	unsigned int pltrel = DT_REL;
	size_t off = 0;
	ssize_t n;
	int fd, i, have_load = 0, hits = 0;

	fd = open("/proc/self/maps", O_RDONLY);
	if (fd < 0)
		return -1;
	while (off < sizeof(maps) - 1 &&
	       (n = read(fd, maps + off, sizeof(maps) - 1 - off)) > 0)
		off += (size_t)n;
	close(fd);
	if (off == 0)
		return -1;
	maps[off] = '\0';

	base = exe_map_lowest(maps, exe);
	if (base == 0)
		return -1;

	eh = (elf32_ehdr *)(uintptr_t)base;
	if (eh->ident[0] != 0x7f || eh->ident[1] != 'E' ||
	    eh->ident[2] != 'L' || eh->ident[3] != 'F')
		return -1;

	ph = (elf32_phdr *)(uintptr_t)(base + eh->phoff);
	for (i = 0; i < eh->phnum; i++) {
		if (ph[i].type == PT_LOAD &&
		    (!have_load || ph[i].vaddr < first_load)) {
			first_load = ph[i].vaddr;
			have_load = 1;
		}
		if (ph[i].type == PT_DYNAMIC)
			dyn_vaddr = ph[i].vaddr;
	}
	if (!have_load || dyn_vaddr == 0)
		return -1;
	l_addr = base - (first_load & ~0xfffUL);

	dyn = (elf32_dyn *)(uintptr_t)(l_addr + dyn_vaddr);
	for (i = 0; dyn[i].tag != DT_NULL && i < 128; i++) {
		switch (dyn[i].tag) {
		case DT_STRTAB:
			strtab = l_addr + dyn[i].val;
			break;
		case DT_SYMTAB:
			symtab = l_addr + dyn[i].val;
			break;
		case DT_JMPREL:
			jmprel = l_addr + dyn[i].val;
			break;
		case DT_PLTRELSZ:
			pltrelsz = dyn[i].val;
			break;
		case DT_PLTREL:
			pltrel = dyn[i].val;
			break;
		case DT_REL:
			rel = l_addr + dyn[i].val;
			break;
		case DT_RELSZ:
			relsz = dyn[i].val;
			break;
		}
	}
	if (symtab == 0 || strtab == 0)
		return -1;

	if (pltrel == DT_REL && jmprel != 0 && pltrelsz != 0)
		hits += patch_rel_table(jmprel, pltrelsz, symtab, strtab,
					l_addr, maps);
	if (rel != 0 && relsz != 0)
		hits += patch_rel_table(rel, relsz, symtab, strtab, l_addr,
					maps);
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

	if (verbose_log())
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
	} else if (strcmp(base, "loader") == 0) {
		int hits = patch_memcmp_got(exe);

		console_log("[ldpreload] loader: memcmp GOT patched (%d slot%s)\n",
			    hits, hits == 1 ? "" : "s");
	}
}
