/*
 * vars.c - EFI variable helpers: find or create the Boot#### entry for the
 * installed loader and put it first in BootOrder.
 */
#include "efi.h"
#include "vars.h"

static VOID boot_var_name(CHAR16 *name, UINT16 index)
{
	static const CHAR16 digits[] = L"0123456789ABCDEF";

	name[0] = L'B';
	name[1] = L'o';
	name[2] = L'o';
	name[3] = L't';
	name[4] = digits[(index >> 12) & 0xf];
	name[5] = digits[(index >> 8) & 0xf];
	name[6] = digits[(index >> 4) & 0xf];
	name[7] = digits[index & 0xf];
	name[8] = 0;
}

static BOOLEAN boot_var_index(const CHAR16 *name, UINT16 *index)
{
	UINTN i, v = 0;

	if (str_len16(name) != 8)
		return FALSE;
	if (name[0] != L'B' || name[1] != L'o' || name[2] != L'o' || name[3] != L't')
		return FALSE;
	for (i = 4; i < 8; i++) {
		CHAR16 c = name[i];

		if (c >= L'0' && c <= L'9')
			v = v * 16 + (c - L'0');
		else if (c >= L'A' && c <= L'F')
			v = v * 16 + (c - L'A' + 10);
		else if (c >= L'a' && c <= L'f')
			v = v * 16 + (c - L'a' + 10);
		else
			return FALSE;
	}
	*index = (UINT16)v;
	return TRUE;
}

static EFI_STATUS read_variable(CHAR16 *name, EFI_GUID *guid, UINT32 *attrs,
				VOID **data, UINTN *size)
{
	UINT8 stack[256];
	UINTN cap = sizeof(stack);
	EFI_STATUS status;

	status = RS->GetVariable(name, guid, attrs, &cap, stack);
	if (status == EFI_BUFFER_TOO_SMALL) {
		*data = pool_alloc(cap);
		if (!*data)
			return EFI_OUT_OF_RESOURCES;
		status = RS->GetVariable(name, guid, attrs, &cap, *data);
		if (EFI_ERROR(status)) {
			BS->FreePool(*data);
			*data = 0;
			return status;
		}
		*size = cap;
		return EFI_SUCCESS;
	}
	if (status == EFI_NOT_FOUND) {
		*data = 0;
		*size = 0;
		return status;
	}
	if (EFI_ERROR(status))
		return status;
	if (cap == 0) {
		*data = 0;
		*size = 0;
		return EFI_SUCCESS;
	}
	*data = pool_alloc(cap);
	if (!*data)
		return EFI_OUT_OF_RESOURCES;
	memcpy(*data, stack, cap);
	*size = cap;
	return EFI_SUCCESS;
}

