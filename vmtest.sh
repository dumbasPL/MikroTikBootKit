#!/usr/bin/env bash
# vmtest.sh - prepare, boot and interact with a RouterOS x86 image copy under
# QEMU/KVM, with the ptrace_init bootkit installed via its own EFI loader
# (bootkit.efi, initramfs embedded).  The loader is installed as
# \EFI\BOOT\BOOTKIT.EFI and is reached through a Boot#### entry; the stock
# kernel stays at \EFI\BOOT\BOOTX64.EFI so RouterOS updates can overwrite it.
#
# The boot entry is created by the loader's own installer: the test boots a
# small "installer stick" (a FAT image with bootkit.efi as \EFI\BOOT\BOOTX64.EFI)
# together with the target image, answers the installer's selection prompt over
# the serial console, and then boots the target normally.
#
# The source image is never modified: everything happens on $WORK/test.img.
#
# Usage:
#   ./vmtest.sh prepare [IMG]   build the bootkit, make the test image and the
#                               installer stick, start from a fresh varstore
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
#   WORK=/tmp/opencode/bkvm     scratch directory (image copy, logs, pidfile)
#   IMG=<path>                  source image (default: x86-7.24.4-clean.img)
#   MODE=chr|x86|keep           override the MBR mode flag (default: keep)
#   INSTALL_MODE=1|2            installer mode for "install": 1 direct
#                               (default), 2 removable
#   MEM=1024  SMP=2             QEMU memory / cpus
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-/tmp/opencode/bkvm}
IMG_SRC=${IMG_SRC:-$ROOT/x86-7.24.4-clean.img}
IMG=$WORK/test.img
STICK=$WORK/stick.img
ESP_OFF=1048576                      # partition 1 starts at LBA 2048
MODE=${MODE:-keep}
MEM=${MEM:-1024}
SMP=${SMP:-2}

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

cmd_prepare() {
    [ -f "$IMG_SRC" ] || die "source image not found: $IMG_SRC"
    log "== building bootkit"
    (cd "$ROOT" && ./build.sh)

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

    log "== building the installer stick ($STICK)"
    export MTOOLS_SKIP_CHECK=1
    rm -f "$STICK"
    dd if=/dev/zero of="$STICK" bs=1M count=2 status=none
    mformat -i "$STICK" -v BKINSTALL ::
    mmd -i "$STICK" ::/EFI ::/EFI/BOOT
    mcopy -i "$STICK" "$ROOT/bootkit.efi" ::/EFI/BOOT/BOOTX64.EFI

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
    *)       sed -n '2,44p' "$0"; exit 1 ;;
esac
