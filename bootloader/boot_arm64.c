/*
 * boot_arm64.c - load \EFI\BOOT\BOOTAA64.EFI (the stock RouterOS arm64
 * kernel) from an ESP and start it with the initramfs embedded in this
 * image.
 *
 * The AArch64 Linux kernel is a PE/COFF EFI application, so this boots it
 * with LoadImage/StartImage: the kernel's own EFI stub keeps the EFI runtime
 * environment RouterOS's init expects (the same reason the x86 loader uses
 * the EFI handover entry instead of a raw jump).  The command line goes
 * through the loaded image's LoadOptions.
 *
 * Passing the initramfs is the arch-specific part.  The 5.6 stub has no
 * LoadFile2 initrd support and the boot is ACPI (the stub prints "Generating
 * empty DTB"), so the DT route is out as well.  What it does support is
 * "initrd=<file>" on the command line: handle_cmdline_files() opens that
 * file on the kernel image's own device handle and adds linux,initrd-start/
 * end to the device tree it generates.  The loader installs the initramfs as
 * a one-file RAM volume (initrdvol.c) and points the loaded image's
 * DeviceHandle at it, so nothing has to be written to the target ESP.
 *
 * The stock initramfs is part of the kernel: the CHR arm64 Image carries it
 * in .data as an XZ stream (the stage-1 /init, /dev/console, /dev/ram0)
 * rather than as an unpacked __initramfs blob.  A kernel-supplied initrd
 * replaces it - the stock /init would be missing from the rootfs - so the
 * loader finds that stream in the kernel it just read and hands the kernel
 * "[stock XZ stream][our cpio]" as one initrd.  Linux's initramfs unpacker
 * takes concatenated archives and detects the compression of each segment,
 * so both the stock rootfs and our /ptrace_init appear; no decompressor is
 * needed in the loader, the XZ stream is located by its magic and validated
 * through the CRC32 fields in its header and footer.
 *
 * Kernel command line: rdinit=/ptrace_init, the initrd file name and the
 * serial console (the CHR console is ttyAMA0 - the QEMU virt PL011 - and the
 * tracer/probe log to /dev/console); debug=1 only adds the bootkit_debug
 * marker.  Linux does not know an "initrd=" parameter of its own, it is
 * consumed by the stub (and ignored as an unknown option afterwards).
 */
#include "efi.h"
#include "boot.h"
#include "initrdvol.h"
#ifndef INITRD_SO_HEADER
#define INITRD_SO_HEADER "initrd_so.h"
#endif
#include INITRD_SO_HEADER

/* compile-time configuration */
#define KERNEL_PATH		L"\\EFI\\BOOT\\BOOTAA64.EFI"
#define KERNEL_FILE		"initrd.cpio"
#define KERNEL_CMDLINE_BASE	"rdinit=/ptrace_init initrd=" KERNEL_FILE \
				" console=ttyAMA0,115200n8"
#define KERNEL_CMDLINE_DEBUG	" bootkit_debug=1"

/* ---- the kernel's built-in initramfs ----------------------------------- */

static const UINT8 xz_magic[6] = { 0xfd, 0x37, 0x7a, 0x58, 0x5a, 0x00 };

/* CRC-32/ISO-HDLC (the one .xz uses), with pre/post inversion done here */
static UINT32 crc32_iso(UINT32 crc, const UINT8 *p, UINTN n)
{
	UINTN i;
	int k;

	for (i = 0; i < n; i++) {
		crc ^= p[i];
		for (k = 0; k < 8; k++)
			crc = (crc & 1) ? (crc >> 1) ^ 0xedb88320U : crc >> 1;
	}
	return crc ^ 0xffffffffU;
}

static UINT32 rd32le(const UINT8 *p)
{
	return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) |
	       ((UINT32)p[3] << 24);
}

/* Is there a complete .xz stream starting at buf[0]?  Returns its length
 * (0 when the header or the footer does not check out). */
