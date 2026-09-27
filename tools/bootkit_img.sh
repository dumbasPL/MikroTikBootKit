#!/bin/sh
# bootkit_img.sh <image> <label> <mbr-signature> <src> <dest> [<src> <dest>...]
# - build an installer/removable USB stick image:
#
#   - USB_MB (default 32) MB, zero-filled
#   - MBR with one EFI system partition (type 0xEF, FAT) written by
#     tools/mbr.py, the partition starting at LBA 2048 (1 MiB)
#   - every <src> copied in as <dest>, its directories created first:
#       bootkit.img        bootkit.efi       -> EFI/BOOT/BOOTX64.EFI
#       bootkit-arm64.img  bootkit-arm64.efi -> EFI/BOOT/BOOTAA64.EFI
#       bootkit-auto.img   both loaders plus bootloader/auto.cfg as
#                          BOOTKIT.CFG (\BOOTKIT.CFG = target=auto)
#
# Flash it (dd if=<image> of=/dev/sdX bs=4M conv=fsync); with no
# \BOOTKIT.CFG on the FAT partition the loader starts its install menu.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ $# -ge 5 ] || {
    echo "usage: $0 <image> <label> <mbr-signature> <src> <dest> [<src> <dest>...]" >&2
    exit 1
}
image=$1
label=$2
mbrsig=$3
shift 3
[ $(( $# % 2 )) -eq 0 ] || {
    echo "ERROR: the <src> <dest> arguments come in pairs" >&2
    exit 1
}
for tool in mformat mmd mcopy; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: $tool not found (mtools are needed for $image)." >&2
        exit 1
    fi
done
command -v python3 >/dev/null 2>&1 || {
    echo "ERROR: python3 not found (needed for the MBR)." >&2
    exit 1
}
USB_MB=${USB_MB:-32}
OFF=1048576				# partition 1 starts at LBA 2048 (1 MiB)
dd if=/dev/zero of="$image" bs=1M count="$USB_MB" status=none
python3 "$ROOT/tools/mbr.py" "$image" "$mbrsig"
mformat -i "$image@@$OFF" -v "$label" :: </dev/null

# create the directories of a destination path (::/EFI/BOOT for a loader).
# dirs holds what has been created, so mmd runs once per directory: called
# again for an existing one, mtools asks how to resolve the name clash and -
# with a terminal on stdin, as in a make run - waits for input forever
dirs=
mkpath() {
    path=${1%/*}
    [ "$path" = "$1" ] && return 0
    dir=
    while :; do
        part=${path%%/*}
        dir="$dir/$part"
        case " $dirs " in
        *" $dir "*) ;;
        *)  mmd -i "$image@@$OFF" "::$dir" </dev/null
            dirs="$dirs $dir" ;;
        esac
        [ "$part" = "$path" ] && break
        path=${path#*/}
    done
}

while [ $# -ge 2 ]; do
    src=$1
    dest=${2#/}
    shift 2
    mkpath "$dest"
    mcopy -i "$image@@$OFF" "$src" "::/$dest" </dev/null
    echo "  ::/$dest <- $src"
done
echo "built: $image ($(wc -c < "$image") bytes, flash to a USB stick)"
