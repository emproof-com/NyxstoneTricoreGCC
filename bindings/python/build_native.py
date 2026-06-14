"""CFFI builder for the ``nyxstone_tricore_gcc._native`` extension.

Referenced from setup.py via ``cffi_modules=["build_native.py:ffibuilder"]``.
Module scope only declares the cdef and the source/include layout -- it must
stay cheap, because metadata-only PEP 517 hooks (egg_info / dist-info) execute
this file too.  The heavy work (extracting the bundled binutils-tricore
prebuilt tarballs into build/) is done by :func:`extract_prebuilts`, which
setup.py's ``build_ext`` subclass calls right before compiling.

Everything (C++ wrapper sources, headers, binutils-tricore prebuilt tarballs)
lives under ``nyxstone-tricore-gcc/``, a symlink view of the top-level repo
that gets materialized into real files inside the sdist.  No external ``make``
step is required.

All paths are relative to the project root (the directory containing
setup.py), which is the cwd for PEP 517 builds and for
``python setup.py build_ext --inplace``.
"""

import glob
import os
import platform
import shutil
import subprocess

from cffi import FFI

ROOT = "nyxstone-tricore-gcc"                  # symlink view of the repo
PREBUILT = os.path.join(ROOT, "binutils-prebuilt")

# ----- Prebuilt variant selection -------------------------------------------
_ARCH_MAP = {
    "x86_64":  "x86_64-linux-gnu",
    "aarch64": "aarch64-linux-gnu",
    "arm64":   "aarch64-linux-gnu",
}


def host_arch():
    """Prebuilt directory name for this host, or None if unsupported."""
    return _ARCH_MAP.get(platform.machine())


def pic_variant():
    """Python's CFFI extension is loaded as a shared object, so we need the
    PIC variant unless the caller explicitly opts out."""
    return "nopic" if os.environ.get("NYX_BINUTILS_PIC") == "0" else "pic"


# Where the prebuilts get extracted to (overridable for cross/dev setups).
EXTRACTED = os.environ.get("NYX_EXTRACTED_DIR",
                           os.path.join("build", "binutils-tricore"))


def _extract(tar_xz, dst):
    os.makedirs(dst, exist_ok=True)
    subprocess.check_call(["tar", "-C", dst, "-xf", tar_xz])


def extract_prebuilts():
    """Extract the matching prebuilt tarballs into EXTRACTED.

    Called from setup.py's build_ext at build time, never at module import,
    so metadata-only PEP 517 hooks stay cheap.  A no-op when the caller
    points NYX_EXTRACTED_DIR at an already-populated tree."""
    if "NYX_EXTRACTED_DIR" in os.environ:
        return
    arch = host_arch()
    if arch is None:
        raise RuntimeError(
            f"no bundled binutils-tricore prebuilt for host "
            f"'{platform.machine()}'.\n"
            f"  Supported: {sorted(set(_ARCH_MAP.values()))}\n"
            f"  Other arches: build from source and set NYX_EXTRACTED_DIR.")
    if os.path.exists(EXTRACTED):
        shutil.rmtree(EXTRACTED)
    _extract(os.path.join(PREBUILT, "headers-shared.tar.xz"), EXTRACTED)
    _extract(os.path.join(PREBUILT, arch, "headers-arch.tar.xz"), EXTRACTED)
    _extract(os.path.join(PREBUILT, arch, pic_variant(), "lib.tar.xz"),
             EXTRACTED)


def gas_objects():
    """Pre-compiled gas .o files to link in.  Only valid after
    :func:`extract_prebuilts` has run."""
    return sorted(glob.glob(os.path.join(EXTRACTED, "lib/gas/*.o")) +
                  glob.glob(os.path.join(EXTRACTED, "lib/gas/config/*.o")))


# ----- Everything CFFI needs to compile + link ------------------------------
INCLUDE_DIRS = [
    os.path.join(ROOT, "c_api"),
    os.path.join(ROOT, "include"),
    os.path.join(EXTRACTED, "include"),
    os.path.join(EXTRACTED, "binutils-include"),
    os.path.join(EXTRACTED, "gas-internal-headers"),
    os.path.join(EXTRACTED, "gas-internal-headers/config"),
    os.path.join(EXTRACTED, "gas-internal-headers-build"),
    os.path.join(EXTRACTED, "bfd-internal-headers"),
    os.path.join(EXTRACTED, "bfd-internal-headers-build"),
    EXTRACTED,                                 # resolves "bfd/elf-bfd.h"
]
SOURCES = [
    os.path.join(ROOT, "src/nyxstone.cpp"),
    os.path.join(ROOT, "src/nyxstone_glue.c"),
    os.path.join(ROOT, "src/nyxstone_c.cpp"),
]

# ----- Hand-written cdef (CFFI parser chokes on the real header) ------------
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
    # extra_objects (the extracted gas .o files) are injected at build time
    # by setup.py's build_ext, after extract_prebuilts() has run.
    extra_objects   = [],
    libraries       = ["opcodes", "bfd", "iberty", "sframe",
                       "z", "zstd", "dl", "m", "stdc++"],
    library_dirs    = [os.path.join(EXTRACTED, "lib")],
    # -std=c++17 is added per-source (only for the .cpp TUs) by setup.py's
    # build_ext; passing it here would also hit the C sources and error.
    extra_link_args = ["-Wl,--no-as-needed"],
)
