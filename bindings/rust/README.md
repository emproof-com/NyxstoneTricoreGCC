# nyxstone-tricore-gcc (Rust)

Safe Rust bindings to the [NyxstoneTricoreGCC](..) in-process TriCore
assembler/disassembler.

The API mirrors that of the sibling project
[Nyxstone](https://github.com/emproof-com/nyxstone) (LLVM-MC based, covers
LLVM-supported architectures; this crate uses GNU binutils for TriCore).
Four methods named `assemble`, `assemble_to_instructions`, `disassemble`,
and `disassemble_to_instructions`, each taking an absolute `address` (and
for the assembly entry points, a slice of `LabelDefinition`s).

## Usage

```rust
use nyxstone_tricore_gcc::{LabelDefinition, NyxstoneTricoreGCC};

let nx = NyxstoneTricoreGCC::new()?;

// Assemble.
let bytes = nx.assemble("start:\n nop\n j here\nhere:\n ret\n", 0, &[])?;
assert_eq!(bytes, [0x00, 0x00, 0x1d, 0x00, 0x00, 0x00, 0x00, 0x90]);

// Assemble with an external label.
let bytes2 = nx.assemble(
    "nop\n j ext\n",
    /*address=*/ 0x1000,
    &[LabelDefinition::new("ext", 0x2000)],
)?;

// Disassemble.
for ins in nx.disassemble_to_instructions(&bytes, 0x80000000, /*count=*/0)? {
    println!("0x{:08x}  {}", ins.address, ins.assembly);
}

// Or disassemble to a single string.
println!("{}", nx.disassemble(&bytes, 0x80000000, 0)?);
# Ok::<(), nyxstone_tricore_gcc::Error>(())
```

## Build

Self-contained.  `cargo build` extracts the bundled binutils-tricore
prebuilts (committed under
[`../../third_party/binutils-tricore-prebuilt/`](../../third_party/binutils-tricore-prebuilt/))
into `OUT_DIR`, compiles the C++ wrapper sources via the `cc` crate, and
links everything together.  No external `make` step required.

```sh
cargo build --release
cargo test  --release
cargo run   --release --example smoke
cargo run   --release --example bench
```

The crate also ships the `nyxstone-tcd` daemon binary used by the
permissive [`nyxstone-tricore-gcc-ipc`](../rust-ipc/) crate:

```sh
cargo install --path .                 # installs ~/.cargo/bin/nyxstone-tcd
# or, when published to crates.io:
cargo install nyxstone-tricore-gcc
```

## Threading

`NyxstoneTricoreGCC` is both `Send` and `Sync`.  A process-wide mutex
serializes every C-ABI call.  Multiple threads can share a
`NyxstoneTricoreGCC` (or hold
their own), they just won't actually run concurrently.  This matches
the underlying C++ library's process-wide-globals constraint while
keeping the Rust API ergonomic.
