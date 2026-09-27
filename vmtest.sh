#!/usr/bin/env bash
# vmtest.sh - prepare, boot and interact with a RouterOS image copy under
# QEMU, with the ptrace_init bootkit installed via its own EFI loader
# (initramfs embedded).  The loader is installed as \EFI\BOOT\BOOTKIT.EFI and
# is reached through a Boot#### entry; the stock kernel stays at
# \EFI\BOOT\BOOTX64.EFI (x86_64) or \EFI\BOOT\BOOTAA64.EFI (arm64) so
# RouterOS updates can overwrite it.
#
# The installer is the bootkit.img / bootkit-arm64.img the build produces (MBR
# disk with a FAT ESP holding the loader at the removable-media path): the
# test boots it as a second disk together with the target image, answers the
# install menu over the serial console, and then boots the target normally.
#
# The source image is never modified: everything happens on $WORK/test.img.
#
# Usage:
#   ./vmtest.sh prepare [IMG|ISO|ZIP]
#                               build the bootkit, make the test image, copy
#                               the arch's boot image as the stick and start
#                               from a fresh varstore; an installer ISO (x86)
#                               is installed to $WORK/clean.img first, an arm64
#                               CHR *.img.zip is extracted
#   ./vmtest.sh iso-install [ISO] [OUT]
#                               install RouterOS from an installer ISO onto a
#                               fresh disk image (the clean image the tests
#                               start from; its first boot sets admin/admin)
#   ./vmtest.sh install         boot stick + target, run the installer
#   ./vmtest.sh boot            start QEMU in the background (target only)
#   ./vmtest.sh boot-removable  boot the stick + target (stick first)
#   ./vmtest.sh wait [SECS]     wait for the login prompt in the serial log
#   ./vmtest.sh check           show the bootkit result lines
#   ./vmtest.sh login           attach to the serial console (Ctrl-] to quit)
#   ./vmtest.sh cmd "<command>" log in as admin/admin and run one command
#   ./vmtest.sh stop            stop QEMU
#   ./vmtest.sh test            prepare + install + boot + wait + check
#   ./vmtest.sh test-removable  prepare + install (mode 2) + boot-removable +
#                               wait + check
#
# Environment:
#   WORK=$ROOT/.work            scratch directory (image copy, logs, pidfile)
#   ARCH=auto|x86_64|arm64      target architecture (default auto: arm64 when
#                               IMG_SRC names an arm64 image, else x86_64)
#   IMG_SRC=<path>              source image (default: x86-7.24.4-clean.img,
#                               or chr-7.24.4-arm64.img for ARCH=arm64); an
#                               *.iso is installed first (see iso-install), an
#                               *.img.zip is extracted
#   ISO_SRC=<path>              installer ISO for "iso-install" (default: the
#                               newest mikrotik-*.iso next to this script)
#   ISO_OUT=<path>              image "iso-install" writes (default:
#                               $WORK/clean.img)
#   IMG_SIZE=128M               size of that fresh image
#   MODE=chr|x86|keep           override the MBR mode flag (default: keep; the
#                               arm64 CHR image is already CHR mode)
#   INSTALL_MODE=1|2            installer mode for "install": 1 direct
#                               (default), 2 removable
#   DEBUG=0|1                   build variant (default 1 in the harness: serial
#                               console + verbose tracer/probe logs)
#   ACCEL=kvm|tcg               QEMU accelerator (default: kvm on x86_64, tcg
#                               on arm64; use tcg where there is no /dev/kvm,
#                               e.g. hosted CI runners)
#   OVMF_CODE=<path> OVMF_VARS=<path>
#                               x86_64 UEFI firmware (default: the first
#                               edk2-ovmf / ovmf pair of paths that exists)
#   AAVMF_CODE=<path> AAVMF_VARS=<path>
#                               arm64 UEFI firmware (default: the first
#                               edk2-aarch64 / AAVMF pair that exists; both
#                               files must be the same size)
#   MEM=1024  SMP=2             QEMU memory / cpus
#   WAIT=90                     seconds "test" waits for the login prompt
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$ROOT/.work}
ARCH=${ARCH:-auto}
IMG=$WORK/test.img
ESP_OFF=1048576                      # partition 1 starts at LBA 2048
MODE=${MODE:-keep}
MEM=${MEM:-1024}
SMP=${SMP:-2}
DEBUG=${DEBUG:-1}
IMG_SIZE=${IMG_SIZE:-128M}
WAIT=${WAIT:-90}

