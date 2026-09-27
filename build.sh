#!/bin/sh
# Build the boot kit:
#
#   bootkit/     ptrace_init (the alternative initramfs init) and the
#                LD_PRELOAD probe it drops (preload.c, keygen.c), for i386
#                (x86/x86_64 RouterOS) and for arm32 (the arm64 CHR's
#                AArch32 userspace)
#   bootloader/  the EFI loaders (bootkit.efi for x86_64, bootkit-arm64.efi
#                for AArch64) and the USB installer/removable sticks
#                (bootkit.img, bootkit-arm64.img)
#
# Usage: ./build.sh [ptrace_init output]   (default: ./ptrace_init)
#        DEBUG=1 ./build.sh               # test build: serial console +
#                                         # verbose tracer/probe logs by default
#        ARCH=x86|arm64|all ./build.sh    # what to build (default: all;
#                                         # arm64 needs clang, x86 also accepts
#                                         # a PE-targeting gcc)
#        EFI_CC=<cc> ./build.sh           # x86 EFI loader compiler (clang or a
#                                         # PE-targeting gcc like
#                                         # x86_64-w64-mingw32-gcc; auto)
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$ROOT/ptrace_init}
DEBUG=${DEBUG:-0}
ARCH=${ARCH:-all}
case "$DEBUG" in 0|1) ;; *) echo "ERROR: DEBUG must be 0 or 1" >&2; exit 1 ;; esac
case "$ARCH" in x86|arm64|all) ;; *) echo "ERROR: ARCH must be x86, arm64 or all" >&2; exit 1 ;; esac

# The licence key pair can be overridden at build time; the probe and the
# keygen must use the same pair, so this applies to both architectures.
DEFS="-DBOOTKIT_DEBUG_DEFAULT=$DEBUG"
[ -n "${CUSTOM_LICENSE_PUBLIC_KEY:-}" ] && DEFS="$DEFS -DKEYGEN_LICENSE_PUBLIC_HEX=\"$CUSTOM_LICENSE_PUBLIC_KEY\""
[ -n "${CUSTOM_LICENSE_PRIVATE_KEY:-}" ] && DEFS="$DEFS -DKEYGEN_LICENSE_PRIVATE_HEX=\"$CUSTOM_LICENSE_PRIVATE_KEY\""
[ -n "${MIKRO_LICENSE_PUBLIC_KEY:-}" ] && DEFS="$DEFS -DSTOCK_LICENSE_PUBLIC_HEX=\"$MIKRO_LICENSE_PUBLIC_KEY\""
echo "== build: DEBUG=$DEBUG (1 = serial console + verbose tracer/probe logs), ARCH=$ARCH"
echo "== probe keys: custom ${CUSTOM_LICENSE_PUBLIC_KEY:-<built-in>}, stock ${MIKRO_LICENSE_PUBLIC_KEY:-<built-in>}"

# embed <input> <output> <symbol> - write a C array header
embed() {
    python3 - "$1" "$2" "$3" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
sym = sys.argv[3]
with open(sys.argv[2], 'w') as f:
    f.write('/* generated from %s by build.sh - do not edit */\n' % sys.argv[1])
    f.write('static const unsigned char %s[] = {\n' % sym)
    for i in range(0, len(data), 16):
        f.write('\t' + ','.join('0x%02x' % b for b in data[i:i + 16]) + ',\n')
    f.write('};\n')
    f.write('static const unsigned int %s_len = %d;\n' % (sym, len(data)))
PY
}

# initrd_cpio <ptrace_init> <cpio> <header>
initrd_cpio() {
    if ! command -v cpio >/dev/null 2>&1; then
        echo "ERROR: cpio not found (needed for the embedded initramfs)." >&2
        exit 1
    fi
    TMP=$(mktemp -d)
    trap 'rm -rf "$TMP"' EXIT
    cp "$1" "$TMP/ptrace_init"
    (cd "$TMP" && find ptrace_init | cpio -o -H newc --owner=0:0 2>/dev/null) > "$2"
    rm -rf "$TMP"
    trap - EXIT
    embed "$2" "$3" "initrd_cpio"
    echo "built: $2 ($(wc -c < "$2") bytes), embedded in the loader"
}

