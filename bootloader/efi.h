#ifndef EFIBOOT_EFI_H
#define EFIBOOT_EFI_H

/*
 * efi.h - the minimal EFI subset the bootloader uses: types, protocols, GUIDs
 * and the helper prototypes implemented in efi.c.  Freestanding: no libc, no
 * gnu-efi (the loader is built with -nostdlib for the Windows target).
 */

typedef unsigned char		UINT8;
typedef unsigned short		UINT16;
typedef unsigned int		UINT32;
typedef unsigned long long	UINT64;
typedef signed short		INT16;
typedef signed long long	INT64;
typedef unsigned long long	UINTN;	/* LLP64: long is 32-bit on Windows targets */
typedef UINT16			CHAR16;
typedef UINT8			BOOLEAN;
typedef void			VOID;

#define TRUE 1
#define FALSE 0

/*
 * Boot-time debug logging and serial console (see \BOOTKIT.CFG "debug=").
 * DEBUG=1 ./build.sh changes the default for newly written configs and for
 * boots with a config that has no debug= line; production builds leave it 0.
 */
#ifndef BOOTKIT_DEBUG_DEFAULT
#define BOOTKIT_DEBUG_DEFAULT 0
#endif

/*
 * The UEFI calling convention.  x86_64 UEFI uses the Microsoft ABI, so the
 * freestanding Windows target gets ms_abi; AArch64 UEFI uses the standard
 * AAPCS64 (edk2's Base.h defines EFIAPI empty there too).
 */
#if defined(__x86_64__) || defined(_M_X64)
#define EFIAPI __attribute__((ms_abi))
#else
#define EFIAPI
#endif
#define EFI_PAGE_SIZE 4096
#define EFI_SIZE_TO_PAGES(n) (((n) + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE)

typedef UINTN EFI_STATUS;
typedef VOID *EFI_HANDLE;
typedef VOID *EFI_EVENT;
typedef UINT64 EFI_PHYSICAL_ADDRESS;
typedef UINT64 EFI_VIRTUAL_ADDRESS;
typedef UINTN EFI_TPL;

#define EFI_ERROR(s) ((INT64)(s) < 0)
#define EFI_SUCCESS		0
#define EFI_LOAD_ERROR		(1ULL | (1ULL << 63))
#define EFI_INVALID_PARAMETER	(2ULL | (1ULL << 63))
#define EFI_UNSUPPORTED		(3ULL | (1ULL << 63))
#define EFI_BUFFER_TOO_SMALL	(5ULL | (1ULL << 63))
#define EFI_DEVICE_ERROR	(7ULL | (1ULL << 63))
#define EFI_OUT_OF_RESOURCES	(9ULL | (1ULL << 63))
#define EFI_NOT_FOUND		(14ULL | (1ULL << 63))
#define EFI_ABORTED		(21ULL | (1ULL << 63))

#define EFI_FILE_MODE_READ	0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE	0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE	0x8000000000000000ULL
#define EFI_FILE_DIRECTORY	0x0000000000000010ULL
#define EFI_FILE_PROTOCOL_REVISION 0x00010000
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION 0x00010000
#define EFI_NATIVE_INTERFACE	0

#define EFI_VARIABLE_NON_VOLATILE	0x00000001
#define EFI_VARIABLE_BOOTSERVICE_ACCESS	0x00000002
#define EFI_VARIABLE_RUNTIME_ACCESS	0x00000004
#define EFI_LOAD_OPTION_ACTIVE		0x00000001

#define EfiResetCold		0

typedef enum {
	AllocateAnyPages = 0,
	AllocateMaxAddress = 1,
	AllocateAddress = 2
} EFI_ALLOCATE_TYPE;

typedef enum {
	EfiReservedMemoryType = 0,
	EfiLoaderCode = 1,
	EfiLoaderData = 2,
	EfiBootServicesCode = 3,
	EfiBootServicesData = 4,
	EfiRuntimeServicesCode = 5,
	EfiRuntimeServicesData = 6,
	EfiConventionalMemory = 7
} EFI_MEMORY_TYPE;

typedef struct {
	UINT32 Data1;
	UINT16 Data2;
	UINT16 Data3;
	UINT8 Data4[8];
} EFI_GUID;