SERIAL_SOCK=$WORK/serial.sock
SERIAL_LOG=$WORK/serial.log
PIDFILE=$WORK/qemu.pid

log() { printf '%s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# architecture: explicit, or guessed from an arm64 image name
IMG_SRC=${IMG_SRC:-}
case "$ARCH" in
    auto)
        case "$IMG_SRC" in
            *arm64*) ARCH=arm64 ;;
            *)       ARCH=x86_64 ;;
        esac
        ;;
    x86|x86_64)    ARCH=x86_64 ;;
    arm64|aarch64) ARCH=arm64 ;;
    *) die "ARCH must be auto, x86_64 or arm64" ;;
esac
if [ "$ARCH" = arm64 ]; then
    IMG_SRC=${IMG_SRC:-$ROOT/chr-7.24.4-arm64.img}
    STICK=$WORK/stick-arm64.img
    STICK_IMG=$ROOT/bootkit-arm64.img
    KERNEL_FILE=BOOTAA64.EFI
    ACCEL=${ACCEL:-tcg}
else
    IMG_SRC=${IMG_SRC:-$ROOT/x86-7.24.4-clean.img}
    STICK=$WORK/stick.img
    STICK_IMG=$ROOT/bootkit.img
    KERNEL_FILE=BOOTX64.EFI
    ACCEL=${ACCEL:-kvm}
fi

case "$ACCEL" in
    kvm) ;;
    tcg) ;;
    *)   die "ACCEL must be kvm or tcg" ;;
esac

