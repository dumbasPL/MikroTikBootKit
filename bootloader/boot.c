/*
 * boot.c - load \EFI\BOOT\BOOTX64.EFI from an ESP and enter it through the
 * x86 EFI handover protocol with the initramfs embedded in this image.  The
 * kernel command line is "rdinit=/ptrace_init", plus the serial console and
 * the bootkit_debug marker when the config's debug= is on.
 */
#include "efi.h"
#include "bootabi.h"
#include "boot.h"
#include "initrd_so.h"

/* compile-time configuration; the serial console and the verbose tracer/probe
 * logs are enabled by "debug=1" in \BOOTKIT.CFG (default BOOTKIT_DEBUG_DEFAULT,
 * set by DEBUG=1 ./build.sh) */
#define KERNEL_PATH		L"\\EFI\\BOOT\\BOOTX64.EFI"
#define KERNEL_CMDLINE_BASE	"rdinit=/ptrace_init"
#define KERNEL_CMDLINE_DEBUG	" console=ttyS0,115200n8 bootkit_debug=1"

/* kept out of the stack frame: >4 KiB frames need a __chkstk probe */
static UINT8 first_page[4096];

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

static VOID print_kernel_version(const UINT8 *kbuf, UINTN klen)
{
	const struct SetupHeader *h = (const struct SetupHeader *)(kbuf + SETUP_HDR_OFFSET);
	UINT64 off = (UINT64)h->kernel_version + 0x200;
	CHAR16 buf[128];
	UINTN i = 0;

	if (h->kernel_version == 0 || off >= klen)
		return;
	while (i < sizeof(buf) / sizeof(buf[0]) - 1 && off + i < klen && kbuf[off + i]) {
		buf[i] = (CHAR16)kbuf[off + i];
		i++;
	}
	buf[i] = 0;
	if (i) {
		print(L"efiboot: kernel ");
		print(buf);
		print(L"\r\n");
	}
}