static UINTN xz_stream_len(const UINT8 *buf, UINTN len)
{
	UINTN i;

	if (len < 24 || memcmp(buf, xz_magic, sizeof(xz_magic)) != 0)
		return 0;
	/* the header CRC32 covers the 2 stream flag bytes after the magic */
	if (crc32_iso(0xffffffffU, buf + 6, 2) != rd32le(buf + 8))
		return 0;
	/* scan for the footer: <crc32><backward size><flags>YZ */
	for (i = 15; i + 2 <= len; i++) {
		if (buf[i] != 'Y' || buf[i + 1] != 'Z')
			continue;
		if (buf[i - 2] != 0)		/* reserved byte */
			continue;
		if (buf[i - 1] > 15)		/* check id: none/crc32/crc64/sha256 */
			continue;
		if (rd32le(buf + i - 10) !=
		    crc32_iso(0xffffffffU, buf + i - 6, 6))
			continue;
		return i + 2;
	}
	return 0;
}

/* Find the stock initramfs inside the kernel image. */
static BOOLEAN find_builtin_initrd(const UINT8 *kbuf, UINTN klen,
				   UINTN *off, UINTN *len)
{
	UINTN i, n;

	for (i = 0; i + 12 < klen; i++) {
		if (kbuf[i] != xz_magic[0] || kbuf[i + 1] != xz_magic[1] ||
		    kbuf[i + 2] != xz_magic[2] || kbuf[i + 3] != xz_magic[3] ||
		    kbuf[i + 4] != xz_magic[4] || kbuf[i + 5] != xz_magic[5])
			continue;
		n = xz_stream_len(kbuf + i, klen - i);
		if (n < 1024)		/* too small to be an initramfs */
			continue;
		*off = i;
		*len = n;
		return TRUE;
	}
	return FALSE;
}

/* ---- command line and console ------------------------------------------ */

/* base + optional debug options, NUL terminated; returns bytes incl. NUL */
static UINTN build_cmdline(char *buf, UINTN cap, BOOLEAN debug)
{
	const char *s;
	UINTN n = 0;

	for (s = KERNEL_CMDLINE_BASE; *s && n < cap - 1; s++)
		buf[n++] = *s;
	if (debug)
		for (s = KERNEL_CMDLINE_DEBUG; *s && n < cap - 1; s++)
			buf[n++] = *s;
	buf[n] = 0;
	return n + 1;
}

/* ASCII -> UTF-16 (EFI LoadOptions), len bytes including the NUL */
static VOID ascii_to_utf16(const char *s, CHAR16 *w, UINTN len)
{
	UINTN i;

	for (i = 0; i < len; i++)
		w[i] = (CHAR16)(UINT8)s[i];
}

static VOID print_kernel_version(const UINT8 *kbuf, UINTN klen)
{
	static const char needle[] = "Linux version ";
	CHAR16 buf[128];
	UINTN i, j, n = klen < 262144 ? klen : 262144;

	for (i = 0; i + sizeof(needle) - 1 < n; i++) {
		if (memcmp(kbuf + i, needle, sizeof(needle) - 1) != 0)
			continue;
		for (j = 0; j < sizeof(buf) / sizeof(buf[0]) - 1 &&
			    i + j < n && kbuf[i + j] != '\n' && kbuf[i + j]; j++)
			buf[j] = (CHAR16)kbuf[i + j];
		if (!j)
			return;
		buf[j] = 0;
		print(L"efiboot: kernel ");
		print(buf);
		print(L"\r\n");
		return;
	}
}

/* ---- boot -------------------------------------------------------------- */