if [ "$ARCH" = arm64 ]; then
    # arm64 UEFI firmware: AAVMF_CODE/AAVMF_VARS override; otherwise the first
    # pair that exists.  QEMU's virt pflash banks are 64 MiB, so only a
    # matching code/vars pair works (Ubuntu also ships a compact 2 MiB
    # QEMU_EFI.fd used by some setups; it is not listed on purpose).
    if [ -z "${AAVMF_CODE:-}" ] || [ -z "${AAVMF_VARS:-}" ]; then
        for pair in \
            /usr/share/edk2/aarch64/QEMU_EFI.fd:/usr/share/edk2/aarch64/QEMU_VARS.fd \
            /usr/share/AAVMF/AAVMF_CODE.fd:/usr/share/AAVMF/AAVMF_VARS.fd \
            /usr/share/qemu-efi-aarch64/QEMU_EFI.fd:/usr/share/qemu-efi-aarch64/QEMU_VARS.fd; do
            code=${pair%%:*}; vars=${pair##*:}
            if [ -f "$code" ] && [ -f "$vars" ] &&
               [ "$(stat -c %s "$code")" = "$(stat -c %s "$vars")" ]; then
                AAVMF_CODE=${AAVMF_CODE:-$code}
                AAVMF_VARS=${AAVMF_VARS:-$vars}
                break
            fi
        done
    fi
    [ -f "${AAVMF_CODE:-}" ] && [ -f "${AAVMF_VARS:-}" ] &&
        [ "$(stat -c %s "${AAVMF_CODE:-/dev/null}")" = "$(stat -c %s "${AAVMF_VARS:-/dev/null}")" ] ||
        die "arm64 UEFI firmware not found; set AAVMF_CODE and AAVMF_VARS (same size)"
    FW_CODE=$AAVMF_CODE
    FW_VARS=$AAVMF_VARS
    QEMU=$(command -v qemu-system-aarch64) || die "qemu-system-aarch64 not found"
    # cortex-a72 is the CPU the CHR arm64 kernel likes (-cpu max hangs it)
    if [ "$ACCEL" = kvm ]; then
        QEMU_ACCEL=(-machine virt -accel kvm -cpu host)
    else
        QEMU_ACCEL=(-machine virt -accel tcg -cpu cortex-a72)
    fi
else
    # UEFI firmware: OVMF_CODE/OVMF_VARS override; otherwise the first pair
    # that exists (Arch's edk2-ovmf or Debian/Ubuntu's ovmf)
    if [ -z "${OVMF_CODE:-}" ] || [ -z "${OVMF_VARS:-}" ]; then
        for pair in \
            /usr/share/edk2/x64/OVMF_CODE.4m.fd:/usr/share/edk2/x64/OVMF_VARS.4m.fd \
            /usr/share/OVMF/OVMF_CODE_4M.fd:/usr/share/OVMF/OVMF_VARS_4M.fd \
            /usr/share/OVMF/OVMF_CODE.fd:/usr/share/OVMF/OVMF_VARS.fd; do
            code=${pair%%:*}; vars=${pair##*:}
            if [ -f "$code" ] && [ -f "$vars" ]; then
                OVMF_CODE=${OVMF_CODE:-$code}
                OVMF_VARS=${OVMF_VARS:-$vars}
                break
            fi
        done
    fi
    [ -f "${OVMF_CODE:-}" ] && [ -f "${OVMF_VARS:-}" ] ||
        die "UEFI firmware not found; set OVMF_CODE and OVMF_VARS"
    FW_CODE=$OVMF_CODE
    FW_VARS=$OVMF_VARS
    QEMU=$(command -v qemu-system-x86_64) || die "qemu-system-x86_64 not found"
    # -cpu host needs KVM; -cpu max is the usual emulated (TCG) choice
    if [ "$ACCEL" = kvm ]; then
        QEMU_ACCEL=(-accel kvm -cpu host)
    else
        QEMU_ACCEL=(-accel tcg -cpu max)
    fi
fi

# the extra controller the x86 commands need for their usb-storage disks
usb_bus() {
    [ "$ARCH" = x86_64 ] && printf '%s' '-device qemu-xhci,id=usb-bus'
    return 0
}

# one disk: <drive id> <serial> [bootindex]
disk_dev() {
    if [ "$ARCH" = arm64 ]; then
        if [ -n "${3:-}" ]; then
            printf '%s' "-device virtio-blk-pci,drive=$1,serial=$2,bootindex=$3"
        else
            printf '%s' "-device virtio-blk-pci,drive=$1,serial=$2"
        fi
    else
        if [ -n "${3:-}" ]; then
            printf '%s' "-device usb-storage,bus=usb-bus.0,drive=$1,serial=$2,bootindex=$3"
        else
            printf '%s' "-device usb-storage,bus=usb-bus.0,drive=$1,serial=$2"
        fi
    fi
}

qemu_cmd() {
    # the $(...) helpers emit several words on purpose
    # shellcheck disable=SC2046
    echo "$QEMU" -m "$MEM" -smp "$SMP" "${QEMU_ACCEL[@]}" \
        -drive if=none,id=d1,file="$IMG",format=raw \
        $(usb_bus) $(disk_dev d1 test) \
        -drive if=pflash,format=raw,readonly=on,file="$FW_CODE" \
        -drive if=pflash,format=raw,file="$WORK/vars.fd" \
        -display none \
        -chardev "socket,id=ser,path=$SERIAL_SOCK,server=on,wait=off,logfile=$SERIAL_LOG" \
        -serial chardev:ser
}

# same, plus the installer stick as the second disk (target stays on drive 1 so
# the device path in the created boot entry stays valid later)
qemu_cmd_install() {
    # the $(...) helpers emit several words on purpose
    # shellcheck disable=SC2046
    echo "$QEMU" -m "$MEM" -smp "$SMP" "${QEMU_ACCEL[@]}" \
        -drive if=none,id=d1,file="$IMG",format=raw \
        -drive if=none,id=d2,file="$STICK",format=raw \
        $(usb_bus) $(disk_dev d1 test) $(disk_dev d2 bk-stick) \
        -drive if=pflash,format=raw,readonly=on,file="$FW_CODE" \
        -drive if=pflash,format=raw,file="$WORK/vars.fd" \
        -display none \
        -chardev "socket,id=ser,path=$SERIAL_SOCK,server=on,wait=off,logfile=$SERIAL_LOG" \
        -serial chardev:ser
}

# stick + target with the stick first (bootindex), for the removable install:
# the stick's loader reads its config and boots the kernel on the target disk
qemu_cmd_removable() {
    # the $(...) helpers emit several words on purpose
    # shellcheck disable=SC2046
    echo "$QEMU" -m "$MEM" -smp "$SMP" "${QEMU_ACCEL[@]}" \
        -drive if=none,id=d1,file="$IMG",format=raw \
        -drive if=none,id=d2,file="$STICK",format=raw \
        $(usb_bus) $(disk_dev d1 test 1) $(disk_dev d2 bk-stick 0) \
        -drive if=pflash,format=raw,readonly=on,file="$FW_CODE" \
        -drive if=pflash,format=raw,file="$WORK/vars.fd" \
        -display none \
        -chardev "socket,id=ser,path=$SERIAL_SOCK,server=on,wait=off,logfile=$SERIAL_LOG" \
        -serial chardev:ser
}

# the stock RouterOS installer ISO as a CD-ROM, the fresh target on drive 1
# (same device as in the boot tests, so the installed image matches) and a
# separate varstore; the ISO's EFI boot image (refind) boots the installer
# under OVMF, which is what makes it create the FAT EFI partition layout
qemu_cmd_iso() {
    # the $(...) helpers emit several words on purpose
    # shellcheck disable=SC2046
    echo "$QEMU" -m "$MEM" -smp "$SMP" "${QEMU_ACCEL[@]}" \
        -boot order=d \
        -cdrom "$WORK/ros-install.iso" \
        -drive if=none,id=d1,file="$1",format=raw \
        $(usb_bus) $(disk_dev d1 test) \
        -drive if=pflash,format=raw,readonly=on,file="$FW_CODE" \
        -drive if=pflash,format=raw,file="$WORK/iso-vars.fd" \
        -display none \
        -chardev "socket,id=ser,path=$SERIAL_SOCK,server=on,wait=off,logfile=$SERIAL_LOG" \
        -serial chardev:ser
}

# the freshly installed system on its own (no CD, no stick): the firmware's
# removable-media fallback boots the stock kernel from the target, which is
# what a stock image does; used for the first login of a new image
qemu_cmd_stock() {
    # the $(...) helpers emit several words on purpose
    # shellcheck disable=SC2046
    echo "$QEMU" -m "$MEM" -smp "$SMP" "${QEMU_ACCEL[@]}" \
        -drive if=none,id=d1,file="$1",format=raw \
        $(usb_bus) $(disk_dev d1 test) \
        -drive if=pflash,format=raw,readonly=on,file="$FW_CODE" \
        -drive if=pflash,format=raw,file="$WORK/iso-vars.fd" \
        -display none \
        -chardev "socket,id=ser,path=$SERIAL_SOCK,server=on,wait=off,logfile=$SERIAL_LOG" \
        -serial chardev:ser
}

# newest installer ISO next to this script (or $ISO_SRC)
default_iso() {
    local f
    f=$(find "$ROOT" -maxdepth 1 -name 'mikrotik-*.iso' 2>/dev/null |
        sort -V | tail -n1 || true)
    [ -n "$f" ] || die "no installer ISO found, pass one or set ISO_SRC"
    printf '%s\n' "$f"
}

# byte offset of the ISO's EFI boot image (efiboot.img): the installer ISO
# boots it through El Torito and its \EFI\BOOT\refind.conf holds the kernel
# options refind passes to the installer
iso_efi_offset() {
    local iso=$1 lba=''
    lba=$( { isoinfo -i "$iso" -l -R 2>/dev/null || true; } |
           sed -n 's/.*\[ *\([0-9][0-9]*\) .*\] *efiboot\.img.*/\1/p' | head -n1)
    if [ -z "$lba" ] && command -v xorriso >/dev/null 2>&1; then
        lba=$( { xorriso -indev "$iso" -find / -exec report_lba -- 2>/dev/null || true; } |
               awk '$NF=="/efiboot.img" {print $6; exit}')
    fi
    [ -n "$lba" ] || die "cannot locate /efiboot.img in $iso (need isoinfo or xorriso)"
    echo $((lba * 2048))
}

# copy the ISO and patch the EFI boot config: add "console=ttyS0,115200n8" to
# the options refind passes to the installer kernel (so the installer talks to
# the serial socket) and a short timeout (so it boots without a keypress).
# Only the bytes of refind.conf inside the EFI boot image change.
make_install_iso() {
    local src=$1 dst=$2 off conf=$WORK/iso-refind.conf
    off=$(iso_efi_offset "$src")
    export MTOOLS_SKIP_CHECK=1
    mcopy -o -i "$src@@$off" ::/EFI/BOOT/refind.conf "$conf" 2>/dev/null ||
        die "cannot read /EFI/BOOT/refind.conf from $src (unsupported ISO)"
    grep -q 'console=ttyS0' "$conf" ||
        sed -i -E 's/^([[:space:]]*options[[:space:]]+".*)"$/\1 console=ttyS0,115200n8"/' "$conf"
    if grep -qE '^[[:space:]]*timeout' "$conf"; then
        sed -i -E 's/^[[:space:]]*timeout[[:space:]]+[0-9]+/timeout 5/' "$conf"
    else
        sed -i '1i timeout 5' "$conf"
    fi
    grep -q 'console=ttyS0' "$conf" ||
        die "no refind options line in $src (unsupported ISO)"
    cp -f "$src" "$dst"
    mcopy -o -i "$dst@@$off" "$conf" ::/EFI/BOOT/refind.conf
    log "== installer ISO prepared ($dst)"
}

# Install RouterOS from an installer ISO onto a fresh disk image: boot the ISO
# under OVMF (EFI mode is what makes the installer create the FAT EFI
# partition layout the tests use), drive the installer menu over the serial
# console and wait for it to finish.
cmd_iso_install() {
    local iso=${1:-${ISO_SRC:-$(default_iso)}}
    local out=${2:-${ISO_OUT:-$WORK/clean.img}}
    [ "$ARCH" = x86_64 ] || die "iso-install is only for x86_64 (arm64 CHR comes as an .img.zip)"
    [ -f "$iso" ] || die "installer ISO not found: $iso"
    mkdir -p "$WORK"
    log "== installing RouterOS from $iso"
    make_install_iso "$iso" "$WORK/ros-install.iso"

    rm -f "$out"
    truncate -s "$IMG_SIZE" "$out"

    stop_qemu
    rm -f "$WORK/iso-vars.fd"
    cp -f "$FW_VARS" "$WORK/iso-vars.fd"
    : > "$SERIAL_LOG"
    rm -f "$SERIAL_SOCK"
    # shellcheck disable=SC2046
    $(qemu_cmd_iso "$out") -daemonize -pidfile "$PIDFILE"

    if ! python3 "$ROOT/vmconsole.py" "$SERIAL_SOCK" --ros-install; then
        stop_qemu
        rm -f "$out"
        die "RouterOS install from $iso failed"
    fi
    stop_qemu

    # first boot of the installed system: the admin password is still empty
    # and RouterOS wants the licence question answered, the agreement pager
    # dismissed and the password changed; leave the image with admin/admin
    # like the older hand-made clean images
    log "== first boot (licence, admin/admin)"
    rm -f "$WORK/iso-vars.fd"
    cp -f "$FW_VARS" "$WORK/iso-vars.fd"
    : > "$SERIAL_LOG"
    rm -f "$SERIAL_SOCK"
    # shellcheck disable=SC2046
    $(qemu_cmd_stock "$out") -daemonize -pidfile "$PIDFILE"
    if ! python3 "$ROOT/vmconsole.py" "$SERIAL_SOCK" --ros-firstboot; then
        stop_qemu
        rm -f "$out"
        die "first login of $out failed"
    fi
    stop_qemu
    log "== clean image ready: $out ($IMG_SIZE)"
}

# the image prepare copies from: an installer ISO is first turned into a clean
# image (cached in $WORK/clean.img; rebuild it with ./vmtest.sh iso-install),
# a CHR *.img.zip is unpacked once into $WORK
source_image() {
    local src=${1:-$IMG_SRC} clean=${ISO_OUT:-$WORK/clean.img} out d img
    case "$src" in
        *.iso)
            if [ -f "$clean" ]; then
                log "== using the existing clean image $clean"
                log "   (rebuild it with: $0 iso-install $src)"
            else
                cmd_iso_install "$src" "$clean" >&2
            fi
            printf '%s\n' "$clean" ;;
        *.zip)
            out="$WORK/$(basename "${src%.zip}")"
            if [ ! -f "$out" ]; then
                command -v unzip >/dev/null 2>&1 || die "unzip is needed for $src"
                mkdir -p "$WORK"
                log "== extracting $src"
                d=$(mktemp -d "$WORK/unzip.XXXXXX")
                unzip -q -o "$src" -d "$d" >&2 || { rm -rf "$d"; die "cannot extract $src"; }
                img=$(find "$d" -name '*.img' -type f | head -n1)
                [ -n "$img" ] || { rm -rf "$d"; die "no .img inside $src"; }
                mv "$img" "$out"
                rm -rf "$d"
            fi
            printf '%s\n' "$out" ;;
        *)  printf '%s\n' "$src" ;;
    esac
}

