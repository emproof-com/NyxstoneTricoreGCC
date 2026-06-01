# NyxstoneTricoreGCC

[![ci](https://github.com/emproof-com/NyxstoneTricoreGCC/actions/workflows/ci.yml/badge.svg)](https://github.com/emproof-com/NyxstoneTricoreGCC/actions/workflows/ci.yml)
[![crates.io: nyxstone-tricore-gcc](https://img.shields.io/crates/v/nyxstone-tricore-gcc.svg?label=nyxstone-tricore-gcc%20(GPL))](https://crates.io/crates/nyxstone-tricore-gcc)
[![crates.io: nyxstone-tricore-gcc-ipc](https://img.shields.io/crates/v/nyxstone-tricore-gcc-ipc.svg?label=nyxstone-tricore-gcc-ipc%20(MIT))](https://crates.io/crates/nyxstone-tricore-gcc-ipc)
[![PyPI](https://img.shields.io/pypi/v/nyxstone-tricore-gcc.svg)](https://pypi.org/project/nyxstone-tricore-gcc/)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg)](LICENSE)

In-process TriCore assembler/disassembler.  C++17 library + Rust and Python
bindings.  Built on the GNU assembler's TriCore encoder (`md_assemble`) and
libopcodes' `print_insn_tricore` decoder from a pinned
[`emproof-com/tricore-binutils-gdb`](https://github.com/emproof-com/tricore-binutils-gdb)
fork, fully compatible with upstream [EEESlab/tricore-binutils-gdb](https://github.com/EEESlab/tricore-binutils-gdb).

```sh
# Rust, pick your licensing mode (see "License modes" below):
cargo add nyxstone-tricore-gcc        # GPL-3.0-or-later, in-process, ~2.8 M ops/s
cargo add nyxstone-tricore-gcc-ipc    # MIT, IPC daemon, ~150 k ops/s

# Python
pip install nyxstone-tricore-gcc
```

Supported prebuilts: `x86_64-linux-gnu`, `aarch64-linux-gnu`.  Other hosts
build binutils-tricore from source automatically (see *Binutils provisioning*
below).

The public API mirrors that of the sibling project
[Nyxstone](https://github.com/emproof-com/nyxstone), a different
implementation built on LLVM-MC that covers the architectures LLVM supports.
NyxstoneTricoreGCC is **not** a fork or downstream of Nyxstone; it's an
independent codebase using GNU binutils to cover TriCore (which LLVM-MC
doesn't).  Both projects expose six assemble/disassemble methods with the
same argument order (`source` / `bytes`, `address`, then either `labels` or
`count`):

| method | purpose |
|---|---|
| `assemble` | bytes only, labels resolved inline |
| `assemble_to_instructions` | per-insn records, labels resolved inline |
| `assemble_with_relocs` | bytes + relocations (gcc/gas `-r` equivalent) |
| `assemble_to_instructions_with_relocs` | per-insn records + relocations |
| `disassemble` | bytes → text |
| `disassemble_to_instructions` | bytes → per-insn records |

- **No fork/exec in the hot path.** All work happens in the calling process
  (or in a single long-lived daemon for the MIT-licensed Rust binding).
- **`.text`-only.** Any directive that would switch the active section to
  anything other than `.text` makes `assemble()` fail.
- **Stock binutils.** Uses an unmodified `emproof-com/tricore-binutils-gdb`
  build, no patches to gas, libbfd, or libopcodes.
- **Byte-equivalent to `tricore-elf-as`.** PC-relative branches with forward
  references go through a mini relax + `md_apply_fix` pass.

## License modes

The project ships **two Rust crates with byte-identical public APIs** so you
can pick the licensing mode that fits your situation by changing one line in
`Cargo.toml`:

| crate | license | mode | per-op | typical use |
|---|---|---|---|---|
| **`nyxstone-tricore-gcc`**     | GPL-3.0-or-later | in-process (links gas) | ~360 ns | open-source / GPL-compatible projects |
| **`nyxstone-tricore-gcc-ipc`** | MIT | IPC to GPL daemon       | ~6 µs   | closed-source / commercially permissive |

The MIT crate spawns a separate daemon binary (`nyxstone-tcd`, GPL-3.0+) and
talks to it over a `socketpair(2)` UNIX socket, one daemon per parent
process, lazy-spawn, killed when the parent exits via
`PR_SET_PDEATHSIG(SIGTERM)`.  Strict FIFO.  See
[`bindings/rust-ipc/README.md`](bindings/rust-ipc/README.md) for the binary
dependency and three daemon-install methods (`cargo install`, `cargo
binstall`, programmatic bootstrap).

The C++ and Python bindings only ship in GPL form today, they statically
link gas in-process for full speed.  If you need permissive licensing for
C++ or Python too, use the Rust MIT crate via a thin FFI or open an issue.

## Quick start

`make` Just Works™, it auto-extracts the committed binutils prebuilts
(`third_party/binutils-tricore-prebuilt/`, ~3 MB across x86_64+aarch64,
nopic+pic) on first run, no network access needed:

```sh
# 1. Build the C++ library + tests + examples (~5 s on first run).
make                    # or:  cmake -S . -B build && cmake --build build -j

# 2. Run the test suite (118 + extra API tests).
make test               # or:  ctest --test-dir build --output-on-failure

# 3. Try an example.
./smoke                 # round-trip a handful of TriCore insns
./bench 2.0             # throughput benchmark, 2-second window
```

For the Rust binding (GPL in-process mode):

```sh
cd bindings/rust
NYX_LIB_DIR=.. cargo build --release
NYX_LIB_DIR=.. cargo test --release
```

For the Rust binding (MIT IPC mode):

```sh
cd bindings/rust && cargo build --release --bin nyxstone-tcd   # build the daemon
cd ../rust-ipc
NYXSTONE_TCD_PATH=../rust/target/release/nyxstone-tcd \
    cargo test --release
```

For the Python binding (requires `pip install cffi setuptools`):

```sh
# Python's CFFI extension needs -fPIC binutils objects; use the PIC variant.
NYX_BINUTILS_PIC=1 make
(cd bindings/python && python3 setup.py build_ext --inplace)
(cd bindings/python && PYTHONPATH=. python3 examples/smoke.py)
```

### Binutils provisioning details

| source | trigger | time |
|---|---|---|
| **prebuilt** (default, x86_64 / aarch64 Linux) | `make` extracts `third_party/binutils-tricore-prebuilt/$(uname -m)-linux-gnu/{nopic,pic}/lib.tar.xz` automatically | **~1 s** |
| **from source** (other arches, or `make fetch_binutils`) | `scripts/fetch_binutils.sh`, clone emproof-com fork, configure, build, stage | **~75 s** |

Override the binutils tree wholesale with `make NYX_BINUTILS=/path/to/tree`.

## API

### C++ (`#include <nyxstone/nyxstone.h>`)

```cpp
namespace nyxstone {

using Address = uint64_t;

struct RelocationSymbol { std::string name; Address address; };
struct RelocationInfo {
    Address offset;
    std::optional<int64_t> addend;
    RelocationSymbol symbol;
    uint32_t relocation_type;     // ELF R_TRICORE_*
};

class NyxstoneTricoreGCC {
public:
    struct LabelDefinition { std::string name; Address address; };
    struct Instruction     { Address address; std::string assembly; std::vector<uint8_t> bytes; };

    struct AssembleWithRelocsResult {
        std::vector<uint8_t> bytes;
        std::vector<RelocationInfo> relocations;
    };
    struct AssembleInstructionsWithRelocsResult {
        std::vector<Instruction> instructions;
        std::vector<RelocationInfo> relocations;
    };

    static tl::expected<std::unique_ptr<NyxstoneTricoreGCC>, std::string> create();

    tl::expected<std::vector<uint8_t>, std::string> assemble(
        const std::string& assembly, Address address,
        const std::vector<LabelDefinition>& labels) const;

    tl::expected<std::vector<Instruction>, std::string> assemble_to_instructions(
        const std::string& assembly, Address address,
        const std::vector<LabelDefinition>& labels) const;

    tl::expected<AssembleWithRelocsResult, std::string> assemble_with_relocs(
        const std::string& assembly, Address address,
        const std::vector<LabelDefinition>& labels) const;

    tl::expected<AssembleInstructionsWithRelocsResult, std::string>
    assemble_to_instructions_with_relocs(
        const std::string& assembly, Address address,
        const std::vector<LabelDefinition>& labels) const;

    tl::expected<std::string, std::string> disassemble(
        const std::vector<uint8_t>& bytes, Address address, size_t count) const;

    tl::expected<std::vector<Instruction>, std::string> disassemble_to_instructions(
        const std::vector<uint8_t>& bytes, Address address, size_t count) const;
};

}
```

`tl::expected` is the Sy Brand single-header
([`include/nyxstone/expected.hpp`](include/nyxstone/expected.hpp)), no
external dependency.

### C ABI (`#include <nyxstone_c.h>`)

```c
typedef struct nyxstone_handle nyxstone_handle_t;
typedef struct { const char* name;  uint64_t address; } nyxstone_label_def_t;
typedef struct { uint64_t address;  char* assembly;
                 uint8_t* bytes;    size_t bytes_len; } nyxstone_instruction_t;

nyxstone_handle_t* nyxstone_create(char** out_err);
void                        nyxstone_destroy(nyxstone_handle_t*);

int nyxstone_assemble(nyxstone_handle_t*, const char* src, size_t src_len,
                 uint64_t address,
                 const nyxstone_label_def_t* labels, size_t labels_len,
                 uint8_t** out_bytes, size_t* out_len,
                 char** out_err);
int nyxstone_assemble_to_instructions(/* same args, instr array out */);
int nyxstone_disassemble(nyxstone_handle_t*, const uint8_t* bytes, size_t bytes_len,
                    uint64_t address, size_t count,
                    char** out_text, char** out_err);
int nyxstone_disassemble_to_instructions(/* same args, instr array out */);

void nyxstone_free_bytes(uint8_t*);
void nyxstone_free_string(char*);
void nyxstone_free_instructions(nyxstone_instruction_t*, size_t);
```

### Rust, same code, two crates

Both crates expose `NyxstoneTricoreGCC` with identical method signatures.
Switching is a one-line `Cargo.toml` change:

```rust
// Same user code for both crates, only the use-path differs.
use nyxstone_tricore_gcc::{LabelDefinition, NyxstoneTricoreGCC};         // GPL crate
// or:
use nyxstone_tricore_gcc_ipc::{LabelDefinition, NyxstoneTricoreGCC};     // MIT crate

let nx = NyxstoneTricoreGCC::new()?;
let bytes  = nx.assemble("nop; ret", 0, &[])?;
let insns  = nx.disassemble_to_instructions(&bytes, 0x1000, 0)?;
let bytes2 = nx.assemble("nop; nop; j ext", 0x1000,
                         &[LabelDefinition::new("ext", 0x2000)])?;

// gcc/gas -r equivalent, external labels stay as relocs in the byte stream.
let (rel_bytes, relocs) = nx.assemble_with_relocs(
    "nop\n j ext\n", 0x1000,
    &[LabelDefinition::new("ext", 0x2000)])?;
// relocs[0] = { offset: 2, symbol: { name: "ext", address: 0x2000 },
//               relocation_type: 3 /* R_TRICORE_24REL */, addend: Some(0) }
```

The MIT crate additionally exposes three bootstrap helpers for daemon
installation (`install_daemon_if_missing`, `install_daemon`, `daemon_path`)
so you can avoid documenting a separate `cargo install` step in your own
README:

```rust
fn main() -> Result<(), Box<dyn std::error::Error>> {
    // Optional zero-config bootstrap (prefers `cargo binstall` if available).
    nyxstone_tricore_gcc_ipc::install_daemon_if_missing()?;
    let nx = nyxstone_tricore_gcc_ipc::NyxstoneTricoreGCC::new()?;
    // ... use nx ...
    Ok(())
}
```

See [`bindings/rust-ipc/README.md`](bindings/rust-ipc/README.md) for full
details on the IPC architecture, daemon lifecycle, and license boundary.

### Python

```python
from nyxstone_tricore_gcc import LabelDefinition, NyxstoneTricoreGCC
nx = NyxstoneTricoreGCC()
bytes_ = nx.assemble("nop; ret", address=0)
for ins in nx.disassemble_to_instructions(bytes_, address=0x1000):
    print(f"0x{ins.address:08x}  {ins.assembly}")
ext = nx.assemble("nop\n j ext\n",
                  address=0x1000,
                  labels=[LabelDefinition("ext", 0x2000)])

# gcc/gas -r equivalent: bytes + relocations.
rel_bytes, relocs = nx.assemble_with_relocs(
    "nop\n j ext\n",
    address=0x1000,
    labels=[LabelDefinition("ext", 0x2000)])
# relocs[0] == RelocationInfo(offset=2, addend=0,
#                             symbol=RelocationSymbol(name="ext", address=0x2000),
#                             relocation_type=3)  # R_TRICORE_24REL
```

## Benchmarks

Hot-cache throughput measured by `examples/bench.cpp` (single-threaded,
20-core x86_64 i7-1370P, gcc 14.2.0, `-O3`).  Numbers are ops/second on the
`assemble` / `disassemble_to_instructions` path, fully resetting gas's
state and re-running the encoder on each call.

| backend | per-op (1-insn) | ops/s (1-insn) | insns/s (10-insn batch) |
|---|---|---|---|
| C++ in-process               | ~360 ns | ~2.78 M | ~4.84 M |
| Rust GPL (in-process)        | ~410 ns | ~2.45 M | ~4.70 M |
| **Rust MIT (IPC daemon)**    | **~6 µs** | **~167 k**  | **~1.14 M** |
| process-spawn (`tricore-elf-as` baseline) | ~2.6 ms | ~385  | – |

The MIT mode pays ~15× over the GPL in-process path at the 1-insn level for
the license-clean process boundary.  Batching client-side (e.g., 10
instructions per request) closes most of the gap.

Reproduce locally:

```sh
make && ./bench 2.0                                           # C++
cd bindings/rust     && cargo run --release --example bench   # GPL crate
cd bindings/rust-ipc && NYXSTONE_TCD_PATH=../rust/target/release/nyxstone-tcd \
                        cargo run --release --example bench   # MIT crate
```

## Layout

```
NyxstoneTricoreGCC/
├── include/nyxstone/
│   ├── nyxstone.h                 Public C++ API
│   └── expected.hpp               Sy Brand's tl::expected (vendored, header-only)
├── c_api/nyxstone_c.h             Public C ABI
├── src/
│   ├── nyxstone.cpp               C++ implementation
│   ├── nyxstone_glue.c            The only TU that touches gas internals
│   └── nyxstone_c.cpp             C ABI wrapper
├── tests/tests.cpp                118-test matrix + stress + round-trip + API checks
├── examples/{smoke,bench}.cpp     Demo and throughput benchmark
├── bindings/
│   ├── rust/                      nyxstone-tricore-gcc (GPL) crate + nyxstone-tcd daemon
│   │   └── src/bin/nyxstone-tcd.rs  GPL daemon binary served via cargo install
│   ├── rust-ipc/                  nyxstone-tricore-gcc-ipc (MIT) IPC client crate
│   └── python/                    setuptools + CFFI extension
├── scripts/
│   ├── fetch_binutils.sh          Clone + build binutils from source
│   ├── build_prebuilts.sh         Refresh all 4 prebuilt tarball variants
│   └── extract_prebuilt.sh        Extract host-matching prebuilt into third_party/
├── third_party/
│   ├── binutils-tricore-prebuilt/ Committed prebuilts (~3 MB compressed)
│   └── binutils-tricore/          Extracted-on-demand working tree (gitignored)
├── docs/architecture.md           How the library is plumbed together
├── Makefile                       Simple build
└── CMakeLists.txt                 Production build with install
```

## Tests

`tests/tests.cpp` ships a 118-test matrix split into six groups:

- **insn** (46): every TriCore format we exercise (SR/SRR/SLR/SSR/SC/SRC/RC/
  RR/RLC/B), various register kinds, immediate widths, signedness.
- **label** (12): forward + backward branches, multi-label lines
  (`a: b: nop`), `.L0`/`$x`/`_x` naming variants, label-only sources.
- **data** (40): every data directive Nyxstone supports, `.byte` /
  `.half` / `.short` / `.2byte` / `.word` / `.int` / `.long` / `.4byte` /
  `.quad` / `.8byte` / `.ascii` / `.asciz` / `.string` / `.skip` /
  `.space` / `.zero` / `.org` / `.align` / `.balign`.
- **mixed** (4): instructions + labels + data interleaved.
- **edge** (9): empty / comments-only / whitespace / `;` separators.
- **forbid** (10): `.text`-only restriction, `.data` / `.bss` /
  `.section .foo` / `.pushsection` must all reject; `.text` / `.section
  .text*` must accept.

After the core matrix:

- 100× stress per test catches state-reset drift across consecutive
  `assemble()` calls.
- Every BYTES test of ≥2 bytes is round-tripped through
  `disassemble_to_instructions()` (97 cases).
- Additional checks cover `LabelDefinition` external symbols (with
  address invariance), the `address` parameter propagating to
  `Instruction.address`, and the `count` parameter limiting decoded
  instructions.

The Rust crates run the same 10 unit tests (verbatim sources, only the
`use` line differs between `nyxstone-tricore-gcc` and `nyxstone-tricore-gcc-ipc`)
plus one doctest, and produce byte-identical output across both backends.

```
$ ./run_tests
... (118 tests in 6 groups) ...
--- 100x stress (each non-MUST_FAIL test 100 iterations) ---
  PASS: no drift across 118 non-fail tests * 100 iterations
--- disassembly round-trip ---
  97 round-trip pass, 0 fail
--- external label resolution ---
  2 external-label pass, 0 fail
--- assemble_to_instructions address ---
  1 assemble_to_instructions pass, 0 fail
--- disassemble count ---
  2 disassemble count pass, 0 fail
--- assemble_with_relocs ---
  4 assemble_with_relocs pass, 0 fail
--- assemble_to_instructions_with_relocs ---
  2 assemble_to_instructions_with_relocs pass, 0 fail
--- with_relocs ignores internal labels ---
  1 internal-label pass, 0 fail
Summary: 118 passed, 0 failed, 0 drifts (of 118 tests);
         97 disasm round-trips passed, 0 failed;
         12 additional API checks passed, 0 failed
```

## Threading

Single-threaded only at the gas layer.  All gas globals are process-wide;
concurrent calls from multiple threads would corrupt state.

- The C++ API does not lock, callers must serialize.
- The Rust GPL binding holds a global `Mutex<()>` per call so calls from
  multiple threads are safe (they're just serialized).  `NyxstoneTricoreGCC`
  is both `Send` and `Sync`.
- The Rust MIT binding holds a per-instance `Mutex` around the UDS socket,
  same FIFO contract, scoped per-handle since each MIT-mode instance has
  its own daemon process.
- The Python binding holds a `threading.Lock()` per call.

## License

**GPL-3.0-or-later** for the C++/C/Python parts and the `nyxstone-tricore-gcc`
Rust crate.  These statically link GNU assembler (`gas`) object files from
binutils, which are GPL-3.0-or-later; that propagates to the combined
library and to any binary distribution.  libbfd and libopcodes
(LGPL-3.0-or-later) are subsumed by the GPL term.

**MIT** for the `nyxstone-tricore-gcc-ipc` Rust crate, it ships
zero GPL bytes and only talks to a separate `nyxstone-tcd` daemon binary
(GPL-3.0+) over an IPC socket.  Shipping the daemon alongside a commercial
product follows the same well-trodden pattern as bundling `gcc.exe` next to
a closed-source IDE.

See [LICENSE](LICENSE), [LICENSE-MIT](LICENSE-MIT), and
[`bindings/rust-ipc/README.md`](bindings/rust-ipc/README.md) for the full
story on the license-mode split.
