#ifndef EFIBOOT_CONFIG_H
#define EFIBOOT_CONFIG_H

/*
 * config.h - \BOOTKIT.CFG: the partition RouterOS is installed on, and the
 * boot-time "debug=" switch (serial console + verbose tracer/probe logs).
 * The target may also be the magic value "auto": keep the loader where it is
 * and boot the first RouterOS kernel found (the bootkit-auto.img stick).
 */

#include "efi.h"

#define CONFIG_PATH	L"\\BOOTKIT.CFG"
#define CONFIG_MAX	512

/* "target=auto": no install and nothing written - find_kernel_root() scans
 * the partitions and boots the first RouterOS kernel it can identify */
#define CONFIG_TARGET_AUTO	"auto"

/*
 * Identity of an EFI system partition: the fields of its HD() device path
 * node.  Bus topology is deliberately not part of it, so the same partition
 * is found again when the disk is plugged into another port.
 */
typedef struct {
	UINT8 type;		/* EFI_HD_SIGNATURE_MBR or _GPT */
	UINT8 sig[16];		/* 4 bytes for MBR, 16 for GPT */
	UINT64 start;		/* partition start, 512-byte blocks */
	UINT64 size;		/* partition size, 512-byte blocks */
} TARGET_ID;

BOOLEAN target_id_from_dp(EFI_DEVICE_PATH_PROTOCOL *dp, TARGET_ID *id);
BOOLEAN target_id_equal(const TARGET_ID *a, const TARGET_ID *b);
EFI_STATUS config_read(EFI_FILE_PROTOCOL *root, TARGET_ID *id, BOOLEAN *auto_mode,
		       BOOLEAN *debug);
EFI_STATUS config_write(EFI_FILE_PROTOCOL *root, const TARGET_ID *id,
			EFI_DEVICE_PATH_PROTOCOL *dp, BOOLEAN debug);
EFI_STATUS find_target_root(const TARGET_ID *id, EFI_FILE_PROTOCOL **out);

/*
 * target=auto: the arch-specific probe tells whether a volume holds a
 * bootable RouterOS kernel (boot.c / boot_arm64.c); find_kernel_root() opens
 * every volume the firmware exposes and returns the first one it accepts.
 */
typedef BOOLEAN (*KERNEL_PROBE)(EFI_FILE_PROTOCOL *root);
EFI_STATUS find_kernel_root(KERNEL_PROBE probe, EFI_FILE_PROTOCOL **out);

#endif /* EFIBOOT_CONFIG_H */
