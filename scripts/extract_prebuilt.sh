#!/usr/bin/env bash
# Extract the committed binutils-tricore-prebuilt tarballs into
# third_party/binutils-tricore/ so `make` can link against them.
#
# Picks the host-arch + PIC/non-PIC variant automatically.
#
# Usage:
#   scripts/extract_prebuilt.sh               # default: host arch, non-PIC
#   NYX_BINUTILS_PIC=1 scripts/extract_prebuilt.sh
#                                             # use PIC variant
#
# Returns 0 if extraction succeeded, 2 if no prebuilt matches the host arch
# (Makefile uses this to fall back to fetch_binutils.sh).

set -euo pipefail
HERE="$(cd "$(dirname "$0")"/.. && pwd)"
PB="$HERE/third_party/binutils-tricore-prebuilt"
TP="$HERE/third_party/binutils-tricore"
PIC="${NYX_BINUTILS_PIC:-0}"

case "$(uname -m)" in
    x86_64)  ARCH=x86_64-linux-gnu  ;;
    aarch64|arm64) ARCH=aarch64-linux-gnu ;;
    *)
        echo "extract_prebuilt: no prebuilt for $(uname -m), must build from source." >&2
        exit 2
        ;;
esac

if [[ "$PIC" == "1" ]]; then VARIANT=pic; else VARIANT=nopic; fi
TAG="$ARCH/$VARIANT"

LIB_TAR="$PB/$ARCH/$VARIANT/lib.tar.xz"
HDR_ARCH_TAR="$PB/$ARCH/headers-arch.tar.xz"
HDR_SHARED_TAR="$PB/headers-shared.tar.xz"

for f in "$LIB_TAR" "$HDR_ARCH_TAR" "$HDR_SHARED_TAR"; do
    if [[ ! -f "$f" ]]; then
        echo "extract_prebuilt: missing $f" >&2
        exit 2
    fi
done

# Idempotent: if .populated already records this exact variant, skip.
if [[ -f "$TP/.populated" ]] && grep -q " $TAG " "$TP/.populated"; then
    echo ">>> binutils-tricore already populated for $TAG (skipping)"
    exit 0
fi

echo ">>> Extracting binutils-tricore prebuilts ($ARCH, $VARIANT) into $TP"
rm -rf "$TP"
mkdir -p "$TP"
tar -C "$TP" -xf "$HDR_SHARED_TAR"
tar -C "$TP" -xf "$HDR_ARCH_TAR"
tar -C "$TP" -xf "$LIB_TAR"

# Marker so the Makefile knows third_party/binutils-tricore/ is ready.
echo "$(cat "$PB/BINUTILS_COMMIT")  $ARCH/$VARIANT  $(date -u +%FT%TZ)" \
    > "$TP/.populated"

echo ">>> Done.  third_party/binutils-tricore/ is ready."
