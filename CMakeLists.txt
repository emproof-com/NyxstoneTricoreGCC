# NyxstoneTricoreGCC CMake build.
#
# Quick start:
#   scripts/fetch_binutils.sh                    # one-time bootstrap
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#   ctest --test-dir build                       # 118-test matrix
#
# Override NYX_BINUTILS_DIR if you have an existing populated tree:
#   cmake -DNYX_BINUTILS_DIR=/path/to/binutils-tricore -S . -B build

cmake_minimum_required(VERSION 3.16)
project(nyxstone_tricore
    VERSION 0.1.0
    DESCRIPTION "In-process TriCore assembler/disassembler built on EEESlab tricore-binutils-gdb"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(NYX_BUILD_TESTS    "Build the test suite"     ON)
option(NYX_BUILD_EXAMPLES "Build smoke + bench"      ON)
# A shared library .so requires every binutils archive to have been built
# with -fPIC.  The default `scripts/fetch_binutils.sh` does NOT pass -fPIC
# (matches stock binutils builds).  Re-run with NYX_BINUTILS_PIC=1 first
# if you need the shared lib (typically: Python ctypes binding).
option(NYX_BUILD_SHARED   "Build .so (needs -fPIC binutils)" OFF)

set(NYX_BINUTILS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/binutils-tricore"
    CACHE PATH "Path to vendored binutils-tricore artifacts (populated by scripts/fetch_binutils.sh).")

if(NOT EXISTS "${NYX_BINUTILS_DIR}/.populated")
    message(FATAL_ERROR
        "NYX_BINUTILS_DIR (${NYX_BINUTILS_DIR}) is not populated.\n"
        "Run scripts/fetch_binutils.sh first, or set NYX_BINUTILS_DIR explicitly.")
endif()

# ---- gas .o files -------------------------------------------------------
file(GLOB NYX_GAS_OBJS
    "${NYX_BINUTILS_DIR}/lib/gas/*.o"
    "${NYX_BINUTILS_DIR}/lib/gas/config/*.o")

# ---- core sources -------------------------------------------------------
add_library(nyxstone_tricore_core OBJECT
    src/nyxstone.cpp
    src/nyxstone_glue.c
    src/nyxstone_c.cpp)

target_include_directories(nyxstone_tricore_core PUBLIC
    include
    c_api
    "${NYX_BINUTILS_DIR}/include")

# nyxstone_glue.c additionally needs the gas/bfd internal headers.
target_include_directories(nyxstone_tricore_core PRIVATE
    "${NYX_BINUTILS_DIR}/gas-internal-headers"
    "${NYX_BINUTILS_DIR}/gas-internal-headers/config"
    "${NYX_BINUTILS_DIR}/gas-internal-headers-build"
    "${NYX_BINUTILS_DIR}/bfd-internal-headers"
    "${NYX_BINUTILS_DIR}/bfd-internal-headers-build"
    "${NYX_BINUTILS_DIR}"
    "${NYX_BINUTILS_DIR}/binutils-include")

target_compile_definitions(nyxstone_tricore_core PRIVATE
    PACKAGE="nyxstone" PACKAGE_VERSION="${PROJECT_VERSION}")

# ---- binutils system deps ----------------------------------------------
find_package(ZLIB REQUIRED)
find_library(ZSTD_LIB zstd REQUIRED)

set(NYX_BINUTILS_ARCHIVES
    "${NYX_BINUTILS_DIR}/lib/libopcodes.a"
    "${NYX_BINUTILS_DIR}/lib/libbfd.a"
    "${NYX_BINUTILS_DIR}/lib/libiberty.a"
    "${NYX_BINUTILS_DIR}/lib/libsframe.a")

# ---- static lib ---------------------------------------------------------
add_library(nyxstone_tricore STATIC
    $<TARGET_OBJECTS:nyxstone_tricore_core>
    ${NYX_GAS_OBJS})

target_link_libraries(nyxstone_tricore PUBLIC
    ${NYX_BINUTILS_ARCHIVES}
    ZLIB::ZLIB ${ZSTD_LIB} dl m)

target_include_directories(nyxstone_tricore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/c_api>
    $<INSTALL_INTERFACE:include>)

# ---- shared lib (for Python ctypes) ------------------------------------
if(NYX_BUILD_SHARED)
    add_library(nyxstone_tricore_shared SHARED
        $<TARGET_OBJECTS:nyxstone_tricore_core>
        ${NYX_GAS_OBJS})
    set_target_properties(nyxstone_tricore_shared PROPERTIES
        OUTPUT_NAME nyxstone_tricore
        SOVERSION ${PROJECT_VERSION_MAJOR})
    target_link_libraries(nyxstone_tricore_shared PRIVATE
        ${NYX_BINUTILS_ARCHIVES}
        ZLIB::ZLIB ${ZSTD_LIB} dl m)
endif()

# ---- examples / tests ---------------------------------------------------
if(NYX_BUILD_EXAMPLES)
    add_executable(smoke examples/smoke.cpp)
    target_link_libraries(smoke PRIVATE nyxstone_tricore)

    add_executable(bench examples/bench.cpp)
    target_link_libraries(bench PRIVATE nyxstone_tricore)
endif()

if(NYX_BUILD_TESTS)
    enable_testing()
    add_executable(run_tests tests/tests.cpp)
    target_link_libraries(run_tests PRIVATE nyxstone_tricore)
    add_test(NAME tests COMMAND run_tests)
endif()

# ---- install ------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS nyxstone_tricore
    EXPORT  nyxstone_tricoreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(NYX_BUILD_SHARED)
    install(TARGETS nyxstone_tricore_shared
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY include/nyxstone DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES c_api/nyxstone_c.h    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