typedef struct {
	UINT64 Signature;
	UINT32 Revision;
	UINT32 HeaderSize;
	UINT32 CRC32;
	UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
	EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, BOOLEAN);
	EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, CHAR16 *);
	EFI_STATUS (EFIAPI *TestString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, CHAR16 *);
	EFI_STATUS (EFIAPI *QueryMode)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN, UINTN *, UINTN *);
	EFI_STATUS (EFIAPI *SetMode)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN);
	EFI_STATUS (EFIAPI *SetAttribute)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN);
	EFI_STATUS (EFIAPI *ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *);
	EFI_STATUS (EFIAPI *SetCursorPosition)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN, UINTN);
	EFI_STATUS (EFIAPI *EnableCursor)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, BOOLEAN);
	VOID *Mode;
};

typedef struct {
	UINT16 ScanCode;
	CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
	EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, BOOLEAN);
	EFI_STATUS (EFIAPI *ReadKeyStroke)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, EFI_INPUT_KEY *);
	EFI_EVENT WaitForKey;
};

typedef struct EFI_RUNTIME_SERVICES EFI_RUNTIME_SERVICES;
struct EFI_RUNTIME_SERVICES {
	EFI_TABLE_HEADER Hdr;
	VOID *GetTime;
	VOID *SetTime;
	VOID *GetWakeupTime;
	VOID *SetWakeupTime;
	VOID *SetVirtualAddressMap;
	VOID *ConvertPointer;
	EFI_STATUS (EFIAPI *GetVariable)(CHAR16 *, EFI_GUID *, UINT32 *, UINTN *, VOID *);
	EFI_STATUS (EFIAPI *GetNextVariableName)(UINTN *, CHAR16 *, EFI_GUID *);
	EFI_STATUS (EFIAPI *SetVariable)(CHAR16 *, EFI_GUID *, UINT32, UINTN, VOID *);
	VOID *GetNextHighMonotonicCount;
	VOID (EFIAPI *ResetSystem)(UINT32, EFI_STATUS, UINTN, VOID *);
	VOID *UpdateCapsule;
	VOID *QueryCapsuleCapabilities;
	VOID *QueryVariableInfo;
};

typedef struct EFI_BOOT_SERVICES EFI_BOOT_SERVICES;

/* defined further down; LoadImage/StartImage need the name here */
typedef struct EFI_DEVICE_PATH_PROTOCOL EFI_DEVICE_PATH_PROTOCOL;

struct EFI_BOOT_SERVICES {
	EFI_TABLE_HEADER Hdr;
	EFI_TPL (EFIAPI *RaiseTPL)(EFI_TPL);
	VOID (EFIAPI *RestoreTPL)(EFI_TPL);
	EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE, EFI_MEMORY_TYPE, UINTN,
					   EFI_PHYSICAL_ADDRESS *);
	EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS, UINTN);
	EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *, VOID *, UINTN *, UINTN *, UINT32 *);
	EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE, UINTN, VOID **);
	EFI_STATUS (EFIAPI *FreePool)(VOID *);
	VOID *CreateEvent;
	VOID *SetTimer;
	VOID *WaitForEvent;
	VOID *SignalEvent;
	VOID *CloseEvent;
	VOID *CheckEvent;
	EFI_STATUS (EFIAPI *InstallProtocolInterface)(EFI_HANDLE *, EFI_GUID *, UINTN, VOID *);
	VOID *ReinstallProtocolInterface;
	EFI_STATUS (EFIAPI *UninstallProtocolInterface)(EFI_HANDLE, EFI_GUID *, VOID *);
	EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE, EFI_GUID *, VOID **);
	VOID *Reserved;
	VOID *RegisterProtocolNotify;
	VOID *LocateHandle;
	VOID *LocateDevicePath;
	VOID *InstallConfigurationTable;
	EFI_STATUS (EFIAPI *LoadImage)(BOOLEAN, EFI_HANDLE, EFI_DEVICE_PATH_PROTOCOL *,
				       VOID *, UINTN, EFI_HANDLE *);
	EFI_STATUS (EFIAPI *StartImage)(EFI_HANDLE, UINTN *, CHAR16 **);
	EFI_STATUS (EFIAPI *Exit)(EFI_HANDLE, EFI_STATUS, UINTN, CHAR16 *);
	VOID *UnloadImage;
	EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE, UINTN);
	VOID *GetNextMonotonicCount;
	EFI_STATUS (EFIAPI *Stall)(UINTN);
	VOID *SetWatchdogTimer;
	VOID *ConnectController;
	VOID *DisconnectController;
	VOID *OpenProtocol;
	VOID *CloseProtocol;
	VOID *OpenProtocolInformation;
	VOID *ProtocolsPerHandle;
	EFI_STATUS (EFIAPI *LocateHandleBuffer)(UINTN, EFI_GUID *, VOID *, UINTN *, EFI_HANDLE **);
	EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *, VOID *, VOID **);
	VOID *InstallMultipleProtocolInterfaces;
	VOID *UninstallMultipleProtocolInterfaces;
	VOID *CalculateCrc32;
	VOID *CopyMem;
	VOID *SetMem;
	VOID *CreateEventEx;
};

