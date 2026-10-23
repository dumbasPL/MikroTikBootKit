/*
 * preload.c - LD_PRELOAD probe for RouterOS (i386)
 *
 * Ordinary C, but linked without libc (-nostdlib, see the Makefile): the shared
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
 *   /nova/bin/mode    the embedded keygen (keygen.c) generates and signs the
 *                     licence blob
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
 * The in-memory patch is the runtime equivalent of the key replacement the
 * reference patch does to the binaries.  The encoding differs per arch:
 *
 *   x86        the 32-byte public key is eight `mov dword [ebp-x], imm32`
 *              immediates in .text; the compiler may emit them in any order
 *              (7.25 swaps chunks 5 and 6), so the eight chunks are matched
 *              as a set inside a small window
 *   arm32      most of the key sits in a .text literal pool and the missing
 *              word is derived by an add/sub sequence; both the pool order and
 *              the instruction sequence are compiler output, so the words are
 *              matched as a set and the derived word is located by its
 *              arithmetic (the arm64 CHR's userspace is AArch32, armv7
 *              soft-float)
 *
 * The chunks are replaced in place and the bytes of the instructions in
 * between are left alone.  The affected pages are made writable for the
 * patch and restored to r-x afterwards; only the private (COW) text mapping
 * is touched, so the file on disk is untouched.
 *
 * The key pair is hard-coded in keygen.c and can be overridden at build time
 * (-DKEYGEN_LICENSE_*, from CUSTOM_LICENSE_PUBLIC_KEY /
 * CUSTOM_LICENSE_PRIVATE_KEY in the build environment); signing and patching
 * use the same pair, so the licence this probe signs verifies against the key
 * it patches in.  Built by the Makefile into preload.so;
 * embedded in ptrace_init.
 */

#define _GNU_SOURCE

/*
 * The embedded keygen: keygen.c is the standalone RouterOS keygen trimmed to
 * the generation role (no CLI, no mode2 hand-over, no
 * exit()); its kg_generate()/kg_error() are called below.  The licence key
 * pair is selected with -DKEYGEN_LICENSE_PUBLIC_HEX /
 * -DKEYGEN_LICENSE_PRIVATE_HEX (see the Makefile).
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

/* The stock RouterOS licence public key; the Makefile can override it with
 * -DSTOCK_LICENSE_PUBLIC_HEX from MIKRO_LICENSE_PUBLIC_KEY. */
#ifndef STOCK_LICENSE_PUBLIC_HEX
#define STOCK_LICENSE_PUBLIC_HEX \
	"8E1067E4305FCDC0CFBF95C10F96E5DFE8C49AEF486BD1A4E2E96C27F01E3E32"
#endif

/* the eight key chunks must all sit inside this window; x86 compilers put the
 * immediate loads back to back, even when they schedule them out of order */
#define KEY_SPAN_MAX 256
#define KEY_CHUNKS   8

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

#if !defined(__arm__)

/* Find all eight chunks of oldk in the window around the chunk0 occurrence at
 * anchor.  ends[] is indexed by chunk number and receives each chunk's offset.
 *
 * The x86 verifier loads the key with eight independent mov-immediate
 * instructions.  The compiler may schedule those in any order: 7.25 emits
 * chunks 5 and 6 swapped, so the ordered greedy search used to miss the key
 * even though the resulting on-stack key is identical.  Look for every chunk
 * at its own offset inside a small window instead.
 *
 * Returns 1 when the complete key was found, 0 otherwise. */