EFI_STATUS boot_entry_add(EFI_HANDLE vol_handle, CHAR16 *file_path)
{
	EFI_DEVICE_PATH_PROTOCOL *vol_dp = 0, *full = 0;
	EFI_STATUS status;
	CHAR16 *name = 0, *desc = L"MikroTikBootKit";
	EFI_GUID guid;
	UINTN full_len, name_size, name_cap = 4096;
	UINTN desc_len = str_len16(desc) + 1;
	UINT16 index = 0xffff, max_index = 0xffff, i;
	BOOLEAN found = FALSE;
	VOID *data = 0, *order = 0, *new_order = 0;
	UINTN data_size = 0, order_size = 0, entries = 0;
	UINT32 attrs = 0, order_attrs = EFI_VARIABLE_NON_VOLATILE |
				      EFI_VARIABLE_BOOTSERVICE_ACCESS |
				      EFI_VARIABLE_RUNTIME_ACCESS;

	status = BS->HandleProtocol(vol_handle, &device_path_guid, (VOID **)&vol_dp);
	if (EFI_ERROR(status))
		return status;
	full = dp_with_file(vol_dp, file_path);
	if (!full)
		return EFI_OUT_OF_RESOURCES;
	full_len = dp_nodes_len(full) + 4;

	name = pool_alloc_zero(name_cap);
	if (!name) {
		status = EFI_OUT_OF_RESOURCES;
		goto out;
	}

	/* look for an entry that already points at this file, and the next free id */
	memset(&guid, 0, sizeof(guid));
	for (;;) {
		name_size = name_cap;
		status = RS->GetNextVariableName(&name_size, name, &guid);
		if (status == EFI_NOT_FOUND) {
			status = EFI_SUCCESS;
			break;
		}
		if (EFI_ERROR(status))
			goto out;
		if (!boot_var_index(name, &i))
			continue;
		if (max_index == 0xffff || i > max_index)
			max_index = i;
		status = read_variable(name, &global_variable_guid, &attrs, &data, &data_size);
		if (EFI_ERROR(status) || data_size < 6)
			continue;
		{
			UINT16 fplen = *(UINT16 *)((UINT8 *)data + 4);
			CHAR16 *d = (CHAR16 *)((UINT8 *)data + 6);
			UINTN dlen = 0;
			UINT8 *fplist;

			while (dlen < data_size && d[dlen])
				dlen++;
			fplist = (UINT8 *)(d + dlen + 1);
			if (fplen == full_len &&
			    (UINTN)(fplist - (UINT8 *)data) + fplen <= data_size &&
			    memcmp(fplist, full, full_len) == 0) {
				index = i;
				found = TRUE;
			}
			if (data)
				BS->FreePool(data);
			data = 0;
			if (found)
				break;
		}
	}
	if (!found) {
		index = (max_index == 0xffff) ? 0 : (UINT16)(max_index + 1);
	}

	/* EFI_LOAD_OPTION: attributes, FilePathListLength, description, path list */
	{
		UINTN opt_size = 4 + 2 + desc_len * 2 + full_len;
		UINT8 *opt = pool_alloc_zero(opt_size);

		if (!opt) {
			status = EFI_OUT_OF_RESOURCES;
			goto out;
		}
		*(UINT32 *)opt = EFI_LOAD_OPTION_ACTIVE;
		*(UINT16 *)(opt + 4) = (UINT16)full_len;
		memcpy(opt + 6, desc, desc_len * 2);
		memcpy(opt + 6 + desc_len * 2, full, full_len);
		boot_var_name(name, index);
		status = RS->SetVariable(name, &global_variable_guid,
					 EFI_VARIABLE_NON_VOLATILE |
					 EFI_VARIABLE_BOOTSERVICE_ACCESS |
					 EFI_VARIABLE_RUNTIME_ACCESS,
					 opt_size, opt);
		BS->FreePool(opt);
		if (EFI_ERROR(status))
			goto out;
	}

	/* BootOrder: drop the entry if present, then prepend it */
	status = read_variable(L"BootOrder", &global_variable_guid, &order_attrs,
			       &order, &order_size);
	if (EFI_ERROR(status) && status != EFI_NOT_FOUND)
		goto out;
	entries = order_size / sizeof(UINT16);
	new_order = pool_alloc_zero((entries + 1) * sizeof(UINT16));
	if (!new_order) {
		status = EFI_OUT_OF_RESOURCES;
		goto out;
	}
	((UINT16 *)new_order)[0] = index;
	{
		UINTN k = 1;

		for (i = 0; i < entries; i++)
			if (((UINT16 *)order)[i] != index)
				((UINT16 *)new_order)[k++] = ((UINT16 *)order)[i];
		status = RS->SetVariable(L"BootOrder", &global_variable_guid,
					 order_attrs, k * sizeof(UINT16), new_order);
		if (EFI_ERROR(status))
			goto out;
	}

	print(L"efiboot: boot entry Boot");
	print_hex4(index);
	print(L" -> \\EFI\\BOOT\\BOOTKIT.EFI\r\n");

out:
	if (full)
		BS->FreePool(full);
	if (name)
		BS->FreePool(name);
	if (order)
		BS->FreePool(order);
	if (new_order)
		BS->FreePool(new_order);
	return status;
}
