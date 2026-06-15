# NyxstoneTricoreGCC build.
#
# Quick start:
#   make                         # auto-extracts committed prebuilts, then
#                                # builds the library, tests, and examples
#   make test                    # run the 118-test matrix
#   make bench                   # run the throughput benchmark
#
# Binutils provisioning (in order of preference):
#   1. third_party/binutils-tricore/ already populated → use as-is.
#   2. third_party/binutils-tricore-prebuilt/ has a tarball for this host
#      → extract automatically (~1 s, no network).
#   3. Otherwise → run scripts/fetch_binutils.sh (clone + build, ~75 s).
#
# Force-rebuild from source: `make fetch_binutils`.
# Use the PIC variant (needed for Python ctypes etc.): `NYX_BINUTILS_PIC=1 make`.
# Override the binutils tree entirely: `make NYX_BINUTILS=/path/to/tree`.

NYX_BINUTILS ?= third_party/binutils-tricore

# Provision binutils at Makefile-parse time (not as a rule) so $(wildcard ...)
# below sees the staged .o files even on the first `make` invocation.  The
# extract script is idempotent and re-extracts only when the requested variant
# (controlled by NYX_BINUTILS_PIC) doesn't match what's already on disk.
# Skipped for housekeeping targets that don't touch binutils.
SKIP_PROVISION_GOALS := clean prebuilt fetch_binutils
ifeq ($(filter-out $(SKIP_PROVISION_GOALS),$(or $(MAKECMDGOALS),all)),$(or $(MAKECMDGOALS),all))
  PROVISION_LOG := $(shell \
    if scripts/extract_prebuilt.sh >&2; then \
        echo "prebuilt"; \
    else \
        echo ">>> No matching prebuilt for $$(uname -m); building from source." >&2; \
        scripts/fetch_binutils.sh >&2 && echo "fetched"; \
    fi)
endif

CXX        ?= g++
CC         ?= gcc
CXXFLAGS   ?= -O3 -std=c++17 -Wall -Wextra -fPIC
CFLAGS_C   ?= -O3 -fPIC
CPPFLAGS   ?= -DPACKAGE=\"nyxstone\" -DPACKAGE_VERSION=\"0\"

INCLUDES   := -I include -I c_api -I $(NYX_BINUTILS)/include

# Compiling nyxstone_glue.c needs gas's internal headers + the bfd headers
# + the binutils-src/include/ headers (libiberty.h, dwarf2.h, ...).
GLUE_INCLUDES := -I $(NYX_BINUTILS)/gas-internal-headers \
                 -I $(NYX_BINUTILS)/gas-internal-headers/config \
                 -I $(NYX_BINUTILS)/gas-internal-headers-build \
                 -I $(NYX_BINUTILS)/bfd-internal-headers \
                 -I $(NYX_BINUTILS)/bfd-internal-headers-build \
                 -I $(NYX_BINUTILS) \
                 -I $(NYX_BINUTILS)/binutils-include \
                 -I $(NYX_BINUTILS)/include \
                 -I $(NYX_BINUTILS)/include/opcode \
                 -I $(NYX_BINUTILS)/include/elf

# gas .o files we link (every .o in third_party/lib/gas/ + tc-tricore.o, etc.)
GAS_OBJS := $(wildcard $(NYX_BINUTILS)/lib/gas/*.o) \
            $(wildcard $(NYX_BINUTILS)/lib/gas/config/*.o)

# Library archives.
BINUTILS_LIBS := -L $(NYX_BINUTILS)/lib \
                 -l:libopcodes.a -l:libbfd.a -l:libiberty.a -l:libsframe.a
SYSTEM_LIBS   := -lz -lzstd -ldl -lm -lstdc++

.PHONY: all clean test bench smoke prebuilt fetch_binutils

all: libnyxstone_tricore.a smoke bench run_tests roundtrip_all

# Explicit re-provision targets (override the cached state).
prebuilt:
	@rm -f $(NYX_BINUTILS)/.populated
	@scripts/extract_prebuilt.sh

fetch_binutils:
	@rm -rf $(NYX_BINUTILS)
	@scripts/fetch_binutils.sh

# ---- Object compilation -------------------------------------------------
nyxstone.o: src/nyxstone.cpp include/nyxstone/nyxstone.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(INCLUDES) -c $< -o $@

nyxstone_glue.o: src/nyxstone_glue.c
	$(CC) $(CFLAGS_C) $(GLUE_INCLUDES) -c $< -o $@

nyxstone_c.o: src/nyxstone_c.cpp c_api/nyxstone_c.h include/nyxstone/nyxstone.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(INCLUDES) -c $< -o $@

# ---- Library ------------------------------------------------------------
libnyxstone_tricore.a: nyxstone.o nyxstone_glue.o nyxstone_c.o
	ar rcs $@ $^

# Shared library variant (for Python ctypes etc.).
libnyxstone_tricore.so: nyxstone.o nyxstone_glue.o nyxstone_c.o
	$(CXX) -shared -o $@ $^ $(GAS_OBJS) $(BINUTILS_LIBS) $(SYSTEM_LIBS)

# ---- Examples and tests -------------------------------------------------
smoke: examples/smoke.cpp libnyxstone_tricore.a
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(INCLUDES) $< \
	  libnyxstone_tricore.a $(GAS_OBJS) $(BINUTILS_LIBS) $(SYSTEM_LIBS) -o $@

bench: examples/bench.cpp libnyxstone_tricore.a
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(INCLUDES) $< \
	  libnyxstone_tricore.a $(GAS_OBJS) $(BINUTILS_LIBS) $(SYSTEM_LIBS) -o $@

run_tests: tests/tests.cpp libnyxstone_tricore.a
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(INCLUDES) $< \
	  libnyxstone_tricore.a $(GAS_OBJS) $(BINUTILS_LIBS) $(SYSTEM_LIBS) -o $@

# Exhaustive v1.6.2 round-trip + reference-resolution test (every instruction
# from the binutils opcode table; see tests/roundtrip_all.cpp).
roundtrip_all: tests/roundtrip_all.cpp tests/tricore_v162_insns.inc libnyxstone_tricore.a
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(INCLUDES) $< \
	  libnyxstone_tricore.a $(GAS_OBJS) $(BINUTILS_LIBS) $(SYSTEM_LIBS) -o $@

test: run_tests roundtrip_all
	./run_tests
	./roundtrip_all

clean:
	rm -f *.o *.a *.so smoke bench run_tests roundtrip_all
