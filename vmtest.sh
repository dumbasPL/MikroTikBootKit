#!/usr/bin/env bash
# vmtest.sh - prepare, boot and interact with a RouterOS x86 image copy under
# QEMU/KVM, with the ptrace_init bootkit installed via its own EFI loader
# (bootkit.efi, initramfs embedded).  The loader is installed as
# \EFI\BOOT\BOOTKIT.EFI and is reached through a Boot#### entry; the stock
# kernel stays at \EFI\BOOT\BOOTX64.EFI so RouterOS updates can overwrite it.
#
# The installer is the bootkit.img the build produces (MBR disk with a FAT ESP
# holding the loader as \EFI\BOOT\BOOTX64.EFI): the test boots it as a USB
# stick together with the target image, answers the install menu over the
# serial console, and then boots the target normally.
#
# The source image is never modified: everything happens on $WORK/test.img.
#
# Usage:
#   ./vmtest.sh prepare [IMG|ISO]
#                               build the bootkit (and bootkit.img), make the
#                               test image, copy the boot image as the stick
#                               and start from a fresh varstore; an installer
#                               ISO is installed to $WORK/clean.img first
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
#   IMG_SRC=<path>              source image (default: x86-7.24.4-clean.img);
#                               an *.iso is installed first (see iso-install)
#   ISO_SRC=<path>              installer ISO for "iso-install" (default: the
#                               newest mikrotik-*.iso next to this script)
#   ISO_OUT=<path>              image "iso-install" writes (default:
#                               $WORK/clean.img)
#   IMG_SIZE=128M               size of that fresh image
#   MODE=chr|x86|keep           override the MBR mode flag (default: keep)
#   INSTALL_MODE=1|2            installer mode for "install": 1 direct
#                               (default), 2 removable
#   DEBUG=0|1                   build variant (default 1 in the harness: serial
#                               console + verbose tracer/probe logs)
#   MEM=1024  SMP=2             QEMU memory / cpus
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$ROOT/.work}
IMG_SRC=${IMG_SRC:-$ROOT/x86-7.24.4-clean.img}
IMG=$WORK/test.img
STICK=$WORK/stick.img
ESP_OFF=1048576                      # partition 1 starts at LBA 2048
MODE=${MODE:-keep}
MEM=${MEM:-1024}
SMP=${SMP:-2}
DEBUG=${DEBUG:-1}
IMG_SIZE=${IMG_SIZE:-128M}

SERIAL_SOCK=$WORK/serial.sock
SERIAL_LOG=$WORK/serial.log
PIDFILE=$WORK/qemu.pid

log() { printf '%s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

qemu_cmd() {
    echo qemu-system-x86_64 -m "$MEM" -smp "$SMP" -cpu host -enable-kvm \
        -drive if=none,id=d1,file="$IMG",format=raw \
        -device qemu-xhci,id=usb-bus \
        -device usb-storage,bus=usb-bus.0,drive=d1,serial=test \
        -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
        -drive if=pflash,format=raw,file="$WORK/vars.fd" \
        -display none \
        -chardev "socket,id=ser,path=$SERIAL_SOCK,server=on,wait=off,logfile=$SERIAL_LOG" \
        -serial chardev:ser
}

# same, plus the installer stick as the second USB drive (target stays on USB
# port 1 so the device path in the created boot entry stays valid later)
qemu_cmd_install() {
    echo qemu-system-x86_64 -m "$MEM" -smp "$SMP" -cpu host -enable-kvm \
        -drive if=none,id=d1,file="$IMG",format=raw \
        -drive if=none,id=d2,file="$STICK",format=raw \
        -device qemu-xhci,id=usb-bus \
        -device usb-storage,bus=usb-bus.0,drive=d1,serial=test \
        -device usb-storage,bus=usb-bus.0,drive=d2,serial=bk-stick \
        -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
        -drive if=pflash,format=raw,file="$WORK/vars.fd" \
        -display none \
        -chardev "socket,id=ser,path=$SERIAL_SOCK,server=on,wait=off,logfile=$SERIAL_LOG" \
        -serial chardev:ser
}

# stick + target with the stick first (bootindex), for the removable install:
# the stick's loader reads its config and boots the kernel on the target disk
qemu_cmd_removable() {
    echo qemu-system-x86_64 -m "$MEM" -smp "$SMP" -cpu host -enable-kvm \
        -drive if=none,id=d1,file="$IMG",format=raw \
        -drive if=none,id=d2,file="$STICK",format=raw \
        -device qemu-xhci,id=usb-bus \
        -device usb-storage,bus=usb-bus.0,drive=d1,serial=test,bootindex=1 \
        -device usb-storage,bus=usb-bus.0,drive=d2,serial=bk-stick,bootindex=0 \
        -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
        -drive if=pflash,format=raw,file="$WORK/vars.fd" \
        -display none \
        -chardev "socket,id=ser,path=$SERIAL_SOCK,server=on,wait=off,logfile=$SERIAL_LOG" \
        -serial chardev:ser
}

# the stock RouterOS installer ISO as a CD-ROM, the fresh target on USB port 1
# (same device as in the boot tests, so the installed image matches) and a
# separate varstore; the ISO's EFI boot image (refind) boots the installer
# under OVMF, which is what makes it create the FAT EFI partition layout
qemu_cmd_iso() {
    echo qemu-system-x86_64 -m "$MEM" -smp "$SMP" -cpu host -enable-kvm \
        -boot order=d \
        -cdrom "$WORK/ros-install.iso" \
        -drive if=none,id=d1,file="$1",format=raw \
        -device qemu-xhci,id=usb-bus \
        -device usb-storage,bus=usb-bus.0,drive=d1,serial=test \
        -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
        -drive if=pflash,format=raw,file="$WORK/iso-vars.fd" \
        -display none \
        -chardev "socket,id=ser,path=$SERIAL_SOCK,server=on,wait=off,logfile=$SERIAL_LOG" \
        -serial chardev:ser
}

# the freshly installed system on its own (no CD, no stick): OVMF's
# removable-media fallback boots \EFI\BOOT\BOOTX64.EFI from the target, which
# is what the stock image does; used for the first login of a new image
qemu_cmd_stock() {
    echo qemu-system-x86_64 -m "$MEM" -smp "$SMP" -cpu host -enable-kvm \
        -drive if=none,id=d1,file="$1",format=raw \
        -device qemu-xhci,id=usb-bus \
        -device usb-storage,bus=usb-bus.0,drive=d1,serial=test \
        -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
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
    [ -f "$iso" ] || die "installer ISO not found: $iso"
    [ -f /usr/share/edk2/x64/OVMF_CODE.4m.fd ] || die "OVMF not found (edk2-ovmf)"
    mkdir -p "$WORK"
    log "== installing RouterOS from $iso"
    make_install_iso "$iso" "$WORK/ros-install.iso"

    rm -f "$out"
    truncate -s "$IMG_SIZE" "$out"

    stop_qemu
    rm -f "$WORK/iso-vars.fd"
    cp -f /usr/share/edk2/x64/OVMF_VARS.4m.fd "$WORK/iso-vars.fd"
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
    cp -f /usr/share/edk2/x64/OVMF_VARS.4m.fd "$WORK/iso-vars.fd"
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
# image (cached in $WORK/clean.img; rebuild it with ./vmtest.sh iso-install)
source_image() {
    local src=${1:-$IMG_SRC} clean=${ISO_OUT:-$WORK/clean.img}
    case "$src" in
        *.iso)
            if [ -f "$clean" ]; then
                log "== using the existing clean image $clean"
                log "   (rebuild it with: $0 iso-install $src)"
            else
                cmd_iso_install "$src" "$clean"
            fi
            printf '%s\n' "$clean" ;;
        *)  printf '%s\n' "$src" ;;
    esac
}