typedef struct {
	EFI_TABLE_HEADER Hdr;
	CHAR16 *FirmwareVendor;
	UINT32 FirmwareRevision;
	EFI_HANDLE ConsoleInHandle;
	EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
	EFI_HANDLE ConsoleOutHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
	EFI_HANDLE StandardErrorHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
	EFI_RUNTIME_SERVICES *RuntimeServices;
	EFI_BOOT_SERVICES *BootServices;
	UINTN NumberOfTableEntries;
	VOID *ConfigurationTable;
} EFI_SYSTEM_TABLE;

typedef struct {
	UINT32 Revision;
	UINT32 _pad;
	EFI_HANDLE ParentHandle;
	EFI_SYSTEM_TABLE *SystemTable;
	EFI_HANDLE DeviceHandle;
	VOID *FilePath;
	VOID *Reserved;
	UINT32 LoadOptionsSize;
	UINT32 _pad2;
	VOID *LoadOptions;
	VOID *ImageBase;
	UINT64 ImageSize;
	UINT32 ImageCodeType;
	UINT32 ImageDataType;
	VOID *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
	UINT64 Revision;
	EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL *, EFI_FILE_PROTOCOL **, CHAR16 *,
				  UINT64, UINT64);
	EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *);
	EFI_STATUS (EFIAPI *Delete)(EFI_FILE_PROTOCOL *);
	EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL *, UINTN *, VOID *);
	EFI_STATUS (EFIAPI *Write)(EFI_FILE_PROTOCOL *, UINTN *, VOID *);
	EFI_STATUS (EFIAPI *GetPosition)(EFI_FILE_PROTOCOL *, UINT64 *);
	EFI_STATUS (EFIAPI *SetPosition)(EFI_FILE_PROTOCOL *, UINT64);
	EFI_STATUS (EFIAPI *GetInfo)(EFI_FILE_PROTOCOL *, EFI_GUID *, UINTN *, VOID *);
	EFI_STATUS (EFIAPI *SetInfo)(EFI_FILE_PROTOCOL *, EFI_GUID *, UINTN, VOID *);
	EFI_STATUS (EFIAPI *Flush)(EFI_FILE_PROTOCOL *);
	VOID *OpenEx;
	VOID *ReadEx;
	VOID *WriteEx;
	VOID *FlushEx;
};

