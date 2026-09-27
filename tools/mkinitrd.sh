#!/bin/sh
# mkinitrd.sh <ptrace_init> <out.cpio> - pack one init binary as a cpio/newc
# initramfs.  The Makefile embeds the result in the matching EFI loader
# (bootloader/initrd_so*.h) with tools/embed.py.
#
# The cpio always contains the file as /ptrace_init, whatever <ptrace_init>
# is called on the host (the loader boots it with rdinit=/ptrace_init).
set -e
[ $# -eq 2 ] || { echo "usage: $0 <ptrace_init> <out.cpio>" >&2; exit 1; }
if ! command -v cpio >/dev/null 2>&1; then
    echo "ERROR: cpio not found (needed for the embedded initramfs)." >&2
    exit 1
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cp "$1" "$TMP/ptrace_init"
(cd "$TMP" && find ptrace_init | cpio -o -H newc --owner=0:0 2>/dev/null) > "$2"
echo "built: $2 ($(wc -c < "$2") bytes)"