EFI_STATUS boot_from_root(EFI_HANDLE image, EFI_FILE_PROTOCOL *root, BOOLEAN debug)
{
	struct SetupHeader hdr;
	struct BootParams *params;
	EFI_FILE_PROTOCOL *f;
	EFI_PHYSICAL_ADDRESS addr;
	EFI_STATUS status;
	UINTN setup_sects, setup_total, kalloc, cmd_len;
	UINT64 kfile_size, kneed, klen, ilen;
	UINT8 *kbuf = 0, *ibuf = 0;
	char cmdline[96];
	CHAR16 *cmd;
	handover_fn handover;

	/* -- kernel: read the first page, validate, then load it below 4G -- */

	status = open_file(root, KERNEL_PATH, &f);
	if (EFI_ERROR(status))
		return fail(status, L"cannot open " KERNEL_PATH);

	status = file_size(f, &klen);
	if (EFI_ERROR(status))
		return fail(status, L"cannot size the kernel");
	if (klen < sizeof(first_page))
		return fail(EFI_LOAD_ERROR, L"kernel file is too small");
	status = read_file(f, 0, first_page, sizeof(first_page));
	if (EFI_ERROR(status))
		return fail(status, L"cannot read the kernel");

	memcpy(&hdr, first_page + SETUP_HDR_OFFSET, sizeof(hdr));
	if (hdr.boot_flag != BOOT_FLAG_MAGIC || hdr.header != SETUP_HDR_MAGIC)
		return fail(EFI_LOAD_ERROR, L"not a Linux bzImage");
	if (hdr.version < SETUP_VERSION_2_11)
		return fail(EFI_UNSUPPORTED, L"kernel is too old for EFI handover");
	if (!hdr.relocatable_kernel)
		return fail(EFI_UNSUPPORTED, L"kernel is not relocatable");
	if (!(hdr.xloadflags & XLF_KERNEL_64) ||
	    !(hdr.xloadflags & XLF_EFI_HANDOVER_64))
		return fail(EFI_UNSUPPORTED, L"kernel has no 64-bit EFI handover entry");
	if (!hdr.syssize || !hdr.init_size)
		return fail(EFI_LOAD_ERROR, L"kernel image sizes are invalid");

	setup_sects = hdr.setup_sects ? hdr.setup_sects : 4;
	setup_total = (setup_sects + 1) * 512;
	kfile_size = setup_total + (UINT64)hdr.syssize * 16;
	if (kfile_size > klen)
		return fail(EFI_LOAD_ERROR, L"kernel image is truncated");

	/*
	 * The EFI stub copies hdr.init_size bytes starting at the protected-mode
	 * entry when it relocates the kernel, so reserve that much even when the
	 * file itself is smaller; the tail stays zero.
	 */
	kneed = setup_total + hdr.init_size;
	kalloc = (UINTN)(kneed > kfile_size ? kneed : kfile_size);
	status = alloc_pages(EfiLoaderCode, kalloc, 0xffffffffULL, (VOID **)&kbuf);
	if (EFI_ERROR(status))
		return fail(status, L"cannot allocate kernel memory");
	memset(kbuf, 0, kalloc);
	status = read_file(f, 0, kbuf, (UINTN)kfile_size);
	f->Close(f);
	if (EFI_ERROR(status))
		return fail(status, L"cannot load the kernel");

	print_kernel_version(kbuf, kalloc);

	/* -- initrd: copy the embedded cpio below initrd_addr_max (0x7fffffff) -- */

	ilen = initrd_cpio_len;
	if (ilen == 0)
		return fail(EFI_LOAD_ERROR, L"embedded initrd is empty");
	status = alloc_pages(EfiLoaderData, (UINTN)ilen, 0x7fff0000ULL, (VOID **)&ibuf);
	if (EFI_ERROR(status))
		return fail(status, L"cannot allocate initrd memory");
	memcpy(ibuf, initrd_cpio, (UINTN)ilen);

	/* -- boot_params -- */

	status = alloc_pages(EfiLoaderData, sizeof(*params), 0xffffffffULL, (VOID **)&params);
	if (EFI_ERROR(status))
		return fail(status, L"cannot allocate boot params");
	memset(params, 0, sizeof(*params));

	/*
	 * The kernel's 2-byte short jump at 0x200 lands right after its setup
	 * header, so its displacement byte plus the 0x11 bytes before it is
	 * the length of the header the file carries (123 bytes for the 2.15+
	 * layout these kernels use).  Fields the file does not have stay zero.
	 */
	{
		UINTN hdr_len = 0x11 + hdr.jump_disp;

		if (hdr_len > sizeof(struct SetupHeader))
			hdr_len = sizeof(struct SetupHeader);
		memcpy(&params->hdr, kbuf + SETUP_HDR_OFFSET, hdr_len);
	}
	params->hdr.type_of_loader = 0xff;
	params->hdr.code32_start = (UINT32)(UINTN)(kbuf + setup_total);	/* 32-bit entry */
	params->hdr.ramdisk_image = (UINT32)(UINTN)ibuf;
	params->hdr.ramdisk_size = (UINT32)ilen;
	params->ext_ramdisk_image = (UINT32)((UINT64)(UINTN)ibuf >> 32);
	params->ext_ramdisk_size = (UINT32)(ilen >> 32);

	cmd = (CHAR16 *)0;
	cmd_len = build_cmdline(cmdline, sizeof(cmdline), debug);
	status = alloc_pages(EfiLoaderData, cmd_len, 0xa0000ULL, (VOID **)&cmd);
	if (EFI_ERROR(status))
		status = alloc_pages(EfiLoaderData, cmd_len, 0xffffffffULL,
				    (VOID **)&cmd);
	if (EFI_ERROR(status))
		return fail(status, L"cannot allocate the command line");
	memcpy(cmd, cmdline, cmd_len);
	params->hdr.cmd_line_ptr = (UINT32)(UINTN)cmd;
	params->ext_cmd_line_ptr = (UINT32)((UINT64)(UINTN)cmd >> 32);

	print(L"efiboot: initrd ");
	print_dec(ilen);
	print(L" bytes, booting (");
	{
		CHAR16 cmdw[96];
		UINTN ci;

		for (ci = 0; cmdline[ci] && ci < sizeof(cmdw) / sizeof(cmdw[0]) - 1; ci++)
			cmdw[ci] = (CHAR16)cmdline[ci];
		cmdw[ci] = 0;
		print(cmdw);
	}
	print(L")\r\n");

	/*
	 * EFI handover entry: 32-bit handover_offset from the start of the
	 * protected-mode code and another 0x200 for the 64-bit entry.  The
	 * kernel's EFI stub takes over from here (interrupts must be off).
	 */
	addr = (EFI_PHYSICAL_ADDRESS)(UINTN)kbuf + setup_total;
	handover = (handover_fn)(UINTN)(addr + 0x200 + hdr.handover_offset);
	__asm__ volatile ("cli");
	handover(image, ST, params);
	return EFI_LOAD_ERROR;	/* not reached */
}
