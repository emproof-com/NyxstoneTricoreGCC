#!/usr/bin/env bash
# Makes sure that the committed prebuilt of this host comes from the fork
# commit in third_party/binutils-tricore-prebuilt/BINUTILS_COMMIT.
#
# A byte comparison of the objects is not possible: the object code changes
# with the compiler version.  The script builds the pinned commit from source
# and compares what does not change with the compiler:
#
#   1. Headers: each committed header is byte-identical to the header of the
#      source build (config.h is excluded: it records the host features).
#   2. Objects: the committed tree and the source build have the same object
#      files and archives, and each one defines the same global symbols.
#   3. Behaviour: the test suite (it holds byte-exact reference corpora of
#      tricore-elf-as) passes against the source build.  `make test` on the
#      prebuilt is the usual CI job.
#
# Usage:
#   scripts/verify_prebuilt.sh             # Linux x86_64 / aarch64 host
#   KEEP_WORK=1 scripts/verify_prebuilt.sh # keep the work directory
#
# Exit code 0: no difference.  1: a difference (the report names each one).

set -euo pipefail
HERE="$(cd "$(dirname "$0")"/.. && pwd)"
PB="$HERE/third_party/binutils-tricore-prebuilt"

case "$(uname -s)-$(uname -m)" in
    Linux-x86_64)              ARCH=x86_64-linux-gnu  ;;
    Linux-aarch64|Linux-arm64) ARCH=aarch64-linux-gnu ;;
    *) echo "verify_prebuilt: no check for $(uname -s)-$(uname -m)" >&2; exit 2 ;;
esac

WORK="$(mktemp -d -t nyx-verify-XXXXXX)"
[[ "${KEEP_WORK:-0}" == 1 ]] || trap 'rm -rf "$WORK"' EXIT
BUILT="$WORK/built"
COMMITTED="$WORK/committed"
status=0
fail() { echo "DIFFERENCE: $*"; status=1; }

echo ">>> Source build of $(cat "$PB/BINUTILS_COMMIT")"
# No NYX_BINUTILS_REF: the script must build the pinned commit.
env -u NYX_BINUTILS_REF NYX_BINUTILS_PIC=0 \
    NYX_BINUTILS_SRC="$WORK/src" NYX_BINUTILS_BUILD="$WORK/build" \
    NYX_BINUTILS_OUT="$BUILT" "$HERE/scripts/fetch_binutils.sh" > "$WORK/build.log" 2>&1 \
    || { tail -40 "$WORK/build.log"; echo "verify_prebuilt: the source build failed" >&2; exit 1; }

mkdir -p "$COMMITTED"
tar -C "$COMMITTED" -xJf "$PB/headers-shared.tar.xz"
tar -C "$COMMITTED" -xJf "$PB/$ARCH/headers-arch.tar.xz"
tar -C "$COMMITTED" -xJf "$PB/$ARCH/nopic/lib.tar.xz"

echo ">>> 1. Headers"
headers=0
while IFS= read -r f; do
    rel="${f#"$COMMITTED"/}"
    [[ "$rel" == */config.h ]] && continue
    headers=$((headers + 1))
    if [[ ! -f "$BUILT/$rel" ]]; then
        fail "header $rel is not in the source build"
    elif ! cmp -s "$f" "$BUILT/$rel"; then
        fail "header $rel"
    fi
done < <(find "$COMMITTED" -type f \( -name '*.h' -o -name '*.def' -o -name '*.tab' \) | sort)
echo "    $headers headers compared"

echo ">>> 2. Objects"
globals() { nm -g --defined-only "$1" 2>/dev/null | awk 'NF >= 3 { print $3 }' | sort -u; }
if ! diff <(cd "$COMMITTED/lib" && find . -type f | sort) \
          <(cd "$BUILT/lib" && find . -type f | sort) > "$WORK/files.diff"; then
    cat "$WORK/files.diff"
    fail "the list of object files and archives"
fi
objects=0
while IFS= read -r f; do
    rel="${f#"$COMMITTED"/}"
    [[ -f "$BUILT/$rel" ]] || continue
    objects=$((objects + 1))
    if ! diff <(globals "$f") <(globals "$BUILT/$rel") > "$WORK/syms.diff"; then
        head -20 "$WORK/syms.diff"
        fail "global symbols of $rel"
    fi
done < <(find "$COMMITTED/lib" -type f | sort)
echo "    $objects objects and archives compared"

echo ">>> 3. Test suite against the source build"
BUILD_COPY="$WORK/checkout"
mkdir -p "$BUILD_COPY"
(cd "$HERE" && tar -cf - --exclude=./.git --exclude=./third_party/binutils-tricore \
    --exclude='./bindings/*/target' --exclude='*.o' --exclude='*.a' .) | tar -C "$BUILD_COPY" -xf -
if ! make -C "$BUILD_COPY" test NYX_BINUTILS="$BUILT" > "$WORK/test.log" 2>&1; then
    tail -40 "$WORK/test.log"
    fail "the test suite against the source build"
else
    grep -E '^Summary:|^OK:' "$WORK/test.log" | sed 's/^/    /'
fi

if [[ $status == 0 ]]; then
    echo "OK: the committed $ARCH prebuilt agrees with the source build of BINUTILS_COMMIT"
fi
exit $status
