#!/bin/sh
# Build ptrace_init: a small static i386 binary used as an alternative
# initramfs init that hijacks the RouterOS "mode" exec via ptrace.
#
# Usage: ./build.sh [output]          (default: ./ptrace_init)
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

# shellcheck disable=SC2086
$CC -static -Os -Wall -o "$OUT" "$ROOT/ptrace_init.c"
echo "built: $OUT ($(wc -c < "$OUT") bytes)"
