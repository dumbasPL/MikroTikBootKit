/*
 * config.c - \BOOTKIT.CFG: parse/write the target partition identity and the
 * "debug=" switch, and find the target partition among the EFI system
 * partitions.  "target=auto" (CONFIG_TARGET_AUTO) selects auto mode instead
 * of a partition identity: find_kernel_root() then returns the first volume
 * whose kernel the arch-specific probe accepts.
 */
#include "efi.h"
#include "config.h"

/* ---- partition identity ------------------------------------------------ */

BOOLEAN target_id_from_dp(EFI_DEVICE_PATH_PROTOCOL *dp, TARGET_ID *id)
{
	while (!dp_is_end(dp)) {
		if (dp_is_harddrive(dp)) {
			HARDDRIVE_DEVICE_PATH *hd = (HARDDRIVE_DEVICE_PATH *)dp;

			memset(id, 0, sizeof(*id));
			id->type = hd->SignatureType;
			memcpy(id->sig, hd->Signature, sizeof(id->sig));
			id->start = hd->PartitionStart;
			id->size = hd->PartitionSize;
			return id->type == EFI_HD_SIGNATURE_MBR ||
			       id->type == EFI_HD_SIGNATURE_GPT;
		}
		dp = dp_next(dp);
	}
	return FALSE;
}

BOOLEAN target_id_equal(const TARGET_ID *a, const TARGET_ID *b)
{
	UINTN sig_len;

	if (a->type != b->type || a->start != b->start || a->size != b->size)
		return FALSE;
	sig_len = a->type == EFI_HD_SIGNATURE_GPT ? 16 : 4;
	return memcmp(a->sig, b->sig, sig_len) == 0;
}

/* ---- parsing ----------------------------------------------------------- */

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static BOOLEAN hex_u64(const char **ps, const char *end, UINT64 *out)
{
	const char *p = *ps;
	UINT64 v = 0;
	UINTN digits = 0;

	while (p < end) {
		int d = hex_digit(*p);

		if (d < 0)
			break;
		v = (v << 4) | (UINTN)d;
		digits++;
		p++;
	}
	if (!digits)
		return FALSE;
	*ps = p;
	*out = v;
	return TRUE;
}

static BOOLEAN parse_target_id(const char *s, UINTN len, TARGET_ID *id)
{
	const char *p = s, *end = s + len;
	UINTN sig_len, i;

	memset(id, 0, sizeof(*id));
	if (len >= 4 && (s[0] == 'm' || s[0] == 'M') &&
	    (s[1] == 'b' || s[1] == 'B') && (s[2] == 'r' || s[2] == 'R') && s[3] == ':') {
		id->type = EFI_HD_SIGNATURE_MBR;
		sig_len = 4;
		p += 4;
	} else if (len >= 4 && (s[0] == 'g' || s[0] == 'G') &&
		   (s[1] == 'p' || s[1] == 'P') && (s[2] == 't' || s[2] == 'T') && s[3] == ':') {
		id->type = EFI_HD_SIGNATURE_GPT;
		sig_len = 16;
		p += 4;
	} else {
		return FALSE;
	}
	if ((UINTN)(end - p) < sig_len * 2)
		return FALSE;
	for (i = 0; i < sig_len; i++) {
		int hi = hex_digit(p[0]), lo = hex_digit(p[1]);

		if (hi < 0 || lo < 0)
			return FALSE;
		id->sig[i] = (UINT8)((hi << 4) | lo);
		p += 2;
	}
	if (p >= end || *p != ':')
		return FALSE;
	p++;
	if (!hex_u64(&p, end, &id->start))
		return FALSE;
	if (p >= end || *p != ':')
		return FALSE;
	p++;
	if (!hex_u64(&p, end, &id->size))
		return FALSE;
	if (p != end || id->size == 0)
		return FALSE;
	return TRUE;
}

static BOOLEAN name_is(const char *s, UINTN len, const char *key)
{
	UINTN i;

	for (i = 0; key[i]; i++) {
		char a = s[i], b = key[i];

		if (a >= 'A' && a <= 'Z')
			a += 32;
		if (i >= len || a != b)
			return FALSE;
	}
	return i < len && s[i] == '=';
}

static BOOLEAN value_is(const char *s, UINTN len, const char *val)
{
	UINTN i;

	for (i = 0; i < len; i++) {
		char a = s[i], b = val[i];

		if (a >= 'A' && a <= 'Z')
			a += 32;
		if (a != b)
			return FALSE;
	}
	return val[i] == 0;
}

static BOOLEAN parse_bool(const char *s, UINTN len, BOOLEAN *out)
{
	if (len == 1 && (s[0] == '0' || s[0] == '1')) {
		*out = s[0] == '1';
		return TRUE;
	}
	if (value_is(s, len, "no") || value_is(s, len, "off") ||
	    value_is(s, len, "false"))
		*out = FALSE;
	else if (value_is(s, len, "yes") || value_is(s, len, "on") ||
		 value_is(s, len, "true"))
		*out = TRUE;
	else
		return FALSE;
	return TRUE;
}