EFI_STATUS boot_from_root(EFI_HANDLE image, EFI_FILE_PROTOCOL *root,
			  BOOLEAN debug)
{
	EFI_FILE_PROTOCOL *f;
	EFI_STATUS status;
	EFI_HANDLE khandle = 0, volhandle = 0;
	EFI_LOADED_IMAGE_PROTOCOL *kli;
	UINT8 *kbuf = 0, *ibuf;
	const UINT8 *initrd;
	UINTN initrd_len, blen = 0, boff = 0, pad;
	UINT64 size64, klen;
	UINTN cmd_len, cmd_chars;
	char cmdline[128];
	CHAR16 cmdw[128];

	/* -- kernel: read \EFI\BOOT\BOOTAA64.EFI into memory -- */

	status = open_file(root, KERNEL_PATH, &f);
	if (EFI_ERROR(status))
		return fail(status, L"cannot open " KERNEL_PATH);
	status = file_size(f, &size64);
	if (EFI_ERROR(status)) {
		f->Close(f);
		return fail(status, L"cannot size the kernel");
	}
	klen = size64;
	if (klen < 1024 * 1024 || klen > 64 * 1024 * 1024) {
		f->Close(f);
		return fail(EFI_LOAD_ERROR, L"the kernel file looks wrong");
	}
	/* the image stays alive until StartImage, so it is not freed */
	status = alloc_pages(EfiLoaderData, (UINTN)klen, ~(EFI_PHYSICAL_ADDRESS)0,
			     (VOID **)&kbuf);
	if (EFI_ERROR(status)) {
		f->Close(f);
		return fail(status, L"cannot allocate kernel memory");
	}
	status = read_file(f, 0, kbuf, (UINTN)klen);
	f->Close(f);
	if (EFI_ERROR(status))
		return fail(status, L"cannot load the kernel");

	print_kernel_version(kbuf, (UINTN)klen);

	/* -- initramfs: the stock one from the kernel + ours -- */

	if (initrd_cpio_len == 0)
		return fail(EFI_LOAD_ERROR, L"embedded initrd is empty");
	initrd = initrd_cpio;
	initrd_len = initrd_cpio_len;
	if (find_builtin_initrd(kbuf, (UINTN)klen, &boff, &blen)) {
		pad = (4 - (blen & 3)) & 3;
		initrd_len = blen + pad + initrd_cpio_len;
		status = alloc_pages(EfiLoaderData, initrd_len,
				     ~(EFI_PHYSICAL_ADDRESS)0, (VOID **)&ibuf);
		if (EFI_ERROR(status))
			return fail(status, L"cannot allocate the initrd");
		memcpy(ibuf, kbuf + boff, blen);
		memset(ibuf + blen, 0, pad);
		memcpy(ibuf + blen + pad, initrd_cpio, initrd_cpio_len);
		initrd = ibuf;
	} else {
		print(L"efiboot: no kernel initramfs found, only the kit's\r\n");
	}
	status = initrdvol_create(initrd, initrd_len, &volhandle);
	if (EFI_ERROR(status))
		return fail(status, L"cannot create the initrd volume");

	/* -- LoadImage the kernel, point it at the initrd and the cmdline -- */

	status = BS->LoadImage(FALSE, image, 0, kbuf, (UINTN)klen, &khandle);
	if (EFI_ERROR(status))
		return fail(status, L"cannot load the kernel image");
	status = BS->HandleProtocol(khandle, &loaded_image_protocol_guid,
				    (VOID **)&kli);
	if (EFI_ERROR(status))
		return fail(status, L"no loaded image protocol");

	cmd_len = build_cmdline(cmdline, sizeof(cmdline), debug);
	cmd_chars = cmd_len * sizeof(CHAR16);
	if (cmd_chars > sizeof(cmdw))
		return fail(EFI_LOAD_ERROR, L"the command line is too long");
	ascii_to_utf16(cmdline, cmdw, cmd_len);
	kli->LoadOptions = cmdw;
	kli->LoadOptionsSize = (UINT32)cmd_chars;
	kli->DeviceHandle = volhandle;		/* initrd= is read from here */

	print(L"efiboot: initrd ");
	print_dec(initrd_len);
	print(L" bytes (stock ");
	print_dec(blen);
	print(L" + kit ");
	print_dec(initrd_cpio_len);
	print(L"), booting (");
	print(cmdw);
	print(L")\r\n");

	/* on success the kernel takes over and this never returns */
	status = BS->StartImage(khandle, 0, 0);
	return fail(status, L"the kernel did not start");
}
