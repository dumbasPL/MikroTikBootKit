#ifndef EFIBOOT_CONFIG_H
#define EFIBOOT_CONFIG_H

/*
 * config.h - \BOOTKIT.CFG: the partition RouterOS is installed on, and the
 * boot-time "debug=" switch (serial console + verbose tracer/probe logs).
 */

#include "efi.h"

#define CONFIG_PATH	L"\\BOOTKIT.CFG"
#define CONFIG_MAX	512

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
EFI_STATUS config_read(EFI_FILE_PROTOCOL *root, TARGET_ID *id, BOOLEAN *debug);
EFI_STATUS config_write(EFI_FILE_PROTOCOL *root, const TARGET_ID *id,
			EFI_DEVICE_PATH_PROTOCOL *dp, BOOLEAN debug);
EFI_STATUS find_target_root(const TARGET_ID *id, EFI_FILE_PROTOCOL **out);

#endif /* EFIBOOT_CONFIG_H */
