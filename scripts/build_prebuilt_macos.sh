#!/usr/bin/env bash
# Build the aarch64-apple-darwin prebuilt of binutils-tricore natively on an
# Apple silicon Mac and stage it into third_party/binutils-tricore-prebuilt/.
# .github/workflows/prebuilts-macos.yml runs it on a hosted macos-14 runner
# (free for a public repository); a developer with a Mac can run it directly.
#
# Differences from the Linux build (scripts/build_prebuilts.sh):
#   - --without-zstd: a Mac has no libzstd unless Homebrew put it there, and
#     the gas objects must not depend on it.  zlib is part of macOS.
#   - MAKEINFO=true: macOS has no makeinfo, and the docs are not shipped.
#   - as.o is rebuilt with -Dmain=gas_main instead of `objcopy --redefine-sym`:
#     macOS has no objcopy, and Mach-O symbols carry a leading underscore.
#   - One build serves the nopic and the pic variant: code is position
#     independent by default on macOS.
#   - Only all-gas and all-opcodes are built (ld and the binutils tools are
#     not shipped), which also avoids the old bison of macOS.
#
# Usage:
#   scripts/build_prebuilt_macos.sh                   # fork at BINUTILS_COMMIT
#   NYX_BINUTILS_REF=<sha> scripts/build_prebuilt_macos.sh
#   NYX_WORK_DIR=/path scripts/build_prebuilt_macos.sh  # keep the build tree
#
# Side effect: overwrites third_party/binutils-tricore-prebuilt/aarch64-apple-darwin/.

set -euo pipefail
HERE="$(cd "$(dirname "$0")"/.. && pwd)"
PB="$HERE/third_party/binutils-tricore-prebuilt"
ARCH=aarch64-apple-darwin

if [[ "$(uname -s)-$(uname -m)" != Darwin-arm64 ]]; then
    echo "build_prebuilt_macos: run this on an Apple silicon Mac (got $(uname -s)-$(uname -m))" >&2
    exit 1
fi

FORK_URL="${NYX_BINUTILS_URL:-https://github.com/emproof-com/tricore-binutils-gdb.git}"
FORK_REF="${NYX_BINUTILS_REF:-$(cat "$PB/BINUTILS_COMMIT")}"

if [[ -n "${NYX_WORK_DIR:-}" ]]; then
    WORK="$NYX_WORK_DIR"
    mkdir -p "$WORK"
else
    WORK="$(mktemp -d -t nyx-prebuilt-macos)"
    trap 'rm -rf "$WORK"' EXIT
fi
SRC="$WORK/src"
BUILD="$WORK/build"
STAGE="$WORK/stage"

if [[ ! -d "$SRC/gas" ]]; then
    echo ">>> Fetching $FORK_URL @ $FORK_REF into $SRC"
    mkdir -p "$SRC"
    git -C "$SRC" init -q
    git -C "$SRC" fetch -q --depth=1 "$FORK_URL" "$FORK_REF"
    git -C "$SRC" checkout -q FETCH_HEAD
fi
COMMIT="$(git -C "$SRC" rev-parse HEAD)"
if [[ "$COMMIT" != "$(cat "$PB/BINUTILS_COMMIT")" ]]; then
    echo ">>> NOTE: building $COMMIT, BINUTILS_COMMIT records $(cat "$PB/BINUTILS_COMMIT")" >&2
fi

if [[ ! -f "$BUILD/gas/as.o" ]]; then
    echo ">>> Configuring binutils (target=tricore-elf, host=$ARCH) in $BUILD"
    mkdir -p "$BUILD"
    (cd "$BUILD" && "$SRC/configure" \
        --target=tricore-elf \
        --prefix="$BUILD/install" \
        --with-gnu-as --with-gnu-ld --with-system-zlib --without-zstd \
        --disable-gdb --disable-sim --disable-libdecnumber \
        --disable-readline --disable-werror --disable-nls \
        --disable-bootstrap --disable-multilib \
        --program-transform-name='s&^&tricore-elf-&' > "$BUILD/configure.log")
    echo ">>> Building gas + opcodes (a few minutes)"
    make -C "$BUILD" -j"$(sysctl -n hw.ncpu)" CFLAGS="-O2" CXXFLAGS="-O2" MAKEINFO=true \
        all-gas all-opcodes > "$BUILD/build.log" 2>&1 \
        || { tail -50 "$BUILD/build.log" >&2; exit 1; }
fi

# The generated bfd.h must match the committed host-independent headers:
# the glue and the C++ sources compile against headers-shared.tar.xz.
SHARED_CHECK="$WORK/shared-check"
rm -rf "$SHARED_CHECK"; mkdir -p "$SHARED_CHECK"
tar -C "$SHARED_CHECK" -xf "$PB/headers-shared.tar.xz"
if ! diff -q "$SHARED_CHECK/include/bfd.h" "$BUILD/bfd/bfd.h" >/dev/null; then
    echo "build_prebuilt_macos: the generated bfd/bfd.h differs from headers-shared.tar.xz:" >&2
    diff "$SHARED_CHECK/include/bfd.h" "$BUILD/bfd/bfd.h" >&2 || true
    exit 1
fi

echo ">>> Staging into $STAGE"
rm -rf "$STAGE"
LIB="$STAGE/lib"
mkdir -p "$LIB/gas/config" "$STAGE/headers/gas-internal-headers-build"
cp "$BUILD/opcodes/libopcodes.a"        "$LIB/"
cp "$BUILD/bfd/.libs/libbfd.a"          "$LIB/"
cp "$BUILD/libiberty/libiberty.a"       "$LIB/"
cp "$BUILD/libsframe/.libs/libsframe.a" "$LIB/"
for f in "$BUILD/gas"/*.o; do
    [[ "$(basename "$f")" == "as.o" ]] && continue
    cp "$f" "$LIB/gas/"
done
# as.o with main renamed, so that it does not clash with the host program.
rm -f "$BUILD/gas/as.o"
make -C "$BUILD/gas" as.o CFLAGS="-O2 -Dmain=gas_main" > /dev/null
cp "$BUILD/gas/as.o" "$LIB/gas/as_renamed.o"
if ! nm "$LIB/gas/as_renamed.o" | grep -q " T _gas_main$"; then
    echo "build_prebuilt_macos: as_renamed.o lacks _gas_main" >&2
    exit 1
fi
cp "$BUILD/gas/config/obj-elf.o"    "$LIB/gas/config/"
cp "$BUILD/gas/config/tc-tricore.o" "$LIB/gas/config/"
cp "$BUILD/gas/config/atof-ieee.o"  "$LIB/gas/config/" 2>/dev/null || true
for h in config.h targ-cpu.h targ-env.h obj-format.h; do
    cp "$BUILD/gas/$h" "$STAGE/headers/gas-internal-headers-build/"
done

echo ">>> Writing tarballs into $PB/$ARCH"
rm -rf "$PB/$ARCH"
mkdir -p "$PB/$ARCH/nopic" "$PB/$ARCH/pic"
# COPYFILE_DISABLE keeps the AppleDouble (._*) entries out of the archives.
export COPYFILE_DISABLE=1
tar -C "$STAGE/headers" -cJf "$PB/$ARCH/headers-arch.tar.xz" .
tar -C "$STAGE"         -cJf "$PB/$ARCH/nopic/lib.tar.xz" ./lib
cp "$PB/$ARCH/nopic/lib.tar.xz" "$PB/$ARCH/pic/lib.tar.xz"

echo ">>> Done ($COMMIT):"
find "$PB/$ARCH" -type f -name '*.tar.xz' -exec ls -l {} \;
