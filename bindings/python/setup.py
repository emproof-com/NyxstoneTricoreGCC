"""setup.py, compiles the CFFI extension self-contained.

Everything (C++ wrapper sources, headers, binutils-tricore prebuilt tarballs)
lives under `nyxstone-tricore-gcc/` as a symlink view of the top-level repo,
which setuptools follows when building both sdist and wheels.  No external
`make` step is required.

Build flow:
  1. Pick host arch + PIC variant.
  2. Extract the matching binutils-tricore prebuilt tarballs into build/.
  3. Have CFFI compile our C++/C wrapper TUs + link binutils archives + every
     gas .o file in one shot.
"""

import glob
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

from setuptools import setup
from cffi import FFI

HERE = Path(__file__).resolve().parent
ROOT = HERE / "nyxstone-tricore-gcc"           # symlink view of the repo
PREBUILT = ROOT / "binutils-prebuilt"

# ----- 1. Choose prebuilt variant ------------------------------------------
_ARCH_MAP = {
    "x86_64":  "x86_64-linux-gnu",
    "aarch64": "aarch64-linux-gnu",
    "arm64":   "aarch64-linux-gnu",
}
arch = _ARCH_MAP.get(platform.machine())
if arch is None:
    sys.stderr.write(
        f"ERROR: no bundled binutils-tricore prebuilt for host '{platform.machine()}'.\n"
        f"  Supported: {sorted(set(_ARCH_MAP.values()))}\n"
        f"  Other arches: build from source and set NYX_EXTRACTED_DIR.\n")
    sys.exit(1)

# Python's CFFI extension is loaded as a shared object, so we need the PIC
# variant unless the caller explicitly opts out.
variant = "nopic" if os.environ.get("NYX_BINUTILS_PIC") == "0" else "pic"

# ----- 2. Extract prebuilts ------------------------------------------------
EXTRACTED = Path(os.environ.get(
    "NYX_EXTRACTED_DIR",
    HERE / "build" / "binutils-tricore"))


def _extract(tar_xz: Path, dst: Path) -> None:
    dst.mkdir(parents=True, exist_ok=True)
    subprocess.check_call(["tar", "-C", str(dst), "-xf", str(tar_xz)])


if "NYX_EXTRACTED_DIR" not in os.environ:
    if EXTRACTED.exists():
        shutil.rmtree(EXTRACTED)
    _extract(PREBUILT / "headers-shared.tar.xz",                EXTRACTED)
    _extract(PREBUILT / arch / "headers-arch.tar.xz",           EXTRACTED)
    _extract(PREBUILT / arch / variant / "lib.tar.xz",          EXTRACTED)

# ----- 3. Locate everything CFFI needs to compile + link -------------------
INCLUDE_DIRS = [
    str(ROOT / "c_api"),
    str(ROOT / "include"),
    str(EXTRACTED / "include"),
    str(EXTRACTED / "binutils-include"),
    str(EXTRACTED / "gas-internal-headers"),
    str(EXTRACTED / "gas-internal-headers/config"),
    str(EXTRACTED / "gas-internal-headers-build"),
    str(EXTRACTED / "bfd-internal-headers"),
    str(EXTRACTED / "bfd-internal-headers-build"),
    str(EXTRACTED),                                # resolves "bfd/elf-bfd.h"
]
SOURCES = [
    str(ROOT / "src/nyxstone.cpp"),
    str(ROOT / "src/nyxstone_glue.c"),
    str(ROOT / "src/nyxstone_c.cpp"),
]
gas_objs = sorted(glob.glob(str(EXTRACTED / "lib/gas/*.o")) +
                  glob.glob(str(EXTRACTED / "lib/gas/config/*.o")))

# ----- 4. Hand-written cdef (CFFI parser chokes on the real header) --------
ffibuilder = FFI()
ffibuilder.cdef("""
typedef struct nyxstone_handle nyxstone_handle_t;

typedef struct {
    const char* name;
    uint64_t    address;
} nyxstone_label_def_t;

typedef struct {
    uint64_t  address;
    char*     assembly;
    uint8_t*  bytes;
    size_t    bytes_len;
} nyxstone_instruction_t;

typedef struct {
    char*    name;
    uint64_t address;
} nyxstone_reloc_symbol_t;

typedef struct {
    uint64_t            offset;
    int64_t             addend;
    int                 has_addend;
    nyxstone_reloc_symbol_t  symbol;
    uint32_t            relocation_type;
} nyxstone_reloc_t;

nyxstone_handle_t* nyxstone_create(char**);
void                        nyxstone_destroy(nyxstone_handle_t*);

int nyxstone_assemble(nyxstone_handle_t*, const char*, size_t,
                 uint64_t, const nyxstone_label_def_t*, size_t,
                 uint8_t**, size_t*, char**);
int nyxstone_assemble_to_instructions(nyxstone_handle_t*, const char*, size_t,
                                 uint64_t, const nyxstone_label_def_t*, size_t,
                                 nyxstone_instruction_t**, size_t*, char**);
int nyxstone_assemble_with_relocs(nyxstone_handle_t*, const char*, size_t,
                             uint64_t, const nyxstone_label_def_t*, size_t,
                             uint8_t**, size_t*, nyxstone_reloc_t**, size_t*, char**);
int nyxstone_assemble_to_instructions_with_relocs(nyxstone_handle_t*, const char*, size_t,
                                             uint64_t, const nyxstone_label_def_t*, size_t,
                                             nyxstone_instruction_t**, size_t*,
                                             nyxstone_reloc_t**, size_t*, char**);
int nyxstone_disassemble(nyxstone_handle_t*, const uint8_t*, size_t,
                    uint64_t, size_t, char**, char**);
int nyxstone_disassemble_to_instructions(nyxstone_handle_t*, const uint8_t*, size_t,
                                    uint64_t, size_t,
                                    nyxstone_instruction_t**, size_t*, char**);

void nyxstone_free_bytes(uint8_t*);
void nyxstone_free_string(char*);
void nyxstone_free_instructions(nyxstone_instruction_t*, size_t);
void nyxstone_free_relocations(nyxstone_reloc_t*, size_t);
""")

ffibuilder.set_source(
    "nyxstone_tricore_gcc._native",
    '#include "nyxstone_c.h"',
    include_dirs    = INCLUDE_DIRS,
    sources         = SOURCES,
    extra_objects   = gas_objs,
    libraries       = ["opcodes", "bfd", "iberty", "sframe",
                       "z", "zstd", "dl", "m", "stdc++"],
    library_dirs    = [str(EXTRACTED / "lib")],
    extra_compile_args = ["-std=c++17"] if False else [],   # CFFI picks C++ via .cpp ext
    extra_link_args = ["-Wl,--no-as-needed"],
)

if __name__ == "__main__":
    ffibuilder.compile(verbose=True)
    setup()
