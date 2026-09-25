#!/usr/bin/env bash
# vmtest.sh - prepare, boot and interact with a RouterOS x86 image copy under
# QEMU/KVM, with the ptrace_init bootkit installed in an external initrd.
#
# The source image is never modified: everything happens on $WORK/test.img.
#
# Usage:
#   ./vmtest.sh prepare [IMG]   build the bootkit, make the test image and put
#                               the EFI shell + KERNEL.EFI + initrd.cpio +
#                               startup.nsh on its ESP
#   ./vmtest.sh boot            start QEMU in the background
#   ./vmtest.sh wait [SECS]     wait for the login prompt in the serial log
#   ./vmtest.sh check           show the bootkit result lines
#   ./vmtest.sh login           attach to the serial console (Ctrl-] to quit)
#   ./vmtest.sh cmd "<command>" log in as admin/admin and run one command
#   ./vmtest.sh stop            stop QEMU
#   ./vmtest.sh test            prepare + boot + wait + check
#
# Environment:
#   WORK=/tmp/opencode/bkvm     scratch directory (image copy, logs, pidfile)
#   IMG=<path>                  source image (default: x86-7.24.4-clean.img)
#   MODE=chr|x86|keep           override the MBR mode flag (default: keep)
#   MEM=1024  SMP=2             QEMU memory / cpus
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-/tmp/opencode/bkvm}
IMG_SRC=${IMG_SRC:-$ROOT/x86-7.24.4-clean.img}
IMG=$WORK/test.img
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

    log "== building initrd.cpio"
    rm -f "$WORK/initrd.cpio"
    (cd "$ROOT" && find ptrace_init -maxdepth 0 | cpio -o -H newc --owner=0:0 2>/dev/null) > "$WORK/initrd.cpio"

    log "== installing the boot files on the ESP"
    export MTOOLS_SKIP_CHECK=1
    local esp="$IMG@@$ESP_OFF"
    mcopy -o -i "$esp" ::/EFI/BOOT/BOOTX64.EFI "$WORK/kernel.efi"
    mcopy -o -i "$esp" /usr/share/edk2-shell/x64/Shell.efi ::/EFI/BOOT/BOOTX64.EFI
    mcopy -o -i "$esp" "$WORK/kernel.efi" ::/EFI/BOOT/KERNEL.EFI
    mcopy -o -i "$esp" "$WORK/initrd.cpio" ::/initrd.cpio
    printf 'fs0:\\EFI\\BOOT\\kernel.efi console=ttyS0,115200n8 initrd=\\initrd.cpio rdinit=/ptrace_init\n' > "$WORK/startup.nsh"
    mcopy -o -i "$esp" "$WORK/startup.nsh" ::/startup.nsh
    mdir -i "$esp" ::/ ::/EFI/BOOT | sed -n '1,3p;$p' >/dev/null
    log "== ready: $IMG"
}

cmd_boot() {
    [ -f "$IMG" ] || die "no test image, run: $0 prepare"
    stop_qemu
    cp -f /usr/share/edk2/x64/OVMF_VARS.4m.fd "$WORK/vars.fd"
    : > "$SERIAL_LOG"
    rm -f "$SERIAL_SOCK"
    # shellcheck disable=SC2046
    $(qemu_cmd) -daemonize -pidfile "$PIDFILE"
    log "== qemu started (pid $(cat "$PIDFILE"))"
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
    cmd_boot
    cmd_wait 90 || true
    cmd_check
}

case "${1:-}" in
    prepare) shift; cmd_prepare "$@" ;;
    boot)    cmd_boot ;;
    wait)    shift; cmd_wait "$@" ;;
    check)   cmd_check ;;
    login)   cmd_login ;;
    cmd)     shift; cmd_cmd "$@" ;;
    stop)    cmd_stop ;;
    test)    cmd_test ;;
    *)       sed -n '2,30p' "$0"; exit 1 ;;
esac