cmd_prepare() {
    IMG_SRC=$(source_image "${1:-}")
    [ -f "$IMG_SRC" ] || die "source image not found: $IMG_SRC"
    log "== building bootkit"
    local build_arch=x86
    [ "$ARCH" = arm64 ] && build_arch=arm64
    (cd "$ROOT" && DEBUG="$DEBUG" ARCH="$build_arch" ./build.sh)

    mkdir -p "$WORK"
    log "== copying image ($IMG_SRC -> $IMG)"
    rm -f "$IMG.sha" 2>/dev/null || true
    cp -f "$IMG_SRC" "$IMG"

    case "$MODE" in
        chr)  printf '\001' | dd of="$IMG" bs=1 seek=$((0x150)) conv=notrunc status=none
              log "== MBR mode flag -> CHR" ;;
        x86)  printf '\000' | dd of="$IMG" bs=1 seek=$((0x150)) conv=notrunc status=none
              log "== MBR mode flag -> x86" ;;
        keep) ;;
        *)    die "MODE must be chr, x86 or keep" ;;
    esac

    [ -f "$STICK_IMG" ] || die "$STICK_IMG not found (build it with ARCH=arm64/x86 ./build.sh)"
    log "== using the boot image ($STICK_IMG -> $STICK)"
    export MTOOLS_SKIP_CHECK=1
    cp -f "$STICK_IMG" "$STICK"

    log "== fresh varstore (the installer creates the boot entry)"
    rm -f "$WORK/vars.fd"
    cp -f "$FW_VARS" "$WORK/vars.fd"
    log "== ready ($ARCH): $IMG, $STICK"
}