cmd_prepare() {
    IMG_SRC=$(source_image "${1:-}")
    [ -f "$IMG_SRC" ] || die "source image not found: $IMG_SRC"
    log "== building bootkit"
    (cd "$ROOT" && DEBUG="$DEBUG" ./build.sh)

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

    log "== using the boot image ($ROOT/bootkit.img -> $STICK)"
    export MTOOLS_SKIP_CHECK=1
    cp -f "$ROOT/bootkit.img" "$STICK"

    log "== fresh varstore (the installer creates the boot entry)"
    rm -f "$WORK/vars.fd"
    cp -f /usr/share/edk2/x64/OVMF_VARS.4m.fd "$WORK/vars.fd"
    log "== ready: $IMG, $STICK"
}

# Boot the installer stick together with the target and drive the installer
# over the serial console.  The target's kernel is moved aside for the run so
# the stick is the only bootable medium; it is restored afterwards.
cmd_install() {
    [ -f "$IMG" ] || die "no test image, run: $0 prepare"
    [ -f "$STICK" ] || die "no installer stick, run: $0 prepare"
    local esp="$IMG@@$ESP_OFF"

    export MTOOLS_SKIP_CHECK=1
    log "== running the installer"
    mcopy -o -i "$esp" ::/EFI/BOOT/BOOTX64.EFI "$WORK/kernel.bak"
    mcopy -o -i "$esp" "$WORK/kernel.bak" ::/EFI/BOOT/BOOTX64.BAK
    mdel -i "$esp" ::/EFI/BOOT/BOOTX64.EFI

    stop_qemu
    rm -f "$WORK/vars.fd"
    cp -f /usr/share/edk2/x64/OVMF_VARS.4m.fd "$WORK/vars.fd"
    : > "$SERIAL_LOG"
    rm -f "$SERIAL_SOCK"
    # shellcheck disable=SC2046
    $(qemu_cmd_install) -daemonize -pidfile "$PIDFILE"

    if ! python3 "$ROOT/vmconsole.py" "$SERIAL_SOCK" --install "${INSTALL_MODE:-1}"; then
        log "== installer failed, restoring the kernel"
        mcopy -o -i "$esp" "$WORK/kernel.bak" ::/EFI/BOOT/BOOTX64.EFI
        mdel -i "$esp" ::/EFI/BOOT/BOOTX64.BAK 2>/dev/null || true
        die "installer did not complete"
    fi
    stop_qemu

    mcopy -o -i "$esp" "$WORK/kernel.bak" ::/EFI/BOOT/BOOTX64.EFI
    mdel -i "$esp" ::/EFI/BOOT/BOOTX64.BAK 2>/dev/null || true
    log "== installer finished (mode ${INSTALL_MODE:-1})"
}

cmd_boot() {
    [ -f "$IMG" ] || die "no test image, run: $0 prepare"
    stop_qemu
    if [ ! -f "$WORK/vars.fd" ]; then
        cp -f /usr/share/edk2/x64/OVMF_VARS.4m.fd "$WORK/vars.fd"
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
    cmd_wait 90 || true
    cmd_check
}

cmd_test_removable() {
    cmd_prepare
    INSTALL_MODE=2 cmd_install
    cmd_boot_removable
    cmd_wait 90 || true
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