static BOOLEAN config_parse(const char *buf, UINTN len, TARGET_ID *id,
			    BOOLEAN *auto_mode, BOOLEAN *debug)
{
	BOOLEAN have_target = FALSE;
	UINTN i = 0;

	while (i < len) {
		UINTN start = i, end;

		while (i < len && buf[i] != '\n' && buf[i] != '\r')
			i++;
		end = i;
		while (i < len && (buf[i] == '\n' || buf[i] == '\r'))
			i++;
		while (start < end && (buf[start] == ' ' || buf[start] == '\t'))
			start++;
		while (end > start && (buf[end - 1] == ' ' || buf[end - 1] == '\t'))
			end--;
		if (start == end || buf[start] == '#')
			continue;
		if (name_is(buf + start, end - start, "target")) {
			const char *v = buf + start + 7;
			UINTN vlen = end - start - 7;

			if (value_is(v, vlen, CONFIG_TARGET_AUTO)) {
				*auto_mode = TRUE;
				have_target = TRUE;
			} else if (parse_target_id(v, vlen, id)) {
				have_target = TRUE;
			}
		} else if (name_is(buf + start, end - start, "debug")) {
			(void)parse_bool(buf + start + 6, end - start - 6, debug);
		}
	}
	return have_target;
}

EFI_STATUS config_read(EFI_FILE_PROTOCOL *root, TARGET_ID *id, BOOLEAN *auto_mode,
		       BOOLEAN *debug)
{
	EFI_FILE_PROTOCOL *f;
	EFI_STATUS status;
	UINT64 size;
	char buf[CONFIG_MAX + 1];
	UINTN len;

	status = root->Open(root, &f, CONFIG_PATH, EFI_FILE_MODE_READ, 0);
	if (EFI_ERROR(status))
		return status;
	status = file_size(f, &size);
	if (EFI_ERROR(status)) {
		f->Close(f);
		return status;
	}
	len = (UINTN)(size > CONFIG_MAX ? CONFIG_MAX : size);
	status = read_file(f, 0, buf, len);
	f->Close(f);
	if (EFI_ERROR(status))
		return status;
	buf[len] = 0;
	*debug = BOOTKIT_DEBUG_DEFAULT;
	*auto_mode = FALSE;
	if (!config_parse(buf, len, id, auto_mode, debug))
		return EFI_INVALID_PARAMETER;
	return EFI_SUCCESS;
}

/* ---- writing ----------------------------------------------------------- */

static UINTN c_append(char *buf, UINTN cap, UINTN pos, const char *s, UINTN len)
{
	if (cap == 0 || pos >= cap - 1)
		return pos;
	if (len > cap - 1 - pos)
		len = cap - 1 - pos;
	memcpy(buf + pos, s, len);
	return pos + len;
}

/* append one character when there is room for it in buf[0..cap) */
static UINTN c_append_ch(char *buf, UINTN cap, UINTN pos, char c)
{
	if (pos < cap)
		buf[pos] = c;
	return pos + 1;
}

static UINTN c_append_hex_u64(char *buf, UINTN cap, UINTN pos, UINT64 v)
{
	static const char digits[] = "0123456789abcdef";
	char tmp[16];
	UINTN n = 0;

	if (!v)
		tmp[n++] = '0';
	while (v) {
		tmp[n++] = digits[v & 0xf];
		v >>= 4;
	}
	while (n && pos + 1 < cap)
		buf[pos++] = tmp[--n];
	return pos;
}

static UINTN fmt_target_id(char *buf, UINTN cap, const TARGET_ID *id)
{
	static const char digits[] = "0123456789abcdef";
	const char *type = id->type == EFI_HD_SIGNATURE_GPT ? "gpt" : "mbr";
	UINTN sig_len = id->type == EFI_HD_SIGNATURE_GPT ? 16 : 4;
	UINTN pos = 0, i;

	pos = c_append(buf, cap, pos, type, 3);
	pos = c_append_ch(buf, cap, pos, ':');
	for (i = 0; i < sig_len; i++) {
		pos = c_append_ch(buf, cap, pos, digits[id->sig[i] >> 4]);
		pos = c_append_ch(buf, cap, pos, digits[id->sig[i] & 0xf]);
	}
	pos = c_append_ch(buf, cap, pos, ':');
	pos = c_append_hex_u64(buf, cap, pos, id->start);
	pos = c_append_ch(buf, cap, pos, ':');
	pos = c_append_hex_u64(buf, cap, pos, id->size);
	return pos;
}

