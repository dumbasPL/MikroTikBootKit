#ifndef EFIBOOT_BOOT_H
#define EFIBOOT_BOOT_H

/* boot.h - load the stock kernel from an ESP and enter it: boot.c is the
 * x86_64 EFI handover implementation, boot_arm64.c the AArch64
 * LoadImage/StartImage one. */

#include "efi.h"

EFI_STATUS boot_from_root(EFI_HANDLE image, EFI_FILE_PROTOCOL *root, BOOLEAN debug);

#endif /* EFIBOOT_BOOT_H */
