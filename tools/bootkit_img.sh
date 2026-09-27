#!/bin/sh
# bootkit_img.sh <image> <label> <mbr-signature> <loader> <dest-path> - build
# an installer/removable USB stick image:
#
#   - USB_MB (default 32) MB, zero-filled
#   - MBR with one EFI system partition (type 0xEF, FAT) written by
#     tools/mbr.py, the partition starting at LBA 2048 (1 MiB)
#   - <loader> copied in as <dest-path> (\EFI\BOOT\BOOTX64.EFI for x86_64,
#     \EFI\BOOT\BOOTAA64.EFI for the arm64 CHR)
#
# Flash it (dd if=<image> of=/dev/sdX bs=4M conv=fsync); with no
# \BOOTKIT.CFG on the FAT partition the loader starts its install menu.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ $# -eq 5 ] || {
    echo "usage: $0 <image> <label> <mbr-signature> <loader> <dest-path>" >&2
    exit 1
}
for tool in mformat mmd mcopy; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: $tool not found (mtools are needed for $1)." >&2
        exit 1
    fi
done
command -v python3 >/dev/null 2>&1 || {
    echo "ERROR: python3 not found (needed for the MBR)." >&2
    exit 1
}
USB_MB=${USB_MB:-32}
dd if=/dev/zero of="$1" bs=1M count="$USB_MB" status=none
python3 "$ROOT/tools/mbr.py" "$1" "$3"
mformat -i "$1@@1048576" -v "$2" ::
mmd -i "$1@@1048576" ::/EFI ::/EFI/BOOT
mcopy -i "$1@@1048576" "$4" "::/$5"
echo "built: $1 ($(wc -c < "$1") bytes, flash to a USB stick)"
