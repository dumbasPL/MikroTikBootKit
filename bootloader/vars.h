#ifndef EFIBOOT_VARS_H
#define EFIBOOT_VARS_H

/* vars.h - EFI variable helpers used by the installer. */

#include "efi.h"

EFI_STATUS boot_entry_add(EFI_HANDLE vol_handle, CHAR16 *file_path);

#endif /* EFIBOOT_VARS_H */
