#ifndef EFIBOOT_BOOTABI_H
#define EFIBOOT_BOOTABI_H

/*
 * bootabi.h - the x86 Linux boot protocol pieces the loader needs: the
 * bzImage setup header, struct boot_params and the EFI handover entry type
 * (Documentation/x86/boot.rst).
 */

#include "efi.h"

/* ---- x86 boot protocol structures -------------------------------------- */

struct SetupHeader {
	UINT8 setup_sects;
	UINT16 root_flags;
	UINT32 syssize;
	UINT16 ram_size;
	UINT16 vid_mode;
	UINT16 root_dev;
	UINT16 boot_flag;
	UINT8 jump;
	UINT8 setup_size;
	UINT32 header;
	UINT16 version;
	UINT32 realmode_swtch;
	UINT16 start_sys_seg;
	UINT16 kernel_version;
	UINT8 type_of_loader;
	UINT8 loadflags;
	UINT16 setup_move_size;
	UINT32 code32_start;
	UINT32 ramdisk_image;
	UINT32 ramdisk_size;
	UINT32 bootsect_kludge;
	UINT16 heap_end_ptr;
	UINT8 ext_loader_ver;
	UINT8 ext_loader_type;
	UINT32 cmd_line_ptr;
	UINT32 initrd_addr_max;
	UINT32 kernel_alignment;
	UINT8 relocatable_kernel;
	UINT8 min_alignment;
	UINT16 xloadflags;
	UINT32 cmdline_size;
	UINT32 hardware_subarch;
	UINT64 hardware_subarch_data;
	UINT32 payload_offset;
	UINT32 payload_length;
	UINT64 setup_data;
	UINT64 pref_address;
	UINT32 init_size;
	UINT32 handover_offset;
	UINT32 kernel_info_offset;
} __attribute__((packed));

#define SETUP_HDR_OFFSET	0x1f1
#define SETUP_HDR_MAGIC		0x53726448	/* "HdrS" */
#define BOOT_FLAG_MAGIC		0xaa55
#define SETUP_VERSION_2_11	0x20b
#define SETUP_VERSION_2_12	0x20c

#define XLF_KERNEL_64			(1 << 0)
#define XLF_CAN_BE_LOADED_ABOVE_4G	(1 << 1)
#define XLF_EFI_HANDOVER_32		(1 << 2)
#define XLF_EFI_HANDOVER_64		(1 << 3)

struct BootParams {
	UINT8 _pad0[0xc0];
	UINT32 ext_ramdisk_image;
	UINT32 ext_ramdisk_size;
	UINT32 ext_cmd_line_ptr;
	UINT8 _pad1[SETUP_HDR_OFFSET - 0xcc];
	struct SetupHeader hdr;
	UINT8 _pad2[4096 - SETUP_HDR_OFFSET - sizeof(struct SetupHeader)];
} __attribute__((packed));

_Static_assert(__builtin_offsetof(struct BootParams, ext_ramdisk_image) == 0xc0, "boot_params layout");
_Static_assert(__builtin_offsetof(struct BootParams, ext_cmd_line_ptr) == 0xc8, "boot_params layout");
_Static_assert(__builtin_offsetof(struct BootParams, hdr) == SETUP_HDR_OFFSET, "boot_params layout");
_Static_assert(sizeof(struct BootParams) == 4096, "boot_params layout");
_Static_assert(__builtin_offsetof(struct SetupHeader, header) == 0x11, "setup header layout");
_Static_assert(__builtin_offsetof(struct SetupHeader, code32_start) == 0x23, "setup header layout");
_Static_assert(__builtin_offsetof(struct SetupHeader, handover_offset) == 0x73, "setup header layout");

/* the handover entry point is kernel code: SysV ABI, not the EFI ABI */
typedef void (*handover_fn)(VOID *handle, EFI_SYSTEM_TABLE *st,
			    struct BootParams *params) __attribute__((sysv_abi));

#endif /* EFIBOOT_BOOTABI_H */
