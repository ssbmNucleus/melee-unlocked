"""Run a test program, or skip (exit 77) when it has not been built.

ctest counts a test whose program is missing as a failure ("Not Run"). The port_slippi_* and
port_mods_menu_fuzz programs are often not built (several link the whole static runtime), so a missing
one is a skip with the command that builds it, not a failure.

    run_if_built.py <program> <target> [args...]
"""
import subprocess
import sys
from pathlib import Path

SKIP = 77


def main():
    program, target, args = Path(sys.argv[1]), sys.argv[2], sys.argv[3:]
    if not program.is_file():
        print(f"SKIP: {program.name} is not built (cmake --build <build dir> --config Release --target {target})")
        return SKIP
    return subprocess.call([str(program), *args])


if __name__ == "__main__":
    sys.exit(main())
