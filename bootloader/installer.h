#ifndef EFIBOOT_INSTALLER_H
#define EFIBOOT_INSTALLER_H

/* installer.h - the install menu (direct / removable). */

#include "efi.h"

EFI_STATUS installer_run(EFI_HANDLE image, EFI_LOADED_IMAGE_PROTOCOL *li);

#endif /* EFIBOOT_INSTALLER_H */
