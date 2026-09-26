/*
 * efi_main.c - the loader entry point: read \BOOTKIT.CFG from the volume we
 * were booted from, boot the configured RouterOS ESP (passing its debug=
 * setting on to the kernel command line), or run the install menu when there
 * is no usable config (--install forces the menu).
 */
#include "efi.h"
#include "config.h"
#include "installer.h"
#include "boot.h"

static BOOLEAN load_options_have(EFI_LOADED_IMAGE_PROTOCOL *li, const CHAR16 *token)
{
	UINTN i, n = li->LoadOptionsSize / sizeof(CHAR16);
	CHAR16 *opt = li->LoadOptions;

	if (!opt)
		return FALSE;
	for (i = 0; i + str_len16(token) <= n; i++) {
		if (str_equal_ci(opt + i, token))
			return TRUE;
	}
	return FALSE;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	EFI_LOADED_IMAGE_PROTOCOL *li = 0;
	EFI_FILE_PROTOCOL *root;
	TARGET_ID target;
	BOOLEAN debug;
	EFI_STATUS status;

	ST = st;
	BS = st->BootServices;
	RS = st->RuntimeServices;

	print(L"efiboot: MikroTik boot kit loader\r\n");

	if (EFI_ERROR(BS->HandleProtocol(image, &loaded_image_protocol_guid,
					  (VOID **)&li)))
		return fail(EFI_LOAD_ERROR, L"no loaded image protocol");

	/* explicit "start the install menu" override */
	if (load_options_have(li, L"--install"))
		return installer_run(image, li);

	/*
	 * Normal case: the config next to the loader names the partition
	 * RouterOS is installed on.  Boot \EFI\BOOT\BOOTX64.EFI from there.
	 * No config (or one that does not point at a partition we can find)
	 * means the kit was not set up yet: run the install menu.
	 */
	status = open_root(image, &root);
	if (EFI_ERROR(status))
		return fail(status, L"cannot open the boot volume");
	status = config_read(root, &target, &debug);
	if (EFI_ERROR(status)) {
		print(L"efiboot: no valid " CONFIG_PATH L" here, starting the installer\r\n");
	} else {
		EFI_FILE_PROTOCOL *target_root;

		status = find_target_root(&target, &target_root);
		if (EFI_ERROR(status))
			print(L"efiboot: the configured target partition was not found\r\n");
		else
			return boot_from_root(image, target_root, debug);
	}
	return installer_run(image, li);
}
