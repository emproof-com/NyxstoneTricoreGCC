#!/usr/bin/env bash
# Rebuild all 4 binutils-tricore-prebuilt variants
# ({x86_64, aarch64} × {nopic, pic}) from scratch and stage them into
# third_party/binutils-tricore-prebuilt/.
#
# Use this when bumping BINUTILS_COMMIT, never edit the committed
# tarballs by hand.
#
# Requirements:
#   - native compiler for x86_64 (gcc / g++)
#   - aarch64 cross-compiler (apt: gcc-aarch64-linux-gnu g++-aarch64-linux-gnu)
#   - aarch64 cross-objcopy (comes with the cross-binutils package)
#
# Usage:
#   scripts/build_prebuilts.sh                       # use default fork URL
#   NYX_BINUTILS_URL=... scripts/build_prebuilts.sh  # custom URL
#   NYX_BINUTILS_REF=<sha> scripts/build_prebuilts.sh
#                                                    # pin to a specific commit
#
# Side effect: overwrites every tarball in
# third_party/binutils-tricore-prebuilt/ and rewrites BINUTILS_COMMIT.

set -euo pipefail
HERE="$(cd "$(dirname "$0")"/.. && pwd)"
PB="$HERE/third_party/binutils-tricore-prebuilt"

FORK_URL="${NYX_BINUTILS_URL:-https://github.com/emproof-com/tricore-binutils-gdb.git}"
FORK_REF="${NYX_BINUTILS_REF:-master}"

# Sanity-check cross-toolchain before doing anything expensive.
if ! command -v aarch64-linux-gnu-gcc >/dev/null; then
    echo "ERROR: aarch64-linux-gnu-gcc not found.  Install with:" >&2
    echo "  sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu" >&2
    exit 1
fi

WORK="$(mktemp -d -t nyx-prebuilts-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

echo ">>> Cloning $FORK_URL @ $FORK_REF"
git clone --depth=1 --branch "$FORK_REF" "$FORK_URL" "$WORK/src"
COMMIT=$(cd "$WORK/src" && git rev-parse HEAD)
echo ">>> Pinned to $COMMIT"

build_variant() {
    local arch="$1" pic="$2"
    local bdir="$WORK/build-${arch}-${pic}"
    local host_args=""
    local cflags="-O2" cxxflags="-O2"
    [[ "$arch" == "aarch64" ]] && host_args="--host=aarch64-linux-gnu"
    [[ "$pic" == "pic"       ]] && { cflags="-O2 -fPIC"; cxxflags="-O2 -fPIC"; }
    mkdir -p "$bdir"
    (cd "$bdir" && "$WORK/src/configure" \
        --target=tricore-elf $host_args \
        --prefix="$bdir/install" \
        --with-gnu-as --with-gnu-ld --with-system-zlib \
        --disable-gdb --disable-sim --disable-libdecnumber \
        --disable-readline --disable-werror --disable-nls \
        --disable-bootstrap --disable-multilib \
        --program-transform-name='s&^&tricore-elf-&' >/dev/null)
    make -C "$bdir" -j4 CFLAGS="$cflags" CXXFLAGS="$cxxflags" \
        all-binutils all-gas all-ld all-opcodes > "$bdir/build.log" 2>&1
    echo "  built: $arch $pic"
}

echo ">>> Building all 4 variants in parallel..."
build_variant x86_64  nopic &
build_variant x86_64  pic   &
build_variant aarch64 nopic &
build_variant aarch64 pic   &
wait
echo ">>> All variants built."

