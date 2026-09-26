/*
 * installer.c - the install menu: list the EFI partitions, ask where
 * RouterOS is installed, ask for the mode (direct/removable) and do it.
 */
#include "efi.h"
#include "config.h"
#include "vars.h"
#include "installer.h"

/* file path of our own loaded image (e.g. \EFI\BOOT\BOOTKIT.EFI) */
static EFI_STATUS self_file_path(EFI_LOADED_IMAGE_PROTOCOL *li, CHAR16 **path)
{
	EFI_DEVICE_PATH_PROTOCOL *node = li->FilePath, *fp = 0;

	if (!node)
		return EFI_NOT_FOUND;
	while (!dp_is_end(node)) {
		if (dp_is_filepath(node))
			fp = node;
		node = dp_next(node);
	}
	if (!fp)
		return EFI_NOT_FOUND;
	*path = (CHAR16 *)((UINT8 *)fp + 4);
	return EFI_SUCCESS;
}

EFI_STATUS installer_run(EFI_HANDLE image, EFI_LOADED_IMAGE_PROTOCOL *li)
{
	EFI_STATUS status;
	EFI_FILE_PROTOCOL *root, *f, *dir;
	EFI_FILE_SYSTEM_INFO *fsi;
	EFI_HANDLE *handles = 0;
	UINTN count = 0, volumes = 0, i, choice = 0, mode = 0;
	CHAR16 *self_path;
	UINT8 info[560];
	UINTN info_size;
	UINT64 size64;
	UINTN src_len;
	VOID *src = 0;
	EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *d2t = 0;
	TARGET_ID target_id;
	EFI_DEVICE_PATH_PROTOCOL *target_dp = 0;

	print(L"efiboot: installer mode\r\n");
	print(L"efiboot: debug logging is ");
	print(BOOTKIT_DEBUG_DEFAULT ? L"on" : L"off");
	print(L" (edit debug= in " CONFIG_PATH L" to change)\r\n");

	if (!ST->ConIn)
		return fail(EFI_UNSUPPORTED, L"no console input available");

	status = self_file_path(li, &self_path);
	if (EFI_ERROR(status))
		return fail(status, L"cannot determine my own path");

	/* read ourselves so we can write the same image elsewhere */
	status = open_root(image, &root);
	if (EFI_ERROR(status))
		return fail(status, L"cannot open my own volume");
	status = open_file(root, self_path, &f);
	if (EFI_ERROR(status))
		return fail(status, L"cannot open my own file");
	status = file_size(f, &size64);
	if (EFI_ERROR(status)) {
		f->Close(f);
		return fail(status, L"cannot size my own file");
	}
	src_len = (UINTN)size64;
	status = alloc_pages(EfiLoaderData, src_len, 0xffffffffULL, &src);
	if (EFI_ERROR(status))
		return fail(status, L"cannot allocate memory");
	status = read_file(f, 0, src, src_len);
	f->Close(f);
	if (EFI_ERROR(status))
		return fail(status, L"cannot read my own file");

	/* list the volumes that look like EFI system partitions */
	status = BS->LocateHandleBuffer(EFI_BY_PROTOCOL, &simple_file_system_guid,
					0, &count, &handles);
	if (EFI_ERROR(status))
		return fail(status, L"cannot enumerate file systems");
	BS->LocateProtocol(&device_path_to_text_guid, 0, (VOID **)&d2t);

	print(L"efiboot: EFI partitions:\r\n");
	for (i = 0; i < count; i++) {
		EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
		EFI_DEVICE_PATH_PROTOCOL *dp = 0;

		if (EFI_ERROR(BS->HandleProtocol(handles[i], &simple_file_system_guid,
						 (VOID **)&fs)))
			continue;
		if (EFI_ERROR(fs->OpenVolume(fs, &root)))
			continue;
		if (EFI_ERROR(root->Open(root, &dir, L"\\EFI", EFI_FILE_MODE_READ, 0))) {
			root->Close(root);
			continue;
		}
		dir->Close(dir);
		dir = 0;

		fsi = 0;
		info_size = sizeof(info);
		if (!EFI_ERROR(root->GetInfo(root, &file_system_info_guid, &info_size, info)))
			fsi = (EFI_FILE_SYSTEM_INFO *)info;
		print(L"efiboot:   ");
		print_dec(volumes + 1);
		print(L") ");
		if (fsi && fsi->VolumeLabel[0])
			print_trunc(fsi->VolumeLabel, 32);
		else
			print(L"(no label)");
		print(L", ");
		if (fsi)
			print_dec(fsi->VolumeSize / (1024 * 1024));
		else
			print_ch(L'?');
		print(L" MB");
		if (handles[i] == li->DeviceHandle)
			print(L", this installer");
		print(L"\r\n");
		if (d2t && !EFI_ERROR(BS->HandleProtocol(handles[i], &device_path_guid,
							 (VOID **)&dp))) {
			CHAR16 *text = d2t->ConvertDevicePathToText(dp, FALSE, TRUE);

			if (text) {
				print(L"efiboot:      ");
				print_trunc(text, 68);
				print(L"\r\n");
				BS->FreePool(text);
			}
		}
		root->Close(root);
		root = 0;
		handles[volumes++] = handles[i];
	}
	if (!volumes) {
		BS->FreePool(handles);
		return fail(EFI_NOT_FOUND, L"no EFI partitions found");
	}

	/* ask the user where RouterOS is installed */
	for (;;) {
		print(L"efiboot: select the EFI partition where RouterOS is installed [1-");
		print_dec(volumes);
		print(L"] (q to cancel): ");
		status = read_number(&choice);
		print(L"\r\n");
		if (status == EFI_ABORTED) {
			print(L"efiboot: cancelled\r\n");
			return EFI_ABORTED;
		}
		if (EFI_ERROR(status))
			return status;
		if (choice >= 1 && choice <= volumes)
			break;
		print(L"efiboot: invalid selection\r\n");
	}

	/* ask for the installation mode */
	print(L"efiboot: installation mode:\r\n");
	print(L"efiboot:   1) direct: copy the loader + config to the target and create the boot entry\r\n");
	print(L"efiboot:   2) removable: keep the loader here, write only the config (target stays untouched)\r\n");
	for (;;) {
		print(L"efiboot: select mode [1-2] (q to cancel): ");
		status = read_number(&mode);
		print(L"\r\n");
		if (status == EFI_ABORTED) {
			print(L"efiboot: cancelled\r\n");
			return EFI_ABORTED;
		}
		if (EFI_ERROR(status))
			return status;
		if (mode == 1 || mode == 2)
			break;
		print(L"efiboot: invalid selection\r\n");
	}

	/* open the target volume and take its partition identity */
	{
		EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
		EFI_DEVICE_PATH_PROTOCOL *dp = 0;

		if (EFI_ERROR(BS->HandleProtocol(handles[choice - 1], &simple_file_system_guid,
						 (VOID **)&fs)) ||
		    EFI_ERROR(fs->OpenVolume(fs, &root)))
			return fail(EFI_DEVICE_ERROR, L"cannot open the selected partition");
		if (EFI_ERROR(BS->HandleProtocol(handles[choice - 1], &device_path_guid,
						 (VOID **)&dp)) ||
		    !target_id_from_dp(dp, &target_id))
			return fail(EFI_NOT_FOUND, L"cannot identify the selected partition");
		target_dp = dp;
	}

	if (mode == 1) {
		/* direct install: loader + config on the target, boot entry for it */
		if (EFI_ERROR(root->Open(root, &dir, L"\\EFI",
					 EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
					 EFI_FILE_DIRECTORY)))
			return fail(EFI_DEVICE_ERROR, L"cannot open \\EFI");
		dir->Close(dir);
		if (EFI_ERROR(root->Open(root, &dir, L"\\EFI\\BOOT",
					 EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
					 EFI_FILE_DIRECTORY)))
			return fail(EFI_DEVICE_ERROR, L"cannot open \\EFI\\BOOT");
		dir->Close(dir);

		status = write_file(root, L"\\EFI\\BOOT\\BOOTKIT.EFI", src, src_len);
		if (EFI_ERROR(status))
			return fail(status, L"cannot write \\EFI\\BOOT\\BOOTKIT.EFI");
		status = open_file(root, L"\\EFI\\BOOT\\BOOTKIT.EFI", &f);
		if (EFI_ERROR(status))
			return fail(status, L"cannot reopen the installed file");
		status = file_size(f, &size64);
		f->Close(f);
		if (EFI_ERROR(status) || size64 != src_len)
			return fail(EFI_DEVICE_ERROR, L"installed file size mismatch");

		status = config_write(root, &target_id, target_dp, BOOTKIT_DEBUG_DEFAULT);
		if (EFI_ERROR(status))
			return fail(status, L"cannot write " CONFIG_PATH);
		status = boot_entry_add(handles[choice - 1], L"\\EFI\\BOOT\\BOOTKIT.EFI");
		if (EFI_ERROR(status))
			return fail(status, L"cannot create the boot entry");
		print(L"efiboot: direct install done, copied ");
		print_dec(src_len);
		print(L" bytes to partition ");
		print_dec(choice);
		print(L"\r\n");
	} else {
		/* removable: only the config, next to the loader that is running */
		EFI_FILE_PROTOCOL *cur_root;

		status = open_root(image, &cur_root);
		if (EFI_ERROR(status))
			return fail(status, L"cannot open my own volume");
		status = config_write(cur_root, &target_id, target_dp, BOOTKIT_DEBUG_DEFAULT);
		cur_root->Close(cur_root);
		if (EFI_ERROR(status))
			return fail(status, L"cannot write " CONFIG_PATH);
		print(L"efiboot: removable install done, " CONFIG_PATH L" points at partition ");
		print_dec(choice);
		print(L"\r\n");
	}

	if (root)
		root->Close(root);
	BS->FreePool(handles);
	print(L"efiboot: press any key to reboot\r\n");
	wait_key();
	RS->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, 0);
	return EFI_SUCCESS;
}
