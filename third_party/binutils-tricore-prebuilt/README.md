# binutils-tricore-prebuilt

Pre-built binutils-tricore objects + headers, committed to skip the
~75 s clone+configure+build cycle of [`scripts/fetch_binutils.sh`](../../scripts/fetch_binutils.sh).

Built from `emproof-com/tricore-binutils-gdb` at the commit recorded in
[`BINUTILS_COMMIT`](BINUTILS_COMMIT).

## Layout

```
binutils-tricore-prebuilt/
├── BINUTILS_COMMIT                 # the emproof-com fork commit these were built from
├── headers-shared.tar.xz           # ~830 KB, host-independent gas/bfd headers
└── {x86_64-linux-gnu,aarch64-linux-gnu}/
    ├── headers-arch.tar.xz         # ~3 KB, host-specific config.h/targ-cpu.h/...
    ├── nopic/lib.tar.xz            # ~600 KB, static .a + .o, no -fPIC
    └── pic/lib.tar.xz              # ~600 KB, same, built with -fPIC
```

**Total committed**: ~3.2 MB for all four variants × two architectures.

## What's inside `lib.tar.xz`

Each archive expands to a `lib/` tree with the artifacts NyxstoneTricoreGCC
links into `libnyxstone_tricore.{a,so}`:

```
lib/
├── libbfd.a            libopcodes.a        libiberty.a    libsframe.a
└── gas/
    ├── *.o             (37 gas core objects)
    ├── as_renamed.o    (as.o post-objcopy main→gas_main)
    └── config/
        ├── tc-tricore.o
        ├── obj-elf.o
        └── atof-ieee.o
```

## How the build picks a variant

[`scripts/extract_prebuilt.sh`](../../scripts/extract_prebuilt.sh) auto-selects
the right variant based on host arch + the `NYX_BINUTILS_PIC` env var:

| host arch | `NYX_BINUTILS_PIC=0` (default) | `NYX_BINUTILS_PIC=1` |
|---|---|---|
| `x86_64`  | `x86_64-linux-gnu/nopic/`  | `x86_64-linux-gnu/pic/`  |
| `aarch64` | `aarch64-linux-gnu/nopic/` | `aarch64-linux-gnu/pic/` |

Other architectures (MIPS, riscv64, ppc64le, …) fall back to
[`scripts/fetch_binutils.sh`](../../scripts/fetch_binutils.sh), clone + build
from source for the local host.

## Refreshing the cache

When the upstream pin in `BINUTILS_COMMIT` is bumped, rebuild all four
variants from scratch and re-commit:

```sh
scripts/build_prebuilts.sh        # rebuilds all 4 variants + restages tarballs
git add third_party/binutils-tricore-prebuilt/
git commit -m "Refresh binutils-tricore prebuilts (commit <new-sha>)"
```

CI should additionally run a verification job: rebuild from
`BINUTILS_COMMIT` and `diff` against the committed tarballs to catch
accidental drift.

## Why per-arch `headers-arch.tar.xz`?

binutils' `configure` writes a `gas/config.h` that records host-detected
features (e.g. `HAVE_ZSTD`).  The compiled `.o` files reference these
defines, so the consumer's `nyxstone_glue.c` must compile against the
*same* `config.h`.  Everything else (`bfd.h`, libopcodes headers,
TriCore-specific gas headers) is target-defined and identical across hosts,
so it lives in the single `headers-shared.tar.xz`.
