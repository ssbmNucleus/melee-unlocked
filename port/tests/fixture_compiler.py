"""The C and C++ compilers the source-fixture tests build with, or a CTest skip when there is none.

CTest runs these tests with CXX and CC naming the compilers CMake found for them (MU_FIXTURE_CXX and
MU_FIXTURE_CC in port/tests/test_labels.cmake: MinGW's g++/gcc on Windows, linking statically so the fixtures run
without MinGW on PATH). Run by hand, a test uses CXX/CC if set, else the first compiler on PATH.
A test that needs a compiler and finds none exits 77, which CTest reports as skipped
(SKIP_RETURN_CODE 77), with the reason on its output."""
import os
import shlex
import shutil
import sys
from pathlib import Path

SKIP = 77
HERE = Path(__file__).resolve().parent


def _split(value):
    # CMake hands over forward-slash paths; a backslash path from a user's shell is kept whole.
    return shlex.split(value) if '\\' not in value else value.split()


def _find(variable, names):
    if os.environ.get(variable):
        return _split(os.environ[variable])
    for name in names:
        if path := shutil.which(name):
            return [path]
    return None


def cxx():
    """The C++ compiler command as a list (with the Windows setenv/unsetenv shim), or None."""
    command = _find('CXX', ('c++', 'g++', 'clang++'))
    if command and os.name == 'nt':
        command += ['-include', str(HERE / 'posix_fixture_compat.h')]
    return command


def cc():
    """The C compiler command as a list, or None."""
    return _find('CC', ('cc', 'gcc', 'clang'))


def skip(reason):
    print('SKIP: ' + reason, flush=True)
    sys.exit(SKIP)


def require_cxx():
    command = cxx()
    if command is None:
        skip('this test compiles a fixture and no C++ compiler was found (set CXX, or put g++/clang++ on PATH)')
    return command


def require_cc():
    command = cc()
    if command is None:
        skip('this test compiles a fixture and no C compiler was found (set CC, or put gcc/clang on PATH)')
    return command
