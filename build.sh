#!/bin/sh
# Build the boot kit:
#
#   bootkit/     ptrace_init (the alternative initramfs init) and the
#                LD_PRELOAD probe it drops (preload.c, keygen.c)
#   bootloader/  the EFI loader (efiboot -> bootkit.efi, USB image bootkit.img)
#
# Usage: ./build.sh [ptrace_init output]   (default: ./ptrace_init)
#        DEBUG=1 ./build.sh               # test build: serial console +
#                                         # verbose tracer/probe logs by default
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$ROOT/ptrace_init}
DEBUG=${DEBUG:-0}
case "$DEBUG" in 0|1) ;; *) echo "ERROR: DEBUG must be 0 or 1" >&2; exit 1 ;; esac

# Prefer musl (tiny static binary); fall back to a multilib host gcc.
find_cc() {
    if [ -x "$ROOT/.toolchain/i386-musl/bin/musl-gcc" ]; then
        echo "$ROOT/.toolchain/i386-musl/bin/musl-gcc"
    elif [ -x "$ROOT/../MikroTikPatch/.toolchain/i386-musl/bin/musl-gcc" ]; then
        echo "$ROOT/../MikroTikPatch/.toolchain/i386-musl/bin/musl-gcc"
    elif printf 'int main(void){return 0;}\n' | gcc -m32 -x c - -o /dev/null 2>/dev/null; then
        echo "gcc -m32"
    fi
}

CC=$(find_cc)
if [ -z "$CC" ]; then
    echo "ERROR: no i386 toolchain found." >&2
    echo "       Run MikroTikPatch/tools/musl_i386.sh, or install gcc-multilib." >&2
    exit 1
fi

# 1. the LD_PRELOAD probe (preload.c): ordinary C built without libc, so its
#    imports (open/write/readlink/getpid/snprintf/...) are resolved at load
#    time by the dynamic linker against the process's libc.
#
#    The probe carries the embedded keygen (keygen.c, included by preload.c)
#    and patches the licence public key in mode/keyman, to the key pair in
#    keys.env: $ROOT/keys.env when present, otherwise MikroTikPatch's next to
#    this repo.  The key pair can be overridden with
#    CUSTOM_LICENSE_PUBLIC_KEY / CUSTOM_LICENSE_PRIVATE_KEY, the stock key
#    with MIKRO_LICENSE_PUBLIC_KEY; when nothing is set the defaults baked
#    into keygen.c / preload.c are used.
KEYS_ENV=${KEYS_ENV:-$ROOT/keys.env}
[ -f "$KEYS_ENV" ] || KEYS_ENV=$ROOT/../MikroTikPatch/keys.env
if [ -f "$KEYS_ENV" ]; then
    [ -n "${CUSTOM_LICENSE_PUBLIC_KEY:-}" ] || CUSTOM_LICENSE_PUBLIC_KEY=$(sed -n 's/^CUSTOM_LICENSE_PUBLIC_KEY=//p' "$KEYS_ENV" | head -n1)
    [ -n "${CUSTOM_LICENSE_PRIVATE_KEY:-}" ] || CUSTOM_LICENSE_PRIVATE_KEY=$(sed -n 's/^CUSTOM_LICENSE_PRIVATE_KEY=//p' "$KEYS_ENV" | head -n1)
    [ -n "${MIKRO_LICENSE_PUBLIC_KEY:-}" ] || MIKRO_LICENSE_PUBLIC_KEY=$(sed -n 's/^MIKRO_LICENSE_PUBLIC_KEY=//p' "$KEYS_ENV" | head -n1)
fi

DEFS="-DBOOTKIT_DEBUG_DEFAULT=$DEBUG"
[ -n "${CUSTOM_LICENSE_PUBLIC_KEY:-}" ] && DEFS="$DEFS -DKEYGEN_LICENSE_PUBLIC_HEX=\"$CUSTOM_LICENSE_PUBLIC_KEY\""
[ -n "${CUSTOM_LICENSE_PRIVATE_KEY:-}" ] && DEFS="$DEFS -DKEYGEN_LICENSE_PRIVATE_HEX=\"$CUSTOM_LICENSE_PRIVATE_KEY\""
[ -n "${MIKRO_LICENSE_PUBLIC_KEY:-}" ] && DEFS="$DEFS -DSTOCK_LICENSE_PUBLIC_HEX=\"$MIKRO_LICENSE_PUBLIC_KEY\""
echo "== build: DEBUG=$DEBUG (1 = serial console + verbose tracer/probe logs)"
echo "== probe keys: custom ${CUSTOM_LICENSE_PUBLIC_KEY:-<keygen.c default>}, stock ${MIKRO_LICENSE_PUBLIC_KEY:-<built-in default>}"

# shellcheck disable=SC2086
$CC -shared -fPIC -nostdlib -Os -fno-stack-protector -fvisibility=hidden \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    $DEFS -o "$ROOT/bootkit/preload.so" "$ROOT/bootkit/preload.c"

# 2. embed preload.so into the init binary
python3 - "$ROOT/bootkit/preload.so" "$ROOT/bootkit/preload_so.h" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w') as f:
    f.write('/* generated from preload.so by build.sh - do not edit */\n')
    f.write('static const unsigned char preload_so[] = {\n')
    for i in range(0, len(data), 16):
        f.write('\t' + ','.join('0x%02x' % b for b in data[i:i + 16]) + ',\n')
    f.write('};\n')
    f.write('static const unsigned int preload_so_len = %d;\n' % len(data))