# bootkit_img <image> <label> <mbr-signature> <loader> <dest-path>
bootkit_img() {
    for tool in mformat mmd mcopy; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "ERROR: $tool not found (mtools are needed for $1)." >&2
            exit 1
        fi
    done
    USB_MB=${USB_MB:-32}
    dd if=/dev/zero of="$1" bs=1M count="$USB_MB" status=none
    python3 - "$1" "$3" <<'PY'
import os, struct, sys

path = sys.argv[1]
sig = int(sys.argv[2], 0)
total = os.path.getsize(path) // 512
start = 2048                      # 1 MiB, aligned like the RouterOS ESP
size = total - start
mbr = bytearray(512)
mbr[0x1b8:0x1bc] = struct.pack('<I', sig)          # fixed disk signature
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
    mformat -i "$1@@1048576" -v "$2" ::
    mmd -i "$1@@1048576" ::/EFI ::/EFI/BOOT
    mcopy -i "$1@@1048576" "$4" "::/$5"
    echo "built: $1 ($(wc -c < "$1") bytes, flash to a USB stick)"
}

# ---------------------------------------------------------------- x86/x86_64

build_x86() {
    # The i386 binary is always built against musl (tiny static binary).  If
    # the local toolchain is missing, build.sh runs tools/musl_i386.sh itself.
    CC=$ROOT/.toolchain/i386-musl/bin/musl-gcc
    if [ ! -x "$CC" ]; then
        echo "== i486 musl toolchain missing, running tools/musl_i386.sh"
        "$ROOT/tools/musl_i386.sh"
    fi
    if [ ! -x "$CC" ]; then
        echo "ERROR: tools/musl_i386.sh did not install $CC" >&2
        exit 1
    fi

    # 1. the LD_PRELOAD probe (preload.c): ordinary C built without libc, so
    #    its imports (open/write/readlink/getpid/snprintf/...) are resolved at
    #    load time by the dynamic linker against the process's libc.
    #
    #    The probe carries the embedded keygen (keygen.c, included by
    #    preload.c) and patches the licence public key in mode/keyman.
    echo "== i386 compiler: $CC"
    # shellcheck disable=SC2086
    $CC -shared -fPIC -nostdlib -Os -fno-stack-protector -fvisibility=hidden \
        -ffunction-sections -fdata-sections -Wl,--gc-sections \
        $DEFS -o "$ROOT/bootkit/preload.so" "$ROOT/bootkit/preload.c"

    # 2. embed preload.so into the init binary
    embed "$ROOT/bootkit/preload.so" "$ROOT/bootkit/preload_so.h" "preload_so"

    # 3. ptrace_init itself
    # shellcheck disable=SC2086
    $CC -static -Os -Wall $DEFS -o "$OUT" "$ROOT/bootkit/ptrace_init.c"
    echo "built: $OUT ($(wc -c < "$OUT") bytes), preload.so ($(wc -c < "$ROOT/bootkit/preload.so") bytes)"

    # 4. the initramfs (the just-built ptrace_init alone, cpio/newc) and its
    #    embedded C array for the EFI loader.
    initrd_cpio "$OUT" "$ROOT/initrd.cpio" "$ROOT/bootloader/initrd_so.h"

    # 5. the EFI loader (installed as \EFI\BOOT\BOOTKIT.EFI; replaces the EFI
    #    shell + startup.nsh trick).  Freestanding x86_64 PE32+ with the
    #    initramfs embedded (initrd_so.h, step 4), no libc and no gnu-efi.
    #
    #    Prefer clang + lld-link (--target=x86_64-unknown-windows); a gcc that
    #    targets PE, e.g. x86_64-w64-mingw32-gcc, works equally well.  Force
    #    one with EFI_CC=<compiler>.
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

    case "$EFI_CC" in
        *clang*)
            "$EFI_CC" --target=x86_64-unknown-windows -ffreestanding -fno-stack-protector \
                -mno-red-zone -mno-sse -fshort-wchar -Os -Wall -Wextra -nostdlib \
                $DEFS \
                -fuse-ld=lld-link \
                -Wl,/subsystem:efi_application,/entry:efi_main,/nodefaultlib \
                -o "$ROOT/bootkit.efi" \
                "$ROOT/bootloader/efi_main.c" "$ROOT/bootloader/efi.c" \
                "$ROOT/bootloader/boot.c" "$ROOT/bootloader/config.c" \
                "$ROOT/bootloader/vars.c" "$ROOT/bootloader/installer.c"
            ;;
        *)
            EFI_OBJ=$(mktemp -d)
            if ! $EFI_CC -dumpmachine 2>/dev/null | grep -q mingw; then
                rm -rf "$EFI_OBJ"
                echo "ERROR: $EFI_CC does not target PE/Windows; use clang" >&2
                echo "       (--target=x86_64-unknown-windows) or" >&2
                echo "       x86_64-w64-mingw32-gcc." >&2
                exit 1
            fi
            for s in efi_main efi boot config vars installer; do
                $EFI_CC -c -ffreestanding -fshort-wchar -fno-stack-protector \
                    -mno-red-zone -mno-sse -fno-asynchronous-unwind-tables \
                    -Os -Wall -Wextra $DEFS \
                    -I "$ROOT/bootloader" \
                    -o "$EFI_OBJ/$s.o" "$ROOT/bootloader/$s.c"
            done
            $EFI_CC -nostdlib -Wl,--subsystem,10 -Wl,-e,efi_main \
                -Wl,--no-insert-timestamp \
                -o "$ROOT/bootkit.efi" "$EFI_OBJ"/*.o
            rm -rf "$EFI_OBJ"
            ;;
    esac
    echo "built: $ROOT/bootkit.efi ($(wc -c < "$ROOT/bootkit.efi") bytes, via $EFI_CC)"

    # 6. bootkit.img: the x86 installer/removable USB stick, a 32 MB MBR disk
    #    with one EFI system partition (FAT, type 0xEF) holding
    #    \EFI\BOOT\BOOTX64.EFI.  Flash it (dd if=bootkit.img of=/dev/sdX
    #    bs=4M conv=fsync); with no \BOOTKIT.CFG on it the loader starts its
    #    direct/removable install menu.
    bootkit_img "$ROOT/bootkit.img" BKINSTALL 0x4d544b42 \
        "$ROOT/bootkit.efi" EFI/BOOT/BOOTX64.EFI
}

# --------------------------------------------------------------- arm64 (CHR)

build_arm64() {
    # The arm64 CHR's userspace is AArch32 (armv7 soft-float), so both the
    # initramfs init and the probe are ARM32:
    #
    #   ptrace_init-arm   static musl; hard-float is fine (it is our own
    #                     PID 1 and executes no FP code)
    #   preload-arm.so    -mfloat-abi=soft -nostdlib, matching the RouterOS
    #                     processes it is preloaded into
    if ! command -v clang >/dev/null 2>&1 || ! command -v lld-link >/dev/null 2>&1; then
        echo "== arm64 skipped: needs clang + lld-link (AArch64 PE loader)" >&2
        return 1
    fi
    ARM_CC=${ARM_CC:-arm-linux-gnueabihf-gcc}
    CC=$ROOT/.toolchain/arm-musl/bin/musl-gcc
    if [ ! -x "$CC" ]; then
        echo "== ARM musl toolchain missing, running tools/musl_arm.sh"
        if ! ARM_CC="$ARM_CC" "$ROOT/tools/musl_arm.sh"; then
            echo "== arm64 skipped: the ARM toolchain could not be built" >&2
            return 1
        fi
    fi
    if [ ! -x "$CC" ] || ! command -v "$ARM_CC" >/dev/null 2>&1; then
        echo "ERROR: arm64 needs the musl arm toolchain and $ARM_CC" >&2
        echo "       (Debian/Ubuntu: gcc-arm-linux-gnueabihf; ARM_CC= overrides)" >&2
        return 1
    fi

    # 1. the ARM32 probe: soft-float, no libc, no DT_NEEDED (same scheme as
    #    the i386 one; the RouterOS arm32 libc is soft-float too).  The musl
    #    headers come from the arm toolchain installed above.
    ARM_INC=$ROOT/.toolchain/arm-musl/include
    echo "== arm32 compiler: $ARM_CC (probe soft-float, sysroot $ROOT/.toolchain/arm-musl)"
    # shellcheck disable=SC2086
    "$ARM_CC" -mfloat-abi=soft -march=armv7-a -nostdinc -isystem "$ARM_INC" \
        -shared -fPIC -nostdlib -Os -fno-stack-protector -fvisibility=hidden \
        -ffunction-sections -fdata-sections -Wl,--gc-sections \
        $DEFS -o "$ROOT/bootkit/preload-arm.so" "$ROOT/bootkit/preload.c"

    # 2. embed it into the ARM init binary
    embed "$ROOT/bootkit/preload-arm.so" "$ROOT/bootkit/preload_so_arm.h" \
        "preload_so"

    # 3. the ARM init itself (static musl)
    # shellcheck disable=SC2086
    $CC -static -Os -Wall $DEFS -DPRELOAD_SO_HEADER='"preload_so_arm.h"' \
        -o "$ROOT/ptrace_init-arm" "$ROOT/bootkit/ptrace_init.c"
    echo "built: $ROOT/ptrace_init-arm ($(wc -c < "$ROOT/ptrace_init-arm") bytes), preload-arm.so ($(wc -c < "$ROOT/bootkit/preload-arm.so") bytes)"

    # 4. its initramfs (embedded in the AArch64 loader)
    initrd_cpio "$ROOT/ptrace_init-arm" "$ROOT/initrd-arm.cpio" \
        "$ROOT/bootloader/initrd_so_arm.h"

    # 5. the AArch64 EFI loader: freestanding PE32+ (clang + lld-link), the
    #    arm32 initramfs embedded.  boot_arm64.c boots the stock kernel at
    #    \EFI\BOOT\BOOTAA64.EFI through LoadImage/StartImage and serves the
    #    initramfs to the kernel's EFI stub as a file on a RAM-backed volume
    #    (the 5.6 arm64 stub supports "initrd=" from the kernel's own volume;
    #    it has no LoadFile2 initrd support).
    clang --target=aarch64-unknown-windows -ffreestanding -fno-stack-protector \
        -fshort-wchar -Os -Wall -Wextra -nostdlib $DEFS \
        -DINITRD_SO_HEADER='"initrd_so_arm.h"' \
        -fuse-ld=lld-link \
        -Wl,/subsystem:efi_application,/entry:efi_main,/nodefaultlib \
        -o "$ROOT/bootkit-arm64.efi" \
        "$ROOT/bootloader/efi_main.c" "$ROOT/bootloader/efi.c" \
        "$ROOT/bootloader/boot_arm64.c" "$ROOT/bootloader/initrdvol.c" \
        "$ROOT/bootloader/config.c" "$ROOT/bootloader/vars.c" \
        "$ROOT/bootloader/installer.c"
    echo "built: $ROOT/bootkit-arm64.efi ($(wc -c < "$ROOT/bootkit-arm64.efi") bytes, via clang --target=aarch64-unknown-windows)"

    # 6. bootkit-arm64.img: the arm64 installer/removable USB stick, the same
    #    MBR+FAT layout with \EFI\BOOT\BOOTAA64.EFI (the AArch64 removable
    #    media path) holding the loader.
    bootkit_img "$ROOT/bootkit-arm64.img" BKINSTALL64 0x4d544b43 \
        "$ROOT/bootkit-arm64.efi" EFI/BOOT/BOOTAA64.EFI
}

case "$ARCH" in
    x86)   build_x86 ;;
    arm64) build_arm64 ;;
    all)
        build_x86
        build_arm64 || true
        ;;
esac
