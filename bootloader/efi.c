/*
 * efi.c - the small EFI subset the bootloader uses: console output/input,
 * memory primitives, allocation helpers, file access and device path walking.
 * Freestanding: no libc, no gnu-efi.
 */
#include "efi.h"

/* ---- protocol GUIDs ---------------------------------------------------- */

EFI_GUID loaded_image_protocol_guid = {
	0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }
};
EFI_GUID simple_file_system_guid = {
	0x0964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }
};
EFI_GUID file_info_guid = {
	0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }
};
EFI_GUID file_system_info_guid = {
	0x09576e93, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }
};
EFI_GUID device_path_guid = {
	0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }
};
EFI_GUID device_path_to_text_guid = {
	0x8b843e20, 0x8132, 0x4852, { 0x90, 0xcc, 0x55, 0x1a, 0x4e, 0x4a, 0x7f, 0x1c }
};
EFI_GUID global_variable_guid = {
	0x8be4df61, 0x93ca, 0x11d2, { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c }
};
/* ---- table pointers, set from efi_main() ------------------------------- */

EFI_SYSTEM_TABLE *ST;
EFI_BOOT_SERVICES *BS;
EFI_RUNTIME_SERVICES *RS;

/*
 * The image is fully position independent, so without this the linker would
 * emit no base relocations at all and firmware could not load the image
 * anywhere but its preferred base (0x140000000).  One self-referencing
 * pointer forces a .reloc section.
 */
static const VOID *const force_relocation __attribute__((used)) = &force_relocation;

/* ---- memory ------------------------------------------------------------ */

VOID *memcpy(VOID *dst, const VOID *src, UINTN n)
{
	UINT8 *d = dst;
	const UINT8 *s = src;

	while (n--)
		*d++ = *s++;
	return dst;
}

VOID *memmove(VOID *dst, const VOID *src, UINTN n)
{
	UINT8 *d = dst;
	const UINT8 *s = src;

	if (d < s)
		while (n--)
			*d++ = *s++;
	else {
		d += n;
		s += n;
		while (n--)
			*--d = *--s;
	}
	return dst;
}

VOID *memset(VOID *dst, int c, UINTN n)
{
	UINT8 *d = dst;

	while (n--)
		*d++ = (UINT8)c;
	return dst;
}

int memcmp(const VOID *a, const VOID *b, UINTN n)
{
	const UINT8 *p = a, *q = b;

	while (n--) {
		if (*p != *q)
			return (int)*p - (int)*q;
		p++;
		q++;
	}
	return 0;
}

/* ---- console ----------------------------------------------------------- */

VOID print(CHAR16 *s)
{
	ST->ConOut->OutputString(ST->ConOut, s);
}

VOID print_ch(CHAR16 ch)
{
	CHAR16 buf[2];

	buf[0] = ch;
	buf[1] = 0;
	print(buf);
}

VOID print_hex(UINT64 v)
{
	static const CHAR16 digits[] = L"0123456789abcdef";
	CHAR16 buf[19];
	UINTN i;

	buf[0] = L'0';
	buf[1] = L'x';
	for (i = 0; i < 16; i++)
		buf[2 + i] = digits[(v >> ((15 - i) * 4)) & 0xf];
	buf[18] = 0;
	print(buf);
}

VOID print_hex4(UINT16 v)
{
	static const CHAR16 digits[] = L"0123456789abcdef";
	CHAR16 buf[5];
	UINTN i;

	for (i = 0; i < 4; i++)
		buf[i] = digits[(v >> ((3 - i) * 4)) & 0xf];
	buf[4] = 0;
	print(buf);
}

VOID print_dec(UINT64 v)
{
	CHAR16 buf[21];
	UINTN i = 20;

	buf[i] = 0;
	if (!v)
		buf[--i] = L'0';
	while (v) {
		buf[--i] = (CHAR16)(L'0' + v % 10);
		v /= 10;
	}
	print(buf + i);
}

VOID print_trunc(const CHAR16 *s, UINTN max)
{
	CHAR16 buf[80];
	UINTN i = 0;
	/* keep room for the "..." marker and the terminator */
	UINTN cap = sizeof(buf) / sizeof(buf[0]) - 4;

	if (max > cap)
		max = cap;
	while (i < max && s[i]) {
		buf[i] = s[i];
		i++;
	}
	if (s[i]) {
		buf[i++] = L'.';
		buf[i++] = L'.';
		buf[i++] = L'.';
	}
	buf[i] = 0;
	print(buf);
}

