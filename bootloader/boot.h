#ifndef EFIBOOT_BOOT_H
#define EFIBOOT_BOOT_H

/* boot.h - load the kernel from an ESP and enter it via EFI handover. */

#include "efi.h"

EFI_STATUS boot_from_root(EFI_HANDLE image, EFI_FILE_PROTOCOL *root);

#endif /* EFIBOOT_BOOT_H */
