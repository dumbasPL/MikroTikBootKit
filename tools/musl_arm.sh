#!/bin/sh
# Build a minimal ARM (armv7, EABI) musl toolchain for the bootkit.
#
# The arm64 CHR runs an AArch32 (armv7 soft-float) RouterOS userspace, so the
# initramfs init and the LD_PRELOAD probe for it are ARM32 binaries:
#
#   ptrace_init-arm   static, linked against this musl (hard-float is fine -
#                     it is our own PID 1 and runs no floating-point code)
#   preload.so        built separately with -mfloat-abi=soft -nostdlib, so it
#                     matches the soft-float RouterOS processes it is loaded
#                     into (see the Makefile)
#
# musl compiles its own libc, so the only requirements are a host cross
# compiler that targets arm-linux-gnueabihf (Debian/Ubuntu:
# gcc-arm-linux-gnueabihf) plus ar/ranlib.  The result is installed under
# .toolchain/arm-musl and picked up automatically by the Makefile.
#
# Usage: tools/musl_arm.sh [musl-version]      (default 1.2.6)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=${1:-1.2.6}
PREFIX="$ROOT/.toolchain/arm-musl"
CACHE="$ROOT/.toolchain/cache"
SRC="$ROOT/.toolchain/musl-arm-$VERSION"
CC=${ARM_CC:-arm-linux-gnueabihf-gcc}

command -v "$CC" >/dev/null 2>&1 || {
    echo "ERROR: $CC not found (needed for the arm32 ptrace_init/probe)" >&2
    echo "       install gcc-arm-linux-gnueabihf or set ARM_CC=..." >&2
    exit 1
}

mkdir -p "$CACHE"
TARBALL="$CACHE/musl-$VERSION.tar.gz"
if [ ! -f "$TARBALL" ]; then
    echo "==> downloading musl-$VERSION"
    # download to a unique name and rename into place, so concurrent builds
    # (e.g. make -j bootstrapping both toolchains) cannot corrupt the cache
    TMP="$TARBALL.$$"
    trap 'rm -f "$TMP"' EXIT
    if command -v wget >/dev/null 2>&1; then
        wget -nv -O "$TMP" "https://musl.libc.org/releases/musl-$VERSION.tar.gz"
    elif command -v curl >/dev/null 2>&1; then
        curl -fL -o "$TMP" "https://musl.libc.org/releases/musl-$VERSION.tar.gz"
    else
        echo "ERROR: neither wget nor curl available to download musl" >&2
        exit 1
    fi
    mv -f "$TMP" "$TARBALL"
    trap - EXIT
fi

rm -rf "$SRC" "$PREFIX"
mkdir -p "$SRC"
tar -xzf "$TARBALL" -C "$SRC" --strip-components=1

echo "==> building musl-$VERSION for armv7 (static)"
cd "$SRC"
CC="$CC" ./configure --target=arm-linux-musleabihf --prefix="$PREFIX" \
    --disable-shared >/dev/null
make -j"$(nproc)" CROSS_COMPILE= AR=ar RANLIB=ranlib >/dev/null
make install CROSS_COMPILE= AR=ar RANLIB=ranlib >/dev/null

# musl installs a wrapper naming the configure-time compiler; rewrite it so
# ARM_CC overrides keep working and the path is explicit.
cat > "$PREFIX/bin/musl-gcc" <<EOF
#!/bin/sh
exec $CC "\$@" -specs "$PREFIX/lib/musl-gcc.specs"
EOF
chmod +x "$PREFIX/bin/musl-gcc"

echo "==> installed: $PREFIX/bin/musl-gcc (via $CC)"