EFI_STATUS fail(EFI_STATUS status, CHAR16 *msg)
{
	print(L"efiboot: ");
	print(msg);
	print(L" (status ");
	print_hex(status);
	print(L")\r\n");
	BS->Stall(5000000);
	return status;
}

/* ---- console input ----------------------------------------------------- */

EFI_INPUT_KEY wait_key(VOID)
{
	EFI_INPUT_KEY key = { 0, 0 };

	while (EFI_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &key)))
		BS->Stall(20000);
	return key;
}

EFI_STATUS read_number(UINTN *out)
{
	UINTN val = 0;
	BOOLEAN any = FALSE;

	for (;;) {
		EFI_INPUT_KEY key = wait_key();

		if (key.ScanCode == 0x17 || key.UnicodeChar == L'q' ||
		    key.UnicodeChar == L'Q')
			return EFI_ABORTED;
		if (key.UnicodeChar == L'\r' || key.UnicodeChar == L'\n') {
			if (any) {
				*out = val;
				return EFI_SUCCESS;
			}
			continue;
		}
		if (key.UnicodeChar >= L'0' && key.UnicodeChar <= L'9' && val < 1000) {
			val = val * 10 + (key.UnicodeChar - L'0');
			any = TRUE;
			print_ch(key.UnicodeChar);
		} else if (key.UnicodeChar == 0x08 && any) {
			val /= 10;
			any = val != 0;
			print(L"\b \b");
		}
	}
}

/* ---- allocation -------------------------------------------------------- */

VOID *pool_alloc(UINTN size)
{
	VOID *p = 0;

	if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, size, &p)))
		return 0;
	return p;
}

VOID *pool_alloc_zero(UINTN size)
{
	VOID *p = pool_alloc(size);

	if (p)
		memset(p, 0, size);
	return p;
}

EFI_STATUS alloc_pages(EFI_MEMORY_TYPE type, UINTN size,
			      EFI_PHYSICAL_ADDRESS max_addr, VOID **out)
{
	EFI_PHYSICAL_ADDRESS addr = max_addr;
	EFI_STATUS status;

	status = BS->AllocatePages(AllocateMaxAddress, type,
				   EFI_SIZE_TO_PAGES(size), &addr);
	if (EFI_ERROR(status))
		return status;
	*out = (VOID *)(UINTN)addr;
	return EFI_SUCCESS;
}

/* ---- files ------------------------------------------------------------- */

EFI_STATUS open_root(EFI_HANDLE image, EFI_FILE_PROTOCOL **root)
{
	EFI_LOADED_IMAGE_PROTOCOL *li;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
	EFI_STATUS status;

	status = BS->HandleProtocol(image, &loaded_image_protocol_guid, (VOID **)&li);
	if (EFI_ERROR(status))
		return status;
	status = BS->HandleProtocol(li->DeviceHandle, &simple_file_system_guid, (VOID **)&fs);
	if (EFI_ERROR(status))
		return status;
	return fs->OpenVolume(fs, root);
}

EFI_STATUS open_file(EFI_FILE_PROTOCOL *root, CHAR16 *path, EFI_FILE_PROTOCOL **f)
{
	return root->Open(root, f, path, EFI_FILE_MODE_READ, 0);
}

EFI_STATUS file_size(EFI_FILE_PROTOCOL *f, UINT64 *size)
{
	EFI_FILE_INFO *info;
	EFI_STATUS status;
	UINTN buf_size = 560;
	UINT8 buf[560];

	status = f->GetInfo(f, &file_info_guid, &buf_size, buf);
	if (EFI_ERROR(status))
		return status;
	info = (EFI_FILE_INFO *)buf;
	*size = info->FileSize;
	return EFI_SUCCESS;
}

EFI_STATUS read_file(EFI_FILE_PROTOCOL *f, UINT64 off, VOID *buf, UINTN len)
{
	EFI_STATUS status;
	UINT8 *p = buf;

	status = f->SetPosition(f, off);
	if (EFI_ERROR(status))
		return status;
	while (len) {
		UINTN n = len;

		status = f->Read(f, &n, p);
		if (EFI_ERROR(status))
			return status;
		if (n == 0)
			return EFI_LOAD_ERROR;	/* short file */
		p += n;
		len -= n;
	}
	return EFI_SUCCESS;
}