static int match_key(const unsigned char *buf, size_t len, size_t anchor,
		     const unsigned char *oldk, size_t *ends)
{
	int seen[KEY_CHUNKS] = { 0 };
	size_t lo, hi, p, first, last;
	int n = 0, k;

	lo = anchor > KEY_SPAN_MAX ? anchor - KEY_SPAN_MAX : 0;
	hi = anchor + KEY_SPAN_MAX + 4;
	if (hi > len)
		hi = len;
	first = anchor;
	last = anchor + 4;
	for (p = lo; p + 4 <= hi; p++) {
		for (k = 0; k < KEY_CHUNKS; k++) {
			if (seen[k] || memcmp(buf + p, oldk + 4 * k, 4) != 0)
				continue;
			seen[k] = 1;
			ends[k] = p;
			if (p < first)
				first = p;
			if (p + 4 > last)
				last = p + 4;
			n++;
			p += 3;
			break;
		}
	}
	return n == KEY_CHUNKS && last - first <= KEY_SPAN_MAX;
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
		if (!match_key(buf, len, i, oldk, ends)) {
			i++;
			continue;
		}
		end = i + 4;
		for (c = 0; c < KEY_CHUNKS; c++) {
			memcpy(buf + ends[c], newk + 4 * c, 4);
			if (ends[c] + 4 > end)
				end = ends[c] + 4;
		}
		hits++;
		i = end;
	}
	return hits;
}

#else /* __arm__ */

/*
 * The arm32 builds (the routeros arm64 CHR userspace is AArch32) materialise
 * the key with a literal pool plus an instruction sequence.  The full-word
 * form stores seven of the eight words and derives the fourth from one of
 * them; the pool order and the instruction block are compiler output and are
 * not stable across RouterOS builds, so the words are matched as a set and the
 * derived word is located by its arithmetic instead of a fixed instruction
 * pattern.
 */

#define ARM_WORDS      8
#define ARM_SPAN_MAX   128	/* window holding one literal pool */
#define ARM_CODE_BACK  512	/* instructions before the pool */
#define ARM_CODE_FWD   256	/* instructions after the pool */
#define ARM_MAX_INSNS  256
#define ARM_MAX_PATCH  16

struct arm_insn {
	size_t off;
	unsigned int word;
};

struct arm_step {
	size_t off;
	unsigned int imm;
	int sub;
};

struct arm_patch {
	size_t off;
	unsigned int word;
};