EFI_STATUS config_write(EFI_FILE_PROTOCOL *root, const TARGET_ID *id,
			EFI_DEVICE_PATH_PROTOCOL *dp, BOOLEAN debug)
{
	EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *d2t = 0;
	char buf[CONFIG_MAX];
	UINTN pos = 0;
	/* reserve room for the lines that follow the comment: the target line
	 * (up to 70 bytes), its CRLF and the debug line */
	UINTN head_cap = sizeof(buf) - 128;

	pos = c_append(buf, sizeof(buf), pos, "# MikroTik boot kit\r\n", 21);
	BS->LocateProtocol(&device_path_to_text_guid, 0, (VOID **)&d2t);
	if (d2t && dp) {
		CHAR16 *text = d2t->ConvertDevicePathToText(dp, FALSE, TRUE);

		if (text) {
			pos = c_append(buf, head_cap, pos, "# target ESP: ", 14);
			for (; *text && pos + 1 < head_cap; text++)
				buf[pos++] = (char)*text;
			pos = c_append(buf, sizeof(buf), pos, "\r\n", 2);
			BS->FreePool(text);
		}
	}
	pos = c_append(buf, sizeof(buf), pos, "target=", 7);
	pos += fmt_target_id(buf + pos, sizeof(buf) - pos - 3, id);
	pos = c_append(buf, sizeof(buf), pos, "\r\n", 2);
	/* debug=1 adds the serial console to the kernel cmdline and the verbose
	 * tracer/probe logs; edit this line to switch at boot time */
	pos = c_append(buf, sizeof(buf), pos, debug ? "debug=1\r\n" : "debug=0\r\n", 9);
	return write_file(root, CONFIG_PATH, buf, pos);
}

/* ---- lookup ------------------------------------------------------------ */

EFI_STATUS find_target_root(const TARGET_ID *id, EFI_FILE_PROTOCOL **out)
{
	EFI_HANDLE *handles = 0;
	UINTN count = 0, i;
	EFI_STATUS status;

	status = BS->LocateHandleBuffer(EFI_BY_PROTOCOL, &simple_file_system_guid,
					0, &count, &handles);
	if (EFI_ERROR(status))
		return status;
	for (i = 0; i < count; i++) {
		EFI_DEVICE_PATH_PROTOCOL *dp = 0;
		EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
		TARGET_ID cur;

		if (EFI_ERROR(BS->HandleProtocol(handles[i], &device_path_guid,
						 (VOID **)&dp)))
			continue;
		if (!target_id_from_dp(dp, &cur) || !target_id_equal(&cur, id))
			continue;
		if (EFI_ERROR(BS->HandleProtocol(handles[i], &simple_file_system_guid,
						 (VOID **)&fs)))
			continue;
		if (EFI_ERROR(fs->OpenVolume(fs, out)))
			continue;
		BS->FreePool(handles);
		return EFI_SUCCESS;
	}
	BS->FreePool(handles);
	return EFI_NOT_FOUND;
}

/* "efiboot: auto: kernel found on ..." - which volume the scan picked */
static VOID print_kernel_volume(EFI_HANDLE handle)
{
	EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *d2t = 0;
	EFI_DEVICE_PATH_PROTOCOL *dp = 0;
	CHAR16 *text;

	BS->LocateProtocol(&device_path_to_text_guid, 0, (VOID **)&d2t);
	if (!d2t ||
	    EFI_ERROR(BS->HandleProtocol(handle, &device_path_guid, (VOID **)&dp)))
		return;
	text = d2t->ConvertDevicePathToText(dp, FALSE, TRUE);
	if (!text)
		return;
	print(L"efiboot: auto: kernel found on ");
	print_trunc(text, 60);
	print(L"\r\n");
	BS->FreePool(text);
}

/*
 * target=auto: hand every volume to the arch-specific probe and return the
 * first one holding a bootable RouterOS kernel.  EFI_NOT_FOUND means there is
 * none (the callers then fall back to the install menu).
 */
EFI_STATUS find_kernel_root(KERNEL_PROBE probe, EFI_FILE_PROTOCOL **out)
{
	EFI_HANDLE *handles = 0;
	UINTN count = 0, i;
	EFI_STATUS status;

	status = BS->LocateHandleBuffer(EFI_BY_PROTOCOL, &simple_file_system_guid,
					0, &count, &handles);
	if (EFI_ERROR(status))
		return status;
	for (i = 0; i < count; i++) {
		EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
		EFI_FILE_PROTOCOL *root;

		if (EFI_ERROR(BS->HandleProtocol(handles[i], &simple_file_system_guid,
						 (VOID **)&fs)))
			continue;
		if (EFI_ERROR(fs->OpenVolume(fs, &root)))
			continue;
		if (probe(root)) {
			print_kernel_volume(handles[i]);
			BS->FreePool(handles);
			*out = root;
			return EFI_SUCCESS;
		}
		root->Close(root);
	}
	BS->FreePool(handles);
	return EFI_NOT_FOUND;
}