EFI_STATUS write_file(EFI_FILE_PROTOCOL *root, CHAR16 *path, VOID *data, UINTN len)
{
	EFI_FILE_PROTOCOL *f;
	EFI_STATUS status;
	UINTN written = len;
	UINT8 fi_buf[sizeof(EFI_FILE_INFO) + 8];
	EFI_FILE_INFO *fi = (EFI_FILE_INFO *)fi_buf;

	status = root->Open(root, &f, path,
			    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
	if (EFI_ERROR(status))
		return status;
	memset(fi_buf, 0, sizeof(fi_buf));
	fi->Size = sizeof(EFI_FILE_INFO) + 2;
	fi->FileSize = 0;
	f->SetInfo(f, &file_info_guid, sizeof(EFI_FILE_INFO) + 2, fi_buf);
	status = f->Write(f, &written, data);
	f->Flush(f);
	f->Close(f);
	if (EFI_ERROR(status))
		return status;
	if (written != len)
		return EFI_DEVICE_ERROR;
	return EFI_SUCCESS;
}

/* ---- strings ----------------------------------------------------------- */

UINTN str_len16(const CHAR16 *s)
{
	UINTN n = 0;

	while (s[n])
		n++;
	return n;
}

static BOOLEAN chr_equal_ci(CHAR16 a, CHAR16 b)
{
	if (a >= L'a' && a <= L'z')
		a -= 32;
	if (b >= L'a' && b <= L'z')
		b -= 32;
	return a == b;
}

BOOLEAN str_equal_ci(const CHAR16 *a, const CHAR16 *b)
{
	while (*a && *b) {
		if (!chr_equal_ci(*a, *b))
			return FALSE;
		a++;
		b++;
	}
	return *a == *b;
}

/* ---- device paths ------------------------------------------------------ */

UINTN dp_node_len(EFI_DEVICE_PATH_PROTOCOL *node)
{
	return (UINTN)node->Length[0] | ((UINTN)node->Length[1] << 8);
}

BOOLEAN dp_is_end(EFI_DEVICE_PATH_PROTOCOL *node)
{
	return node->Type == EFI_DEVICE_PATH_TYPE_END &&
	       node->SubType == EFI_DEVICE_PATH_SUBTYPE_END_ENTIRE;
}

EFI_DEVICE_PATH_PROTOCOL *dp_next(EFI_DEVICE_PATH_PROTOCOL *node)
{
	return (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)node + dp_node_len(node));
}

BOOLEAN dp_is_filepath(EFI_DEVICE_PATH_PROTOCOL *node)
{
	return node->Type == EFI_DEVICE_PATH_TYPE_MEDIA &&
	       node->SubType == EFI_DEVICE_PATH_SUBTYPE_FILEPATH;
}

BOOLEAN dp_is_harddrive(EFI_DEVICE_PATH_PROTOCOL *node)
{
	return node->Type == EFI_DEVICE_PATH_TYPE_MEDIA &&
	       node->SubType == EFI_DEVICE_PATH_SUBTYPE_HARDDRIVE;
}

UINTN dp_nodes_len(EFI_DEVICE_PATH_PROTOCOL *dp)
{
	UINTN len = 0;

	while (!dp_is_end(dp)) {
		len += dp_node_len(dp);
		dp = dp_next(dp);
	}
	return len;
}

EFI_DEVICE_PATH_PROTOCOL *dp_with_file(EFI_DEVICE_PATH_PROTOCOL *dp,
					      const CHAR16 *path)
{
	UINTN dev_len = dp_nodes_len(dp);
	UINTN path_len = str_len16(path);
	UINTN fp_len = 4 + (path_len + 1) * 2, total = dev_len + fp_len + 4;
	EFI_DEVICE_PATH_PROTOCOL *full;
	UINT8 *fp;
	VOID *p = 0;

	if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, total, &p)))
		return 0;
	full = p;
	memcpy(full, dp, dev_len);
	fp = (UINT8 *)full + dev_len;
	fp[0] = EFI_DEVICE_PATH_TYPE_MEDIA;
	fp[1] = EFI_DEVICE_PATH_SUBTYPE_FILEPATH;
	fp[2] = fp_len & 0xff;
	fp[3] = fp_len >> 8;
	memcpy(fp + 4, path, (path_len + 1) * 2);
	fp += fp_len;
	fp[0] = EFI_DEVICE_PATH_TYPE_END;
	fp[1] = EFI_DEVICE_PATH_SUBTYPE_END_ENTIRE;
	fp[2] = 4;
	fp[3] = 0;
	return full;
}
