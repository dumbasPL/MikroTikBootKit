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
#        EFI_CC=<cc> ./build.sh           # EFI loader compiler (clang or a
#                                         # PE-targeting gcc like
#                                         # x86_64-w64-mingw32-gcc; auto)
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$ROOT/ptrace_init}
DEBUG=${DEBUG:-0}
case "$DEBUG" in 0|1) ;; *) echo "ERROR: DEBUG must be 0 or 1" >&2; exit 1 ;; esac

# The i386 binary is always built against musl (tiny static binary).  If the
# local toolchain is missing, build.sh runs tools/musl_i386.sh itself.
CC=$ROOT/.toolchain/i386-musl/bin/musl-gcc
if [ ! -x "$CC" ]; then
    echo "== i486 musl toolchain missing, running tools/musl_i386.sh"
    "$ROOT/tools/musl_i386.sh"
fi
if [ ! -x "$CC" ]; then
    echo "ERROR: tools/musl_i386.sh did not install $CC" >&2
    exit 1
fi

# 1. the LD_PRELOAD probe (preload.c): ordinary C built without libc, so its
#    imports (open/write/readlink/getpid/snprintf/...) are resolved at load
#    time by the dynamic linker against the process's libc.
#
#    The probe carries the embedded keygen (keygen.c, included by preload.c)
#    and patches the licence public key in mode/keyman.  The key pair is
#    hard-coded in keygen.c / preload.c; the environment can override it:
#    CUSTOM_LICENSE_PUBLIC_KEY / CUSTOM_LICENSE_PRIVATE_KEY (the pair) and
#    MIKRO_LICENSE_PUBLIC_KEY (the stock key).

DEFS="-DBOOTKIT_DEBUG_DEFAULT=$DEBUG"
[ -n "${CUSTOM_LICENSE_PUBLIC_KEY:-}" ] && DEFS="$DEFS -DKEYGEN_LICENSE_PUBLIC_HEX=\"$CUSTOM_LICENSE_PUBLIC_KEY\""
[ -n "${CUSTOM_LICENSE_PRIVATE_KEY:-}" ] && DEFS="$DEFS -DKEYGEN_LICENSE_PRIVATE_HEX=\"$CUSTOM_LICENSE_PRIVATE_KEY\""
[ -n "${MIKRO_LICENSE_PUBLIC_KEY:-}" ] && DEFS="$DEFS -DSTOCK_LICENSE_PUBLIC_HEX=\"$MIKRO_LICENSE_PUBLIC_KEY\""
echo "== build: DEBUG=$DEBUG (1 = serial console + verbose tracer/probe logs)"
echo "== i386 compiler: $CC"
echo "== probe keys: custom ${CUSTOM_LICENSE_PUBLIC_KEY:-<built-in>}, stock ${MIKRO_LICENSE_PUBLIC_KEY:-<built-in>}"

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
#    + startup.nsh trick).  Freestanding x86_64 PE32+ with the initramfs
#    embedded (initrd_so.h, step 4), no libc and no gnu-efi.
#
#    Prefer clang + lld-link (--target=x86_64-unknown-windows); a gcc that
#    targets PE, e.g. x86_64-w64-mingw32-gcc, works equally well.  Force one
#    with EFI_CC=<compiler>.
EFI_CC=${EFI_CC:-auto}
if [ "$EFI_CC" = auto ]; then
    if command -v clang >/dev/null 2>&1 && command -v lld-link >/dev/null 2>&1; then
        EFI_CC=clang
    elif command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
        EFI_CC=x86_64-w64-mingw32-gcc
    fi
fi
if [ -z "$EFI_CC" ] || [ "$EFI_CC" = auto ]; then
    echo "ERROR: no EFI compiler found (need clang + lld-link, or" >&2
    echo "       x86_64-w64-mingw32-gcc; override with EFI_CC=...)." >&2
    exit 1
fi

LOADER_SRCS="efi_main efi boot config vars installer"
EFI_OBJ=$(mktemp -d)
trap 'rm -rf "$INITRD_TMP" "$EFI_OBJ"' EXIT
case "$EFI_CC" in
    *clang*)
        clang --target=x86_64-unknown-windows -ffreestanding -fno-stack-protector \
            -mno-red-zone -mno-sse -fshort-wchar -Os -Wall -Wextra -nostdlib \
            -DBOOTKIT_DEBUG_DEFAULT="$DEBUG" \
            -fuse-ld=lld-link \
            -Wl,/subsystem:efi_application,/entry:efi_main,/nodefaultlib \
            -o "$ROOT/bootkit.efi" \
            "$ROOT/bootloader/efi_main.c" "$ROOT/bootloader/efi.c" \
            "$ROOT/bootloader/boot.c" "$ROOT/bootloader/config.c" \
            "$ROOT/bootloader/vars.c" "$ROOT/bootloader/installer.c"
        ;;
    *)
        if ! $EFI_CC -dumpmachine 2>/dev/null | grep -q mingw; then
            echo "ERROR: $EFI_CC does not target PE/Windows; use clang" >&2
            echo "       (--target=x86_64-unknown-windows) or" >&2
            echo "       x86_64-w64-mingw32-gcc." >&2
            exit 1
        fi
        for s in $LOADER_SRCS; do
            $EFI_CC -c -ffreestanding -fshort-wchar -fno-stack-protector \
                -mno-red-zone -mno-sse -fno-asynchronous-unwind-tables \
                -Os -Wall -Wextra -DBOOTKIT_DEBUG_DEFAULT="$DEBUG" \
                -I "$ROOT/bootloader" \
                -o "$EFI_OBJ/$s.o" "$ROOT/bootloader/$s.c"
        done
        $EFI_CC -nostdlib -Wl,--subsystem,10 -Wl,-e,efi_main \
            -Wl,--no-insert-timestamp \
            -o "$ROOT/bootkit.efi" "$EFI_OBJ"/*.o
        ;;
esac
echo "built: $ROOT/bootkit.efi ($(wc -c < "$ROOT/bootkit.efi") bytes, via $EFI_CC)"

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
