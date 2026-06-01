// build.rs, extracts the bundled binutils-tricore prebuilts for the host
// arch + PIC variant, compiles the C++ wrapper sources via the `cc` crate,
// and emits cargo link directives for everything.
//
// All source files and prebuilt tarballs are picked up from the symlinked
// `nyxstone-tricore-gcc/` directory inside this crate.  This makes the
// crate self-contained for `cargo publish` / `cargo install`: no parent
// directory or external `make` step is required.
//
// External `tar`/`xz` binaries are used to extract the .tar.xz prebuilts
// (avoids pulling in big build-dependencies like xz2/tar-rs).
//
// Override / debug knobs:
//   NYX_PREBUILT_DIR=...  use a different binutils-tricore-prebuilt root
//   NYX_EXTRACTED_DIR=... reuse an already-extracted binutils-tricore tree

use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

const CXX_SOURCES: &[&str] = &["nyxstone.cpp", "nyxstone_c.cpp"];
const C_SOURCES:   &[&str] = &["nyxstone_glue.c"];

fn main() {
    for v in &["NYX_BINUTILS_PIC", "NYX_PREBUILT_DIR", "NYX_EXTRACTED_DIR"] {
        println!("cargo:rerun-if-env-changed={v}");
    }

    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let root     = manifest.join("nyxstone-tricore-gcc");
    let out_dir  = PathBuf::from(env::var("OUT_DIR").unwrap());
    let prebuilt = env::var("NYX_PREBUILT_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| root.join("binutils-prebuilt"));

    println!("cargo:rerun-if-changed={}", root.display());

    // ---- 1. Resolve host arch + PIC variant ---------------------------------
    let target_arch = env::var("CARGO_CFG_TARGET_ARCH").unwrap();
    let arch_dir = match target_arch.as_str() {
        "x86_64"  => "x86_64-linux-gnu",
        "aarch64" => "aarch64-linux-gnu",
        other => panic!(
            "nyxstone-tricore-gcc: no bundled prebuilt for target arch '{other}'.\n\
             Supported: x86_64, aarch64.  For other arches, build binutils from\n\
             source and point NYX_EXTRACTED_DIR at the resulting tree."
        ),
    };
    let pic = env::var("NYX_BINUTILS_PIC").unwrap_or_default() == "1";
    let variant = if pic { "pic" } else { "nopic" };

    // ---- 2. Extract the matching prebuilt tarballs -------------------------
    // Each tarball is small (~600 KB) and idempotent extraction is cheap, so
    // we just always re-extract into OUT_DIR/binutils-tricore/.  Reuse via
    // NYX_EXTRACTED_DIR for advanced users with a pre-extracted tree.
    let extracted = if let Ok(p) = env::var("NYX_EXTRACTED_DIR") {
        PathBuf::from(p)
    } else {
        let dst = out_dir.join("binutils-tricore");
        std::fs::create_dir_all(&dst).expect("create extracted dir");
        extract(&prebuilt.join("headers-shared.tar.xz"),                 &dst);
        extract(&prebuilt.join(arch_dir).join("headers-arch.tar.xz"),    &dst);
        extract(&prebuilt.join(arch_dir).join(variant).join("lib.tar.xz"), &dst);
        dst
    };

    // ---- 3. Compile the C++ + C wrapper sources ---------------------------
    let include_paths = include_paths(&extracted, &root);
    let mut cxx = cc::Build::new();
    cxx.cpp(true).std("c++17").flag("-Wno-unused-parameter");
    for p in &include_paths { cxx.include(p); }
    for src in CXX_SOURCES { cxx.file(root.join("src").join(src)); }
    cxx.define("PACKAGE", Some("\"nyxstone\""));
    cxx.define("PACKAGE_VERSION", Some("\"0\""));
    cxx.compile("nyxstone_tricore_cxx");

    let mut cc = cc::Build::new();
    cc.flag("-Wno-unused-parameter");
    for p in &include_paths { cc.include(p); }
    for src in C_SOURCES { cc.file(root.join("src").join(src)); }
    cc.compile("nyxstone_tricore_c");

    // ---- 4. Link binutils archives + every gas .o -------------------------
    let lib = extracted.join("lib");
    println!("cargo:rustc-link-search=native={}", lib.display());
    for l in &["opcodes", "bfd", "iberty", "sframe"] {
        println!("cargo:rustc-link-lib=static={l}");
    }
    for d in &["gas", "gas/config"] {
        let dir = lib.join(d);
        if !dir.is_dir() { continue; }
        for entry in std::fs::read_dir(&dir).expect("gas/ exists") {
            let p = entry.unwrap().path();
            if p.extension().and_then(|s| s.to_str()) == Some("o") {
                println!("cargo:rustc-link-arg={}", p.display());
            }
        }
    }
    for sys in &["z", "zstd", "dl", "m", "stdc++"] {
        println!("cargo:rustc-link-lib={sys}");
    }
}

fn include_paths(extracted: &Path, root: &Path) -> Vec<PathBuf> {
    vec![
        root.join("include"),
        root.join("c_api"),
        extracted.join("include"),
        extracted.join("binutils-include"),
        extracted.join("gas-internal-headers"),
        extracted.join("gas-internal-headers/config"),
        extracted.join("gas-internal-headers-build"),
        extracted.join("bfd-internal-headers"),
        extracted.join("bfd-internal-headers-build"),
        extracted.to_path_buf(),                      // resolves "bfd/elf-bfd.h"
    ]
}

fn extract(tar_xz: &Path, dst: &Path) {
    let status = Command::new("tar")
        .arg("-C").arg(dst)
        .arg("-xf").arg(tar_xz)
        .status()
        .unwrap_or_else(|e| panic!("`tar` not found on PATH: {e}"));
    if !status.success() {
        panic!("tar -xf {} failed (status {status})", tar_xz.display());
    }
}
