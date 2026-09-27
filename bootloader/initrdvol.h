#ifndef EFIBOOT_INITRDVOL_H
#define EFIBOOT_INITRDVOL_H

/*
 * initrdvol.h - a RAM-backed EFI volume serving the loader's embedded
 * initramfs.
 *
 * The arm64 Linux 5.6 EFI stub loads the file named by "initrd=" on the
 * command line from the kernel image's own device handle (efi_open_volume()
 * in the stub).  The loader points that handle at this volume, so the cpio
 * embedded in the loader is handed to the kernel without writing anything to
 * disk; RouterOS 5.6 has no LoadFile2 initrd support and the boot is ACPI
 * (no device tree to put linux,initrd-start in).
 */

#include "efi.h"

/* Install the volume serving data[0..len) and return its handle. */
EFI_STATUS initrdvol_create(const UINT8 *data, UINTN len, EFI_HANDLE *out);

#endif /* EFIBOOT_INITRDVOL_H */
