/*
 * initrdvol.c - the synthetic EFI volume the arm64 loader serves its embedded
 * initramfs from: one handle with the Simple File System protocol and one
 * read-only file (EFI_FILE_PROTOCOL).  The Linux 5.6 arm64 EFI stub opens the
 * "initrd=" file through exactly this path:
 *
 *   handle_protocol(image->device_handle, EFI_FILE_SYSTEM_GUID, &io)
 *   io->open_volume(io, &fh)
 *   fh->open(fh, &h, filename, EFI_FILE_MODE_READ, 0)
 *   h->get_info(h, &EFI_FILE_INFO_ID, &size, NULL)  (expects BUFFER_TOO_SMALL)
 *   h->get_info(h, &EFI_FILE_INFO_ID, &size, info)
 *   h->read(h, &size, addr)
 *   h->close(h)
 *
 * so only Open/Close/GetInfo/Read (and SetPosition) need to do something; the
 * file name is accepted as-is (there is only one file).
 */
#include "efi.h"
#include "initrdvol.h"

static const UINT8 *cpio_data;
static UINTN cpio_len;

typedef struct {
	EFI_FILE_PROTOCOL proto;
	UINT64 pos;
} INITRD_FILE;

static INITRD_FILE initrd_file;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL initrd_fs;

/* the file the kernel asks for by name in "initrd=" */
static CHAR16 initrd_name[] = L"initrd.cpio";

/* ---- the single file --------------------------------------------------- */

static EFI_STATUS EFIAPI file_open(EFI_FILE_PROTOCOL *this,
				   EFI_FILE_PROTOCOL **new_file, CHAR16 *name,
				   UINT64 mode, UINT64 attrs)
{
	(void)name;
	(void)attrs;
	if (!new_file)
		return EFI_INVALID_PARAMETER;
	if (!(mode & EFI_FILE_MODE_READ))
		return EFI_UNSUPPORTED;
	*new_file = this;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_close(EFI_FILE_PROTOCOL *this)
{
	(void)this;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_delete(EFI_FILE_PROTOCOL *this)
{
	(void)this;
	return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI file_read(EFI_FILE_PROTOCOL *this, UINTN *size,
				   VOID *buf)
{
	INITRD_FILE *f = (INITRD_FILE *)this;
	UINTN want, avail, n;

	if (!size || (!buf && *size))
		return EFI_INVALID_PARAMETER;
	want = *size;
	if (f->pos >= cpio_len) {
		*size = 0;
		return EFI_SUCCESS;
	}
	avail = cpio_len - (UINTN)f->pos;
	n = want < avail ? want : avail;
	memcpy(buf, cpio_data + f->pos, n);
	f->pos += n;
	*size = n;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_write(EFI_FILE_PROTOCOL *this, UINTN *size,
				    VOID *buf)
{
	(void)this;
	(void)size;
	(void)buf;
	return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI file_get_position(EFI_FILE_PROTOCOL *this, UINT64 *pos)
{
	if (!pos)
		return EFI_INVALID_PARAMETER;
	*pos = ((INITRD_FILE *)this)->pos;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_set_position(EFI_FILE_PROTOCOL *this, UINT64 pos)
{
	((INITRD_FILE *)this)->pos = pos;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_get_info(EFI_FILE_PROTOCOL *this,
				       EFI_GUID *info_type, UINTN *size,
				       VOID *buf)
{
	EFI_FILE_INFO *info = buf;
	UINTN need;

	(void)this;
	if (!size)
		return EFI_INVALID_PARAMETER;
	if (!info_type || memcmp(info_type, &file_info_guid, sizeof(EFI_GUID)) != 0)
		return EFI_UNSUPPORTED;
	/* EFI_FILE_INFO already carries FileName[1]; add the rest of the name */
	need = sizeof(EFI_FILE_INFO) + sizeof(initrd_name) - 2;
	if (*size < need) {
		*size = need;
		return EFI_BUFFER_TOO_SMALL;
	}
	memset(info, 0, need);
	info->Size = need;
	info->FileSize = cpio_len;
	info->PhysicalSize = cpio_len;
	memcpy(info->FileName, initrd_name, sizeof(initrd_name));
	*size = need;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_set_info(EFI_FILE_PROTOCOL *this,
				       EFI_GUID *info_type, UINTN size,
				       VOID *buf)
{
	(void)this;
	(void)info_type;
	(void)size;
	(void)buf;
	return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI file_flush(EFI_FILE_PROTOCOL *this)
{
	(void)this;
	return EFI_SUCCESS;
}

/* ---- the volume -------------------------------------------------------- */

static EFI_STATUS EFIAPI fs_open_volume(VOID *this, EFI_FILE_PROTOCOL **root)
{
	(void)this;
	if (!root)
		return EFI_INVALID_PARAMETER;
	*root = &initrd_file.proto;
	return EFI_SUCCESS;
}

EFI_STATUS initrdvol_create(const UINT8 *data, UINTN len, EFI_HANDLE *out)
{
	EFI_HANDLE handle = 0;
	EFI_STATUS status;

	if (!data || !len || !out)
		return EFI_INVALID_PARAMETER;
	cpio_data = data;
	cpio_len = len;
	initrd_file.pos = 0;
	initrd_fs.Revision = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
	initrd_fs.OpenVolume = fs_open_volume;
	initrd_file.proto.Revision = EFI_FILE_PROTOCOL_REVISION;
	initrd_file.proto.Open = file_open;
	initrd_file.proto.Close = file_close;
	initrd_file.proto.Delete = file_delete;
	initrd_file.proto.Read = file_read;
	initrd_file.proto.Write = file_write;
	initrd_file.proto.GetPosition = file_get_position;
	initrd_file.proto.SetPosition = file_set_position;
	initrd_file.proto.GetInfo = file_get_info;
	initrd_file.proto.SetInfo = file_set_info;
	initrd_file.proto.Flush = file_flush;
	initrd_file.proto.OpenEx = 0;
	initrd_file.proto.ReadEx = 0;
	initrd_file.proto.WriteEx = 0;
	initrd_file.proto.FlushEx = 0;

	status = BS->InstallProtocolInterface(&handle, &simple_file_system_guid,
					      EFI_NATIVE_INTERFACE, &initrd_fs);
	if (EFI_ERROR(status))
		return status;
	*out = handle;
	return EFI_SUCCESS;
}
