# nyxstone-tricore-gcc-ipc

[![crates.io](https://img.shields.io/crates/v/nyxstone-tricore-gcc-ipc.svg)](https://crates.io/crates/nyxstone-tricore-gcc-ipc)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](#license)

**MIT licensed Rust bindings for NyxstoneTricoreGCC.**  Public API
is byte-for-byte identical to the GPL-licensed
[`nyxstone-tricore-gcc`](https://crates.io/crates/nyxstone-tricore-gcc) crate,
switch between modes by changing **one line** in your `Cargo.toml`:

```toml
# max performance, in-process (GPL-3.0-or-later):
nyxstone-tricore-gcc     = "0.2"

# permissive license, IPC mode (MIT):
nyxstone-tricore-gcc-ipc = "0.2"
```

User code stays the same.

## Usage

```rust
use nyxstone_tricore_gcc_ipc::{LabelDefinition, NyxstoneTricoreGCC};

let nx = NyxstoneTricoreGCC::new()?;
let bytes = nx.assemble("nop\n j ext", 0x1000,
                        &[LabelDefinition::new("ext", 0x2000)])?;
```

---

## Binary dependency: `nyxstone-tcd` (GPL-3.0-or-later)

This crate ships **zero GPL code**.  At runtime it spawns a separate daemon
binary, `nyxstone-tcd`, that links the GNU assembler (gas) in-process and
serves your requests over a UNIX socket.  The daemon is GPL-3.0-or-later
because gas is, but it lives in a different binary, so your application's
license stays untouched.

You must obtain the daemon binary out-of-band.  Three supported install
methods:

### Method 1, manual `cargo install` (simplest, recommended)

```sh
cargo install nyxstone-tricore-gcc       # produces ~/.cargo/bin/nyxstone-tcd
```

One-time per developer machine.  Compiles the daemon from source using the
bundled binutils-tricore prebuilts (~10 s on a typical x86_64 host, longer
on a fresh checkout).

### Method 2, `cargo binstall` (fastest, when available)

If [`cargo-binstall`](https://github.com/cargo-bins/cargo-binstall) is on
PATH:

```sh
cargo binstall nyxstone-tricore-gcc      # downloads pre-built binary (~1 s)
```

Skips the compile step entirely, downloads a pre-built `nyxstone-tcd` from
the GitHub release matching the crate version.

### Method 3, programmatic bootstrap (zero user steps)

Call once at startup:

```rust
fn main() -> Result<(), Box<dyn std::error::Error>> {
    // Ensures nyxstone-tcd is installed; no-op if already present.
    // Prefers `cargo binstall` if available, else falls back to `cargo install`.
    nyxstone_tricore_gcc_ipc::install_daemon_if_missing()?;

    // Now use the assembler normally.
    let nx = nyxstone_tricore_gcc_ipc::NyxstoneTricoreGCC::new()?;
    // ...
    Ok(())
}
```

The helper is idempotent and returns immediately when the daemon is already
locatable.  Trade-off: a one-time ~1, 10 s pause on first run, in exchange
for true zero-config developer onboarding.

There's also `install_daemon()` (force-reinstall, useful when bumping the
crate version) and `daemon_path()` (locate-only, no install).

### Method 4, bring your own binary

For **production deployments** (where `cargo` isn't on the customer's
machine), ship the daemon binary alongside your product and point us at it:

```sh
NYXSTONE_TCD_PATH=/opt/myapp/bin/nyxstone-tcd myapp
```

This is the standard pattern for any commercial product shipping a GPL
companion tool, same as bundling `gcc.exe` next to a closed-source IDE.

### How daemon lookup works

When `NyxstoneTricoreGCC::new()` is called, the crate looks for the daemon
in this order:

1. **`$NYXSTONE_TCD_PATH`**, explicit override, used in production.
2. **`nyxstone-tcd` on `$PATH`**, found if you used Method 1, 2, or 3.

If neither resolves, you get a clear `Error::Init` message listing the
install options.

---

## How the IPC mode works

- **One daemon per `NyxstoneTricoreGCC` instance**, owned by that instance.
  Instances (and processes) never share daemons.
- **Lazy spawn.**  No background process unless you actually call `new()`.
- **Socket-lifetime-bound.**  The daemon exits when its socket reads EOF:
  on instance drop, or when the parent process dies for any reason (crash,
  `kill -9`, normal exit — the OS closes the socket).  `PR_SET_PDEATHSIG`
  is deliberately not used: it is parent-*thread*-scoped on Linux and would
  kill a daemon whose spawning thread exited while the instance lives on.
- **Strict FIFO.**  Calls from multiple threads serialize through a
  per-instance mutex.
- **Timeouts + automatic respawn.**  Each request is bounded by a 30 s
  read/write socket timeout (override with `NYXSTONE_TCD_TIMEOUT_MS`, read
  once at instance creation; `0` disables it).  On any transport error
  (timeout, daemon crash, connection reset) the stream is never reused: the
  daemon is respawned once and the request retried exactly once.

## Performance

| backend | per-op | ops/s (1-insn) | insns/s (10-insn batch) |
|---|---|---|---|
| `nyxstone-tricore-gcc`     (GPL, in-process) | ~360 ns | ~2.45 M | ~4.70 M |
| `nyxstone-tricore-gcc-ipc` (MIT, IPC)        | ~6 µs   | ~167 k  | ~1.14 M |

The IPC mode pays about a 15× tax for the license-clean process boundary at
the single-op level.  Batching client-side (e.g., processing 10 sources per
request) closes most of the gap.

## License

`MIT` for everything in this crate. See
[`LICENSE-MIT`](../../LICENSE-MIT).

The `nyxstone-tcd` daemon binary is **GPL-3.0-or-later** (inherits from
GNU binutils) and ships separately from the GPL `nyxstone-tricore-gcc`
crate.  You redistribute it under GPL terms, same as shipping `gcc.exe`
alongside a commercial product.
