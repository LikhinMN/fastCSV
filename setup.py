import os, sys, platform
from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext
import numpy as np

class BuildExt(build_ext):
    def build_extensions(self):
        ct = self.compiler.compiler_type
        for ext in self.extensions:
            if ct == 'msvc':
                ext.extra_compile_args = ['/O2', '/arch:AVX2', '/DNDEBUG']
            else:
                ext.extra_compile_args = [
                    '-O3', '-std=c11', '-DNDEBUG',
                    '-Wall', '-Wextra',
                    '-march=native',        # enable AVX2/SSE4.2 if available
                    '-fvisibility=hidden',  # only PyInit_fastcsv exported
                ]
                if platform.system() == 'Linux':
                    ext.extra_link_args = ['-Wl,--strip-all']
        super().build_extensions()

ext = Extension(
    "fastcsv.fastcsv",
    sources=[
        "src/pymodule.c",
        "src/parser.c",
        "src/mmap_io.c",
        "src/simd.c",
        "src/type_infer.c",
        "src/columnar.c",
    ],
    include_dirs=["src", np.get_include()],
)

setup(
    name="fastcsv",
    version="0.1.0",
    description="Fast CSV parsing for Python via C + SIMD",
    long_description=open("README.md").read(),
    long_description_content_type="text/markdown",
    packages=["fastcsv"],
    ext_modules=[ext],
    cmdclass={"build_ext": BuildExt},
    python_requires=">=3.8",
    install_requires=["numpy>=1.20"],
    extras_require={"bench": ["pandas", "polars"]},
    classifiers=[
        "Programming Language :: Python :: 3",
        "Programming Language :: C",
        "License :: OSI Approved :: MIT License",
        "Operating System :: POSIX :: Linux",
        "Operating System :: MacOS",
        "Operating System :: Microsoft :: Windows",
    ],
)