typedef struct {
	UINT64 Revision;
	EFI_STATUS (EFIAPI *OpenVolume)(VOID *, EFI_FILE_PROTOCOL **);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct {
	UINT16 Year;
	UINT8 Month;
	UINT8 Day;
	UINT8 Hour;
	UINT8 Minute;
	UINT8 Second;
	UINT8 _pad1;
	UINT32 Nanosecond;
	INT16 TimeZone;
	UINT8 Daylight;
	UINT8 _pad2;
} EFI_TIME;

typedef struct {
	UINT64 Size;
	UINT64 FileSize;
	UINT64 PhysicalSize;
	EFI_TIME CreationTime;
	EFI_TIME LastAccessTime;
	EFI_TIME ModificationTime;
	UINT64 Attribute;
	CHAR16 FileName[1];
} EFI_FILE_INFO;

typedef struct {
	UINT64 Size;
	BOOLEAN ReadOnly;
	UINT64 VolumeSize;
	UINT64 FreeSpace;
	UINT32 BlockSize;
	CHAR16 VolumeLabel[1];
} EFI_FILE_SYSTEM_INFO;

struct EFI_DEVICE_PATH_PROTOCOL {
	UINT8 Type;
	UINT8 SubType;
	UINT8 Length[2];
} __attribute__((packed));

typedef struct {
	EFI_DEVICE_PATH_PROTOCOL Header;
	UINT32 PartitionNumber;
	UINT64 PartitionStart;
	UINT64 PartitionSize;
	UINT8 Signature[16];
	UINT8 MBRType;
	UINT8 SignatureType;
} __attribute__((packed)) HARDDRIVE_DEVICE_PATH;

typedef struct {
	CHAR16 *(EFIAPI *ConvertDeviceNodeToText)(EFI_DEVICE_PATH_PROTOCOL *, BOOLEAN, BOOLEAN);
	CHAR16 *(EFIAPI *ConvertDevicePathToText)(EFI_DEVICE_PATH_PROTOCOL *, BOOLEAN, BOOLEAN);
} EFI_DEVICE_PATH_TO_TEXT_PROTOCOL;

#define EFI_BY_PROTOCOL 2
#define EFI_DEVICE_PATH_TYPE_MEDIA 0x04
#define EFI_DEVICE_PATH_SUBTYPE_HARDDRIVE 0x01
#define EFI_DEVICE_PATH_SUBTYPE_FILEPATH 0x04
#define EFI_DEVICE_PATH_TYPE_END 0x7f
#define EFI_DEVICE_PATH_SUBTYPE_END_ENTIRE 0xff
#define EFI_HD_SIGNATURE_MBR 0x01
#define EFI_HD_SIGNATURE_GPT 0x02

/* protocol GUIDs and table pointers (efi.c) */
extern EFI_GUID loaded_image_protocol_guid;
extern EFI_GUID simple_file_system_guid;
extern EFI_GUID file_info_guid;
extern EFI_GUID file_system_info_guid;
extern EFI_GUID device_path_guid;
extern EFI_GUID device_path_to_text_guid;
extern EFI_GUID global_variable_guid;

extern EFI_SYSTEM_TABLE *ST;
extern EFI_BOOT_SERVICES *BS;
extern EFI_RUNTIME_SERVICES *RS;

/* memory */
VOID *memcpy(VOID *dst, const VOID *src, UINTN n);
VOID *memmove(VOID *dst, const VOID *src, UINTN n);
VOID *memset(VOID *dst, int c, UINTN n);
int memcmp(const VOID *a, const VOID *b, UINTN n);

/* console */
VOID print(CHAR16 *s);
VOID print_ch(CHAR16 ch);
VOID print_hex(UINT64 v);
VOID print_hex4(UINT16 v);
VOID print_dec(UINT64 v);
VOID print_trunc(const CHAR16 *s, UINTN max);
EFI_STATUS fail(EFI_STATUS status, CHAR16 *msg);

/* console input */
EFI_INPUT_KEY wait_key(VOID);
EFI_STATUS read_number(UINTN *out);

/* allocation */
VOID *pool_alloc(UINTN size);
VOID *pool_alloc_zero(UINTN size);
EFI_STATUS alloc_pages(EFI_MEMORY_TYPE type, UINTN size,
		       EFI_PHYSICAL_ADDRESS max_addr, VOID **out);

/* files */
EFI_STATUS open_root(EFI_HANDLE image, EFI_FILE_PROTOCOL **root);
EFI_STATUS open_file(EFI_FILE_PROTOCOL *root, CHAR16 *path, EFI_FILE_PROTOCOL **f);
EFI_STATUS file_size(EFI_FILE_PROTOCOL *f, UINT64 *size);
EFI_STATUS read_file(EFI_FILE_PROTOCOL *f, UINT64 off, VOID *buf, UINTN len);
EFI_STATUS write_file(EFI_FILE_PROTOCOL *root, CHAR16 *path, VOID *data, UINTN len);

/* strings */
UINTN str_len16(const CHAR16 *s);
BOOLEAN str_equal_ci(const CHAR16 *a, const CHAR16 *b);

/* device paths */
UINTN dp_node_len(EFI_DEVICE_PATH_PROTOCOL *node);
BOOLEAN dp_is_end(EFI_DEVICE_PATH_PROTOCOL *node);
EFI_DEVICE_PATH_PROTOCOL *dp_next(EFI_DEVICE_PATH_PROTOCOL *node);
BOOLEAN dp_is_filepath(EFI_DEVICE_PATH_PROTOCOL *node);
BOOLEAN dp_is_harddrive(EFI_DEVICE_PATH_PROTOCOL *node);
UINTN dp_nodes_len(EFI_DEVICE_PATH_PROTOCOL *dp);
EFI_DEVICE_PATH_PROTOCOL *dp_with_file(EFI_DEVICE_PATH_PROTOCOL *dp,
				       const CHAR16 *path);

#endif /* EFIBOOT_EFI_H */
