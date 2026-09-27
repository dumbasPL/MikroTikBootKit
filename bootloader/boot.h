#ifndef EFIBOOT_BOOT_H
#define EFIBOOT_BOOT_H

/* boot.h - load the stock kernel from an ESP and enter it: boot.c is the
 * x86_64 EFI handover implementation, boot_arm64.c the AArch64
 * LoadImage/StartImage one. */

#include "efi.h"

EFI_STATUS boot_from_root(EFI_HANDLE image, EFI_FILE_PROTOCOL *root, BOOLEAN debug);

/*
 * \BOOTKIT.CFG "target=auto": scan the volumes for the first bootable
 * RouterOS kernel and boot that one.  Returns EFI_NOT_FOUND when there is
 * none (efi_main() then runs the install menu).
 */
EFI_STATUS boot_auto(EFI_HANDLE image, BOOLEAN debug);

#endif /* EFIBOOT_BOOT_H */
