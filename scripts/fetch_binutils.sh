#!/usr/bin/env bash
# Clone + configure + build the emproof-com/tricore-binutils-gdb fork at a
# known commit, then stage the artifacts NyxstoneTricoreGCC needs into
# third_party/binutils-tricore/.
#
# Usage:
#   scripts/fetch_binutils.sh                    # clone fresh, build, stage
#   NYX_BINUTILS_SRC=/path/to/src scripts/fetch_binutils.sh
#                                                # use an existing source tree
#   NYX_BINUTILS_BUILD=/path/to/build scripts/fetch_binutils.sh
#                                                # use an existing build tree
#
# Side effect: writes ~25 MB into third_party/binutils-tricore/.

set -euo pipefail
HERE="$(cd "$(dirname "$0")"/.. && pwd)"
TP="$HERE/third_party/binutils-tricore"

FORK_URL="${NYX_BINUTILS_URL:-https://github.com/emproof-com/tricore-binutils-gdb.git}"
FORK_REF="${NYX_BINUTILS_REF:-master}"

# Set NYX_BINUTILS_PIC=1 to build binutils with -fPIC.  Required if you want
# to build NyxstoneTricoreGCC as a shared library (e.g. for Python ctypes).
NYX_BINUTILS_PIC="${NYX_BINUTILS_PIC:-0}"
if [[ "$NYX_BINUTILS_PIC" == "1" ]]; then
    EXTRA_CFLAGS="-fPIC"
else
    EXTRA_CFLAGS=""
fi

WORK="$(mktemp -d -t nyx-binutils-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

SRC="${NYX_BINUTILS_SRC:-$WORK/binutils-src}"
BUILD="${NYX_BINUTILS_BUILD:-$WORK/binutils-build}"
PREFIX="$WORK/binutils-install"

if [[ ! -d "$SRC" ]]; then
    echo ">>> Cloning $FORK_URL @ $FORK_REF into $SRC"
    git clone --depth=1 --branch "$FORK_REF" "$FORK_URL" "$SRC"
fi

if [[ ! -d "$BUILD/gas" ]]; then
    echo ">>> Configuring binutils (target=tricore-elf) in $BUILD"
    mkdir -p "$BUILD"
    (cd "$BUILD" && "$SRC/configure" \
        --target=tricore-elf \
        --prefix="$PREFIX" \
        --with-gnu-as --with-gnu-ld --with-system-zlib \
        --disable-gdb --disable-sim --disable-libdecnumber \
        --disable-readline --disable-werror --disable-nls \
        --disable-bootstrap --disable-multilib \
        --program-transform-name='s&^&tricore-elf-&')
    echo ">>> Building binutils (this takes a few minutes${EXTRA_CFLAGS:+, with $EXTRA_CFLAGS})"
    make -C "$BUILD" -j"$(nproc)" \
        ${EXTRA_CFLAGS:+CFLAGS="-O2 $EXTRA_CFLAGS"} \
        ${EXTRA_CFLAGS:+CXXFLAGS="-O2 $EXTRA_CFLAGS"} \
        all-binutils all-gas all-ld all-opcodes
fi

echo ">>> Staging artifacts into $TP"
mkdir -p "$TP/lib/gas/config" "$TP/include" "$TP/binutils-include" \
         "$TP/gas-internal-headers/config" \
         "$TP/gas-internal-headers-build" \
         "$TP/bfd-internal-headers" \
         "$TP/bfd-internal-headers-build"

# Static libraries.
cp "$BUILD/opcodes/libopcodes.a"           "$TP/lib/"
cp "$BUILD/bfd/.libs/libbfd.a"             "$TP/lib/"
cp "$BUILD/libiberty/libiberty.a"          "$TP/lib/"
cp "$BUILD/libsframe/.libs/libsframe.a"    "$TP/lib/"

# gas .o files we need (every .o except as.o; we also stage as_renamed.o).
for f in "$BUILD/gas"/*.o; do
    name="$(basename "$f")"
    if [[ "$name" == "as.o" ]]; then continue; fi
    cp "$f" "$TP/lib/gas/"
done
# Renamed as.o so its main() doesn't conflict with ours.
cp "$BUILD/gas/as.o" "$TP/lib/gas/as_renamed.o"
"${OBJCOPY:-objcopy}" --redefine-sym main=gas_main "$TP/lib/gas/as_renamed.o"
# Target-specific gas config objects.
cp "$BUILD/gas/config/obj-elf.o" "$TP/lib/gas/config/"
cp "$BUILD/gas/config/tc-tricore.o" "$TP/lib/gas/config/"
cp "$BUILD/gas/config/atof-ieee.o" "$TP/lib/gas/config/" 2>/dev/null || true

# Public binutils headers (consumed by nyxstone.cpp / nyxstone_c.cpp).
cp "$SRC/include/dis-asm.h"      "$TP/include/"
cp "$SRC/include/ansidecl.h"     "$TP/include/"
cp "$SRC/include/symcat.h"       "$TP/include/"
cp "$SRC/include/diagnostics.h"  "$TP/include/" 2>/dev/null || true
cp "$BUILD/bfd/bfd.h"            "$TP/include/"
cp -r "$SRC/include/opcode"      "$TP/include/"
cp -r "$SRC/include/elf"         "$TP/include/"

# gas + bfd internal headers (consumed by nyxstone_glue.c only).
cp "$SRC/gas"/*.h                "$TP/gas-internal-headers/"
cp "$SRC/gas/config"/*.h         "$TP/gas-internal-headers/config/"
cp "$BUILD/gas/config.h"         "$TP/gas-internal-headers-build/"
cp "$BUILD/gas/targ-cpu.h"       "$TP/gas-internal-headers-build/"
cp "$BUILD/gas/targ-env.h"       "$TP/gas-internal-headers-build/"
cp "$BUILD/gas/obj-format.h"     "$TP/gas-internal-headers-build/"
cp "$SRC/bfd"/*.h                "$TP/bfd-internal-headers/" 2>/dev/null || true

# binutils-src/include/, gas headers include libiberty.h, dwarf2.h, etc.
cp "$SRC/include"/*.h            "$TP/binutils-include/" 2>/dev/null || true
cp -r "$SRC/include/opcode"      "$TP/binutils-include/" 2>/dev/null || true
cp -r "$SRC/include/elf"         "$TP/binutils-include/" 2>/dev/null || true

# obj-elf.h includes "bfd/elf-bfd.h", expecting the source-tree layout.
# Vendor bfd/*.h under that subdirectory so the include resolves.
mkdir -p "$TP/bfd"
cp "$SRC/bfd"/*.h                "$TP/bfd/" 2>/dev/null || true

touch "$TP/.populated"

echo ">>> Done.  third_party/binutils-tricore/ is ready."
echo "    Run 'make' to build NyxstoneTricoreGCC."
