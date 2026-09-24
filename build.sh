#!/bin/sh
# Build ptrace_init: a small static i386 binary used as an alternative
# initramfs init that hijacks the RouterOS "mode" exec via ptrace.
#
# Usage: ./build.sh [output]          (default: ./ptrace_init)
#
# Also builds preload.so (the LD_PRELOAD probe, see preload.c) and embeds it
# into the init binary as a C array (preload_so.h).
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$ROOT/ptrace_init}

# Prefer musl (tiny static binary); fall back to a multilib host gcc.
find_cc() {
    if [ -x "$ROOT/.toolchain/i386-musl/bin/musl-gcc" ]; then
        echo "$ROOT/.toolchain/i386-musl/bin/musl-gcc"
    elif [ -x "$ROOT/../MikroTikPatch/.toolchain/i386-musl/bin/musl-gcc" ]; then
        echo "$ROOT/../MikroTikPatch/.toolchain/i386-musl/bin/musl-gcc"
    elif printf 'int main(void){return 0;}\n' | gcc -m32 -x c - -o /dev/null 2>/dev/null; then
        echo "gcc -m32"
    fi
}

CC=$(find_cc)
if [ -z "$CC" ]; then
    echo "ERROR: no i386 toolchain found." >&2
    echo "       Run MikroTikPatch/tools/musl_i386.sh, or install gcc-multilib." >&2
    exit 1
fi

# 1. the LD_PRELOAD probe (preload.c): ordinary C built without libc, so its
#    imports (open/write/readlink/getpid/snprintf/...) are resolved at load
#    time by the dynamic linker against the process's libc.
# shellcheck disable=SC2086
$CC -shared -fPIC -nostdlib -Os -fno-stack-protector \
    -o "$ROOT/preload.so" "$ROOT/preload.c"

# 2. embed preload.so into the init binary
python3 - "$ROOT/preload.so" "$ROOT/preload_so.h" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w') as f:
    f.write('/* generated from preload.so by build.sh - do not edit */\n')
    f.write('static const unsigned char preload_so[] = {\n')
    for i in range(0, len(data), 16):
        f.write('\t' + ','.join('0x%02x' % b for b in data[i:i + 16]) + ',\n')
    f.write('};\n')
    f.write('static const unsigned int preload_so_len = %d;\n' % len(data))
PY

# 3. ptrace_init itself
# shellcheck disable=SC2086
$CC -static -Os -Wall -o "$OUT" "$ROOT/ptrace_init.c"
echo "built: $OUT ($(wc -c < "$OUT") bytes), preload.so ($(wc -c < "$ROOT/preload.so") bytes)"
