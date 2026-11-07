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

You must obtain the daemon binary out-of-band.  **`NyxstoneTricoreGCC::new()`
only ever *locates* the daemon — it never installs anything** — so pick the
method that fits how your program is distributed.

### Deployed / containerized builds: ship the daemon (recommended)

For anything you ship (a product, a CLI, a CI/container image), **bundle
`nyxstone-tcd` with your executable**.  Do **not** rely on a runtime
`cargo install` — a minimal container usually has neither `cargo` nor network,
so it fails.  The daemon must be the **matching crate version** (protocols are
version-locked).  It is found automatically if you put it in any of:

```sh
# 1. next to your binary (zero config -- found via the executable's directory)
/opt/myapp/bin/myapp
/opt/myapp/bin/nyxstone-tcd

# 2. on PATH
# 3. or point at it explicitly:
NYXSTONE_TCD_PATH=/opt/myapp/bin/nyxstone-tcd myapp
```

Then just call `NyxstoneTricoreGCC::new()`.  This is the standard pattern for a
product shipping a GPL companion tool, like bundling `gcc` next to a
closed-source IDE.  Grab a matching daemon with `cargo install
nyxstone-tricore-gcc@<version>` (or `cargo binstall`) on your **build** host and
copy the resulting `nyxstone-tcd` into your artifact.

### Local development: auto-install (convenience)

For a local dev tool that wants zero-setup first run, call once at startup:

```rust
fn main() -> Result<(), Box<dyn std::error::Error>> {
    // DEV ONLY: shells out to cargo (needs cargo + network), pinned to this
    // crate's version.  A shipped/containerized binary must NOT do this --
    // bundle the daemon instead (see above).
    nyxstone_tricore_gcc_ipc::install_daemon_if_missing()?;
    let nx = nyxstone_tricore_gcc_ipc::NyxstoneTricoreGCC::new()?;
    // ...
    Ok(())
}
```

It is idempotent (no-op if the daemon is already locatable), prefers
`cargo binstall` when present (no release binaries exist yet, so it builds
from source like `cargo install`), and pins the install to this crate's exact
version.  There's also `install_daemon()` (force-reinstall) and `daemon_path()`
(locate-only, no install).

### How daemon lookup works

`NyxstoneTricoreGCC::new()` (and `daemon_path()`) look for the daemon in order:

1. **`$NYXSTONE_TCD_PATH`** — explicit override.
2. **The directory of the current executable** — ship it next to your binary.
3. **`nyxstone-tcd` on `$PATH`**.

If none resolve, `new()` returns a clear `Error::Init` telling you to ship the
daemon (it does **not** try to install).  It never panics — but note that if
your program `unwrap()`s that `Result`, the panic is on your side; handle the
error or ensure the daemon is present.

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
