#!/usr/bin/env python3
# mbr.py <image> <signature> - write an MBR with one EFI system partition
# (type 0xEF, starting at LBA 2048 = 1 MiB, aligned like the RouterOS ESP)
# and a fixed disk signature into <image>.
#
# Used by tools/bootkit_img.sh.  The fixed signature keeps the stick's
# partition identity stable across builds, which is what \BOOTKIT.CFG
# records (target=mbr:<sig>:<start>:<size>).
import os
import struct
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: %s <image> <signature>" % sys.argv[0])
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


main()
