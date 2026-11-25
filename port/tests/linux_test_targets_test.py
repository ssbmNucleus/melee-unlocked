"""The --test entry point builds the targets registered in CMake before running CTest."""
import sys
from pathlib import Path
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import build_linux

class LinuxTestTargets(unittest.TestCase):
    def test_the_build_precedes_ctest(self):
        with patch.object(sys, "platform", "linux"), patch.object(sys, "argv", ["build_linux", "--test", "--host-only"]), \
             patch.object(build_linux.shutil, "which", return_value="found"), patch.object(build_linux, "run") as run:
            build_linux.main()
        build, test = [call.args for call in run.call_args_list[-2:]]
        self.assertEqual(build[:2], ("cmake", "--build"))
        self.assertEqual(build[build.index("--target") + 1], "all")
        self.assertEqual(test[:2], ("ctest", "--test-dir"))
        self.assertEqual(build[2], test[2])

if __name__ == "__main__":
    unittest.main()