# Boot the installer stick together with the target and drive the installer
# over the serial console.  The target's kernel is moved aside for the run so
# the stick is the only bootable medium; it is restored afterwards.
cmd_install() {
    [ -f "$IMG" ] || die "no test image, run: $0 prepare"
    [ -f "$STICK" ] || die "no installer stick, run: $0 prepare"
    local esp="$IMG@@$ESP_OFF"

    export MTOOLS_SKIP_CHECK=1
    log "== running the installer ($KERNEL_FILE moved aside)"
    mcopy -o -i "$esp" "::/EFI/BOOT/$KERNEL_FILE" "$WORK/kernel.bak"
    mcopy -o -i "$esp" "$WORK/kernel.bak" "::/EFI/BOOT/${KERNEL_FILE%.EFI}.BAK"
    mdel -i "$esp" "::/EFI/BOOT/$KERNEL_FILE"

    stop_qemu
    rm -f "$WORK/vars.fd"
    cp -f "$FW_VARS" "$WORK/vars.fd"
    : > "$SERIAL_LOG"
    rm -f "$SERIAL_SOCK"
    # shellcheck disable=SC2046
    $(qemu_cmd_install) -daemonize -pidfile "$PIDFILE"

    if ! python3 "$ROOT/vmconsole.py" "$SERIAL_SOCK" --install "${INSTALL_MODE:-1}"; then
        log "== installer failed, restoring the kernel"
        mcopy -o -i "$esp" "$WORK/kernel.bak" "::/EFI/BOOT/$KERNEL_FILE"
        mdel -i "$esp" "::/EFI/BOOT/${KERNEL_FILE%.EFI}.BAK" 2>/dev/null || true
        die "installer did not complete"
    fi
    stop_qemu

    mcopy -o -i "$esp" "$WORK/kernel.bak" "::/EFI/BOOT/$KERNEL_FILE"
    mdel -i "$esp" "::/EFI/BOOT/${KERNEL_FILE%.EFI}.BAK" 2>/dev/null || true
    log "== installer finished (mode ${INSTALL_MODE:-1})"
}