PY

# 3. ptrace_init itself
# shellcheck disable=SC2086
$CC -static -Os -Wall -DBOOTKIT_DEBUG_DEFAULT="$DEBUG" -o "$OUT" "$ROOT/bootkit/ptrace_init.c"
echo "built: $OUT ($(wc -c < "$OUT") bytes), preload.so ($(wc -c < "$ROOT/bootkit/preload.so") bytes)"

# 4. the initramfs (the just-built ptrace_init alone, cpio/newc) and its
#    embedded C array for the EFI loader.
if ! command -v cpio >/dev/null 2>&1; then
    echo "ERROR: cpio not found (needed for the embedded initramfs)." >&2
    exit 1
fi
INITRD_TMP=$(mktemp -d)
trap 'rm -rf "$INITRD_TMP"' EXIT
cp "$OUT" "$INITRD_TMP/ptrace_init"
(cd "$INITRD_TMP" && find ptrace_init | cpio -o -H newc --owner=0:0 2>/dev/null) > "$ROOT/initrd.cpio"
python3 - "$ROOT/initrd.cpio" "$ROOT/bootloader/initrd_so.h" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w') as f:
    f.write('/* generated from initrd.cpio by build.sh - do not edit */\n')
    f.write('static const unsigned char initrd_cpio[] = {\n')
    for i in range(0, len(data), 16):
        f.write('\t' + ','.join('0x%02x' % b for b in data[i:i + 16]) + ',\n')
    f.write('};\n')
    f.write('static const unsigned int initrd_cpio_len = %d;\n' % len(data))
PY
echo "built: $ROOT/initrd.cpio ($(wc -c < "$ROOT/initrd.cpio") bytes), embedded in the loader"

# 5. the EFI loader (installed as \EFI\BOOT\BOOTKIT.EFI; replaces the EFI shell
#    + startup.nsh trick).  Freestanding x86_64 PE32+ built with clang/lld-link,
#    no libc and no gnu-efi; the initramfs is embedded (initrd_so.h, step 4).
if ! command -v clang >/dev/null 2>&1; then
    echo "ERROR: clang not found (needed to build bootkit.efi)." >&2
    exit 1
fi
clang --target=x86_64-unknown-windows -ffreestanding -fno-stack-protector \
    -mno-red-zone -mno-sse -fshort-wchar -Os -Wall -Wextra -nostdlib \
    -DBOOTKIT_DEBUG_DEFAULT="$DEBUG" \
    -fuse-ld=lld-link \
    -Wl,/subsystem:efi_application,/entry:efi_main,/nodefaultlib \
    -o "$ROOT/bootkit.efi" \
    "$ROOT/bootloader/efi_main.c" "$ROOT/bootloader/efi.c" \
    "$ROOT/bootloader/boot.c" "$ROOT/bootloader/config.c" \
    "$ROOT/bootloader/vars.c" "$ROOT/bootloader/installer.c"
echo "built: $ROOT/bootkit.efi ($(wc -c < "$ROOT/bootkit.efi") bytes)"

# 6. bootkit.img: the installer/removable USB stick, a 32 MB MBR disk with one
#    EFI system partition (FAT, type 0xEF) holding \EFI\BOOT\BOOTX64.EFI.  Flash
#    it (dd if=bootkit.img of=/dev/sdX bs=4M conv=fsync); with no \BOOTKIT.CFG
#    on it the loader starts its direct/removable install menu.
for tool in mformat mmd mcopy; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: $tool not found (mtools are needed for bootkit.img)." >&2
        exit 1
    fi
done
USB_MB=${USB_MB:-32}
USB_IMG=$ROOT/bootkit.img
dd if=/dev/zero of="$USB_IMG" bs=1M count="$USB_MB" status=none
python3 - "$USB_IMG" <<'PY'
import os, struct, sys

path = sys.argv[1]
total = os.path.getsize(path) // 512
start = 2048                      # 1 MiB, aligned like the RouterOS ESP
size = total - start
mbr = bytearray(512)
mbr[0x1b8:0x1bc] = struct.pack('<I', 0x4d544b42)   # fixed disk signature
entry = bytearray(16)
entry[0] = 0x00                                   # not bootable (UEFI only)
entry[1:4] = b'\xfe\xff\xff'                      # CHS first (LBA mode)
entry[4] = 0xef                                   # EFI system partition
entry[5:8] = b'\xfe\xff\xff'                      # CHS last
entry[8:12] = struct.pack('<I', start)
entry[12:16] = struct.pack('<I', size)
mbr[0x1be:0x1ce] = entry
mbr[0x1fe:0x200] = b'\x55\xaa'
with open(path, 'r+b') as f:
    f.seek(0)
    f.write(mbr)
PY
mformat -i "$USB_IMG@@1048576" -v BKINSTALL ::
mmd -i "$USB_IMG@@1048576" ::/EFI ::/EFI/BOOT
mcopy -i "$USB_IMG@@1048576" "$ROOT/bootkit.efi" ::/EFI/BOOT/BOOTX64.EFI
echo "built: $USB_IMG ($(wc -c < "$USB_IMG") bytes, flash to a USB stick)"
