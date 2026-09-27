#!/usr/bin/env python3
# embed.py <input> <output> <symbol> - write <input> as a C header holding
#   static const unsigned char <symbol>[] = {...};
#   static const unsigned int <symbol>_len = <size>;
#
# The Makefile uses it to embed bootkit/preload.so in ptrace_init and the
# initramfs cpio in the EFI loaders (bootloader/initrd_so*.h).
import sys


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: %s <input> <output> <symbol>" % sys.argv[0])
    data = open(sys.argv[1], 'rb').read()
    sym = sys.argv[3]
    with open(sys.argv[2], 'w') as f:
        f.write('/* generated from %s by tools/embed.py - do not edit */\n'
                % sys.argv[1])
        f.write('static const unsigned char %s[] = {\n' % sym)
        for i in range(0, len(data), 16):
            f.write('\t' + ','.join('0x%02x' % b for b in data[i:i + 16]) +
                    ',\n')
        f.write('};\n')
        f.write('static const unsigned int %s_len = %d;\n' % (sym, len(data)))


main()