cmd_boot() {
    [ -f "$IMG" ] || die "no test image, run: $0 prepare"
    stop_qemu
    if [ ! -f "$WORK/vars.fd" ]; then
        cp -f "$FW_VARS" "$WORK/vars.fd"
        log "== fresh varstore (no boot entry; run: $0 install)"
    fi
    : > "$SERIAL_LOG"
    rm -f "$SERIAL_SOCK"
    # shellcheck disable=SC2046
    $(qemu_cmd) -daemonize -pidfile "$PIDFILE"
    log "== qemu started (pid $(cat "$PIDFILE"))"
}

# removable install case: boot the stick (first) with the target attached; the
# stick's loader follows its config and boots the kernel on the target
cmd_boot_removable() {
    [ -f "$IMG" ] || die "no test image, run: $0 prepare"
    [ -f "$STICK" ] || die "no installer stick, run: $0 prepare"
    stop_qemu
    : > "$SERIAL_LOG"
    rm -f "$SERIAL_SOCK"
    # shellcheck disable=SC2046
    $(qemu_cmd_removable) -daemonize -pidfile "$PIDFILE"
    log "== qemu started (removable, pid $(cat "$PIDFILE"))"
}

stop_qemu() {
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; then
        kill "$(cat "$PIDFILE")"
        for _ in $(seq 1 30); do
            kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null || break
            sleep 0.1
        done
    fi
    rm -f "$PIDFILE"
}