stage_lib() {
    local bdir="$1" out="$2" objcopy="$3"
    mkdir -p "$out/lib/gas/config"
    cp "$bdir/opcodes/libopcodes.a"            "$out/lib/"
    cp "$bdir/bfd/.libs/libbfd.a"              "$out/lib/"
    cp "$bdir/libiberty/libiberty.a"           "$out/lib/"
    cp "$bdir/libsframe/.libs/libsframe.a"     "$out/lib/"
    for f in "$bdir/gas"/*.o; do
        local name; name="$(basename "$f")"
        [[ "$name" == "as.o" ]] && continue
        cp "$f" "$out/lib/gas/"
    done
    cp "$bdir/gas/as.o" "$out/lib/gas/as_renamed.o"
    "$objcopy" --redefine-sym main=gas_main "$out/lib/gas/as_renamed.o"
    cp "$bdir/gas/config/obj-elf.o"    "$out/lib/gas/config/"
    cp "$bdir/gas/config/tc-tricore.o" "$out/lib/gas/config/"
    cp "$bdir/gas/config/atof-ieee.o"  "$out/lib/gas/config/" 2>/dev/null || true
}

stage_lib "$WORK/build-x86_64-nopic"  "$WORK/stage/x86_64-linux-gnu/nopic"  objcopy
stage_lib "$WORK/build-x86_64-pic"    "$WORK/stage/x86_64-linux-gnu/pic"    objcopy
stage_lib "$WORK/build-aarch64-nopic" "$WORK/stage/aarch64-linux-gnu/nopic" aarch64-linux-gnu-objcopy
stage_lib "$WORK/build-aarch64-pic"   "$WORK/stage/aarch64-linux-gnu/pic"   aarch64-linux-gnu-objcopy

# Shared headers (host-independent).
SHARED="$WORK/stage/headers-shared"
mkdir -p "$SHARED"/{include,binutils-include,gas-internal-headers/config,bfd-internal-headers,bfd}
SRC="$WORK/src"; B="$WORK/build-x86_64-nopic"
cp "$SRC/include/dis-asm.h"     "$SHARED/include/"
cp "$SRC/include/ansidecl.h"    "$SHARED/include/"
cp "$SRC/include/symcat.h"      "$SHARED/include/"
cp "$SRC/include/diagnostics.h" "$SHARED/include/" 2>/dev/null || true
cp "$B/bfd/bfd.h"               "$SHARED/include/"
cp -r "$SRC/include/opcode"     "$SHARED/include/"
cp -r "$SRC/include/elf"        "$SHARED/include/"
cp "$SRC/gas"/*.h               "$SHARED/gas-internal-headers/"
cp "$SRC/gas/config"/*.h        "$SHARED/gas-internal-headers/config/"
cp "$SRC/bfd"/*.h               "$SHARED/bfd-internal-headers/" 2>/dev/null || true
cp "$SRC/include"/*.h           "$SHARED/binutils-include/"     2>/dev/null || true
cp -r "$SRC/include/opcode"     "$SHARED/binutils-include/"     2>/dev/null || true
cp -r "$SRC/include/elf"        "$SHARED/binutils-include/"     2>/dev/null || true
cp "$SRC/bfd"/*.h               "$SHARED/bfd/"                  2>/dev/null || true

# Per-arch generated headers.
for ARCH in x86_64 aarch64; do
    OUT="$WORK/stage/${ARCH}-linux-gnu/headers"
    mkdir -p "$OUT/gas-internal-headers-build"
    for h in config.h targ-cpu.h targ-env.h obj-format.h; do
        cp "$WORK/build-${ARCH}-nopic/gas/$h" "$OUT/gas-internal-headers-build/"
    done
done

echo ">>> Writing tarballs into $PB"
rm -rf "$PB"
mkdir -p "$PB"/{x86_64-linux-gnu/nopic,x86_64-linux-gnu/pic,aarch64-linux-gnu/nopic,aarch64-linux-gnu/pic}
echo "$COMMIT" > "$PB/BINUTILS_COMMIT"
tar -C "$WORK/stage/headers-shared" -cJf "$PB/headers-shared.tar.xz" .
for ARCH in x86_64-linux-gnu aarch64-linux-gnu; do
    tar -C "$WORK/stage/$ARCH/headers" -cJf "$PB/$ARCH/headers-arch.tar.xz" .
    tar -C "$WORK/stage/$ARCH/nopic"   -cJf "$PB/$ARCH/nopic/lib.tar.xz"    ./lib
    tar -C "$WORK/stage/$ARCH/pic"     -cJf "$PB/$ARCH/pic/lib.tar.xz"      ./lib
done

echo ">>> Done.  Committed sizes:"
find "$PB" -type f \( -name '*.tar.xz' -o -name 'BINUTILS_COMMIT' \) \
    -printf '  %P  %s bytes\n' | sort
