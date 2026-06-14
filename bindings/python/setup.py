"""setup.py -- builds the CFFI extension via the standard `cffi_modules` hook.

The FFI declaration and source layout live in build_native.py; cffi turns it
into a proper setuptools Extension, so wheels come out platform-tagged and
contain the compiled `nyxstone_tricore_gcc._native` module.

Build flow (all inside the build_ext step, so metadata-only PEP 517 hooks
never do heavy work):
  1. Extract the matching binutils-tricore prebuilt tarballs into
     build/binutils-tricore/ (host arch + PIC variant).
  2. Inject the extracted gas .o files into the extension's extra_objects.
  3. Compile our C++/C wrapper TUs (-std=c++17 for the C++ ones only) and
     link the binutils archives in one shot.
"""

import os
import sys

from setuptools import setup
from setuptools.command.build_ext import build_ext as _build_ext

# Make build_native importable regardless of how the build frontend runs us.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_native  # noqa: E402


class nyx_build_ext(_build_ext):
    """Extract the bundled prebuilts and finalize link inputs at build time.

    cffi's `cffi_modules` machinery wraps this class: it first generates the
    _native C source into build_temp, then delegates to run() below."""

    def run(self):
        build_native.extract_prebuilts()
        for ext in self.extensions:
            if ext.name == "nyxstone_tricore_gcc._native":
                ext.extra_objects = build_native.gas_objects()
        super().run()

    def build_extensions(self):
        # The wrapper TUs are C++17, but distutils passes extra_compile_args
        # to every source, and -std=c++17 errors out on the .c file.  Wrap
        # the per-source compile hook to add it for C++ sources only.
        orig_compile = self.compiler._compile

        def _compile(obj, src, ext, cc_args, extra_postargs, pp_opts):
            postargs = list(extra_postargs)
            if src.endswith((".cpp", ".cc", ".cxx")):
                postargs.append("-std=c++17")
            return orig_compile(obj, src, ext, cc_args, postargs, pp_opts)

        self.compiler._compile = _compile
        super().build_extensions()


setup(
    cffi_modules=["build_native.py:ffibuilder"],
    cmdclass={"build_ext": nyx_build_ext},
)