cmd_stop() { stop_qemu; log "== stopped"; }

cmd_wait() {
    local timeout=${1:-60} t0
    t0=$(date +%s)
    while [ $(( $(date +%s) - t0 )) -lt "$timeout" ]; do
        if grep -aqE 'Login:|login:' "$SERIAL_LOG" 2>/dev/null; then
            log "== login prompt after $(( $(date +%s) - t0 ))s"
            return 0
        fi
        sleep 0.5
    done
    log "== no login prompt after ${timeout}s"
    return 1
}

cmd_check() {
    echo "--- loader:"
    grep -aE 'BdsDxe: (loading|starting) Boot.*MikroTikBootKit|efiboot:' "$SERIAL_LOG" || echo "(no loader lines!)"
    echo "--- tracer:"
    grep -a 'ptrace-init' "$SERIAL_LOG" || echo "(no ptrace-init lines!)"
    echo "--- preload loads:"
    grep -ac 'ldpreload. loaded by' "$SERIAL_LOG" 2>/dev/null || true
    grep -a 'ldpreload' "$SERIAL_LOG" | tail -60 || true
    echo "--- last lines:"
    tail -c 400 "$SERIAL_LOG" | strings | tail -12
}

cmd_login() {
    [ -S "$SERIAL_SOCK" ] || die "no serial socket, is qemu running?"
    log "== attached, Ctrl-] quits"
    exec socat -,raw,echo=0 "UNIX-CONNECT:$SERIAL_SOCK"
}

cmd_cmd() {
    [ -n "${1:-}" ] || die "usage: $0 cmd \"<routeros command>\""
    [ -S "$SERIAL_SOCK" ] || die "no serial socket, is qemu running?"
    python3 "$ROOT/vmconsole.py" "$SERIAL_SOCK" "$1"
}

cmd_test() {
    cmd_prepare
    cmd_install
    cmd_boot
    cmd_wait "$WAIT" || true
    cmd_check
}

cmd_test_removable() {
    cmd_prepare
    INSTALL_MODE=2 cmd_install
    cmd_boot_removable
    cmd_wait "$WAIT" || true
    cmd_check
}

case "${1:-}" in
    prepare) shift; cmd_prepare "$@" ;;
    iso-install) shift; cmd_iso_install "$@" ;;
    install) cmd_install ;;
    boot)    cmd_boot ;;
    boot-removable) cmd_boot_removable ;;
    wait)    shift; cmd_wait "$@" ;;
    check)   cmd_check ;;
    login)   cmd_login ;;
    cmd)     shift; cmd_cmd "$@" ;;
    stop)    cmd_stop ;;
    test)    cmd_test ;;
    test-removable) cmd_test_removable ;;
    *)       sed -n '2,/^set -euo pipefail/p' "$0"; exit 1 ;;
esac