static unsigned int arm_rd32(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
	       ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void arm_wr32(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

/* rotate the 8-bit immediate the way an ARM data-processing immediate does */
static unsigned int arm_ror(unsigned int v, unsigned int shift)
{
	shift &= 31;
	if (shift == 0)
		return v;
	return (v >> shift) | (v << (32 - shift));
}

/* ADD/SUB <reg>, <reg>, #imm, unconditional and without flags */
static int arm_add_sub(unsigned int w, int *reg, unsigned int *imm, int *sub)
{
	unsigned int op;

	if ((w >> 28) != 0xE || (w & 0x0E000000u) != 0x02000000u ||
	    (w & (1u << 20)) != 0)
		return 0;
	op = (w >> 21) & 0xf;
	if (op != 2 && op != 4)
		return 0;
	*reg = (int)((w >> 12) & 0xf);
	if (((w >> 16) & 0xf) != (unsigned int)*reg)
		return 0;
	*imm = arm_ror(w & 0xff, 2 * ((w >> 8) & 0xf));
	*sub = (op == 2);
	return 1;
}

static int arm_movw(unsigned int w, int *reg, unsigned int *imm)
{
	if ((w >> 28) != 0xE || (w & 0x0ff00000u) != 0x03000000u)
		return 0;
	*reg = (int)((w >> 12) & 0xf);
	*imm = (((w >> 16) & 0xf) << 12) | (w & 0xfff);
	return 1;
}

static int arm_movt(unsigned int w, int *reg, unsigned int *imm)
{
	if ((w >> 28) != 0xE || (w & 0x0ff00000u) != 0x03400000u)
		return 0;
	*reg = (int)((w >> 12) & 0xf);
	*imm = (((w >> 16) & 0xf) << 12) | (w & 0xfff);
	return 1;
}

/* LDR <reg>, [pc, #imm] */
static int arm_ldr_literal(unsigned int w, size_t off, int *reg, size_t *target)
{
	int imm;
	long t;

	if ((w >> 28) != 0xE || (w & 0x0f7f0000u) != 0x051f0000u)
		return 0;
	imm = (int)(w & 0xfff);
	if ((w & (1u << 23)) == 0)
		imm = -imm;
	t = (long)off + 8 + imm;
	if (t < 0)
		return 0;
	*reg = (int)((w >> 12) & 0xf);
	*target = (size_t)t;
	return 1;
}

/* STR <reg>, [sp, #imm] */
static int arm_str_sp(unsigned int w, int *reg)
{
	if ((w >> 28) != 0xE ||
	    ((w & 0x0ff00000u) != 0x05800000u &&
	     (w & 0x0ff00000u) != 0x05000000u))
		return 0;
	if (((w >> 16) & 0xf) != 13)
		return 0;
	*reg = (int)((w >> 12) & 0xf);
	return 1;
}

static unsigned int arm_movw_word(int reg, unsigned int imm)
{
	return 0xE3000000u | ((imm >> 12) & 0xfu) << 16 |
	       (unsigned int)reg << 12 | (imm & 0xfffu);
}

static unsigned int arm_movt_word(int reg, unsigned int imm)
{
	return 0xE3400000u | ((imm >> 12) & 0xfu) << 16 |
	       (unsigned int)reg << 12 | (imm & 0xfffu);
}

/* find pat[0..patlen) in buf[lo..hi), returning its offset in at */
static int arm_find_bytes(const unsigned char *buf, size_t len, size_t lo,
			  size_t hi, const unsigned char *pat, size_t patlen,
			  size_t *at)
{
	size_t p;

	if (hi > len)
		hi = len;
	if (lo > hi || patlen > hi - lo)
		return 0;
	for (p = lo; p + patlen <= hi; p++) {
		if (memcmp(buf + p, pat, patlen) == 0) {
			*at = p;
			return 1;
		}
	}
	return 0;
}

/* Find a window that holds all words in indices, in any order.  ends[] is
 * indexed by key word and receives the offset of each word. */
static int arm_find_table(const unsigned char *buf, size_t len,
			  const unsigned char *oldk, const int *indices,
			  int n, size_t *ends)
{
	const unsigned char *anchor = oldk + 4 * indices[0];
	size_t pos = 0, at;

	while (arm_find_bytes(buf, len, pos, len, anchor, 4, &at)) {
		size_t lo = at > ARM_SPAN_MAX ? at - ARM_SPAN_MAX : 0;
		size_t hi = at + ARM_SPAN_MAX + 4;
		size_t first = at, last = at + 4;
		int i, ok = 1;

		ends[indices[0]] = at;
		for (i = 1; i < n; i++) {
			if (!arm_find_bytes(buf, len, lo, hi,
					    oldk + 4 * indices[i], 4,
					    &ends[indices[i]])) {
				ok = 0;
				break;
			}
			if (ends[indices[i]] < first)
				first = ends[indices[i]];
			if (ends[indices[i]] + 4 > last)
				last = ends[indices[i]] + 4;
		}
		if (ok && last - first <= ARM_SPAN_MAX)
			return 1;
		pos = at + 1;
	}
	return 0;
}

/* The chain must start from a table word and the result must be written to
 * the stack right after it; that ties the arithmetic to the key buffer. */
static int arm_chain_matches(const struct arm_insn *insns, int n, int reg,
			     size_t base_off, size_t chain_start,
			     size_t chain_end)
{
	size_t store_off = (size_t)-1;
	int i, loaded = 0;

	for (i = 0; i < n; i++) {
		int r;
		size_t target;

		if (insns[i].off >= chain_start ||
		    chain_start - insns[i].off > 128)
			continue;
		if (arm_ldr_literal(insns[i].word, insns[i].off, &r, &target) &&
		    r == reg && target == base_off)
			loaded = 1;
	}
	if (!loaded)
		return 0;

	for (i = 0; i < n; i++) {
		int r;

		if (insns[i].off <= chain_end ||
		    insns[i].off - chain_end > 64)
			continue;
		if (arm_str_sp(insns[i].word, &r) && r == reg) {
			store_off = insns[i].off;
			break;
		}
	}
	if (store_off == (size_t)-1)
		return 0;

	for (i = 0; i < n; i++) {
		int r, sub;
		unsigned int imm;

		if (insns[i].off <= chain_end || insns[i].off >= store_off)
			continue;
		if (arm_add_sub(insns[i].word, &r, &imm, &sub) && r == reg)
			return 0;
	}
	return 1;
}

/* Find the instructions that compute the missing word and rewrite them to
 * load newk's word directly.  Returns the number of patches, or 0. */
static int arm_patch_derived(const unsigned char *buf, size_t len,
			     const unsigned char *oldk, const unsigned char *newk,
			     const size_t *word_offsets, int derived,
			     struct arm_patch *patches, int max_patches)
{
	static struct arm_insn insns[ARM_MAX_INSNS];
	static struct arm_step steps[ARM_MAX_INSNS];
	unsigned int old_word = arm_rd32(oldk + 4 * derived);
	unsigned int new_word = arm_rd32(newk + 4 * derived);
	size_t min_off = len, max_off = 0, start, end;
	int i, nins = 0;

	for (i = 0; i < ARM_WORDS; i++) {
		if (word_offsets[i] == (size_t)-1)
			continue;
		if (word_offsets[i] < min_off)
			min_off = word_offsets[i];
		if (word_offsets[i] + 4 > max_off)
			max_off = word_offsets[i] + 4;
	}
	if (min_off == len)
		return 0;

	start = min_off > ARM_CODE_BACK ? min_off - ARM_CODE_BACK : 0;
	start &= ~(size_t)3;
	end = max_off + ARM_CODE_FWD;
	if (end > len)
		end = len;
	for (; start + 4 <= end && nins < ARM_MAX_INSNS; start += 4) {
		/* the literal pool is data, not instructions */
		if (start >= min_off && start < max_off)
			continue;
		insns[nins].off = start;
		insns[nins].word = arm_rd32(buf + start);
		nins++;
	}

	/* MOVW/MOVT pair */
	for (i = 0; i + 1 < nins; i++) {
		int r1, r2;
		unsigned int lo, hi;

		if (!arm_movw(insns[i].word, &r1, &lo) ||
		    lo != (old_word & 0xffff))
			continue;
		if (!arm_movt(insns[i + 1].word, &r2, &hi) ||
		    r2 != r1 || hi != (old_word >> 16))
			continue;
		if (max_patches < 2)
			return 0;
		patches[0].off = insns[i].off;
		patches[0].word = arm_movw_word(r1, new_word & 0xffff);
		patches[1].off = insns[i + 1].off;
		patches[1].word = arm_movt_word(r1, new_word >> 16);
		return 2;
	}

	for (i = 0; i < 16; i++) {
		int reg = i, nsteps = 0, a, b;

		for (b = 0; b < nins; b++) {
			int r, sub;
			unsigned int imm;

			if (!arm_add_sub(insns[b].word, &r, &imm, &sub) ||
			    r != reg)
				continue;
			if (nsteps >= ARM_MAX_INSNS)
				break;
			steps[nsteps].off = insns[b].off;
			steps[nsteps].imm = imm;
			steps[nsteps].sub = sub;
			nsteps++;
		}
		for (a = 0; a + 1 < nsteps; a++) {
			unsigned int delta = 0;
			int j;

			for (j = a; j < nsteps && j < a + 4; j++) {
				if (steps[j].sub)
					delta -= steps[j].imm;
				else
					delta += steps[j].imm;
				if (j == a)
					continue;
				for (b = 0; b < ARM_WORDS; b++) {
					int np = j - a + 1, k;

					if (word_offsets[b] == (size_t)-1)
						continue;
					if (arm_rd32(oldk + 4 * b) + delta != old_word)
						continue;
					if (!arm_chain_matches(insns, nins, reg,
							       word_offsets[b],
							       steps[a].off,
							       steps[j].off))
						continue;
					if (np > max_patches)
						return 0;
					patches[0].off = steps[a].off;
					patches[0].word = arm_movw_word(reg,
								       new_word & 0xffff);
					patches[1].off = steps[a + 1].off;
					patches[1].word = arm_movt_word(reg,
								       new_word >> 16);
					for (k = a + 2; k <= j; k++) {
						patches[k - a].off = steps[k].off;
						patches[k - a].word = 0xE1A00000u;
					}
					return np;
				}
			}
		}
	}
	return 0;
}

/* Patch one table; *next is the offset just past the last patched byte. */
static int arm_patch_table(unsigned char *buf, size_t len,
			   const unsigned char *oldk, const unsigned char *newk,
			   size_t *next)
{
	static const int all[ARM_WORDS] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	static const int stored[7] = { 4, 5, 2, 0, 1, 6, 7 };
	struct arm_patch patches[ARM_MAX_PATCH];
	size_t ends[ARM_WORDS], last = 0;
	int i, np;

	for (i = 0; i < ARM_WORDS; i++)
		ends[i] = (size_t)-1;
	if (arm_find_table(buf, len, oldk, all, ARM_WORDS, ends)) {
		for (i = 0; i < ARM_WORDS; i++) {
			arm_wr32(buf + ends[i], arm_rd32(newk + 4 * i));
			if (ends[i] + 4 > last)
				last = ends[i] + 4;
		}
		*next = last;
		return 1;
	}

	for (i = 0; i < ARM_WORDS; i++)
		ends[i] = (size_t)-1;
	if (!arm_find_table(buf, len, oldk, stored, 7, ends))
		return 0;
	np = arm_patch_derived(buf, len, oldk, newk, ends, 3,
			       patches, ARM_MAX_PATCH);
	if (np <= 0)
		return 0;
	for (i = 0; i < 7; i++) {
		int idx = stored[i];

		arm_wr32(buf + ends[idx], arm_rd32(newk + 4 * idx));
		if (ends[idx] + 4 > last)
			last = ends[idx] + 4;
	}
	for (i = 0; i < np; i++) {
		arm_wr32(buf + patches[i].off, patches[i].word);
		if (patches[i].off + 4 > last)
			last = patches[i].off + 4;
	}
	*next = last;
	return 1;
}

/* Replace every arm32-style copy of oldk in buf with newk.  Returns the number
 * of sites patched. */
static int replace_key(unsigned char *buf, size_t len,
		       const unsigned char *oldk, const unsigned char *newk)
{
	size_t off = 0;
	int hits = 0;

	while (off + 4 <= len) {
		size_t next = 0;

		if (!arm_patch_table(buf + off, len - off, oldk, newk, &next))
			break;
		if (next == 0)
			break;
		hits++;
		off += next;
	}
	return hits;
}

#endif /* !__arm__ */

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

		if (hits < 0)
			console_log("[ldpreload] mode: cannot patch the licence key\n");
		else
			console_log("[ldpreload] mode: licence key patched (%d site%s)\n",
				    hits, hits == 1 ? "" : "s");
		run_embedded_keygen();
	} else if (strcmp(base, "keyman") == 0) {
		int hits = patch_licence_key(exe);

		if (hits < 0)
			console_log("[ldpreload] keyman: cannot patch the licence key\n");
		else
			console_log("[ldpreload] keyman: licence key patched (%d site%s)\n",
				    hits, hits == 1 ? "" : "s");
	} else if (strcmp(base, "loader") == 0) {
		int hits = patch_memcmp_got(exe);

		if (hits < 0)
			console_log("[ldpreload] loader: cannot patch the memcmp GOT slot\n");
		else
			console_log("[ldpreload] loader: memcmp GOT patched (%d slot%s)\n",
				    hits, hits == 1 ? "" : "s");
	}
}
