# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
"""The core's own tests (tests/core_tests.cpp, `make test`), built on Linux."""
from pathlib import Path
import os
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform.startswith("linux"), "the core uses Linux calls: run it in a product's build image")
class CoreTests(unittest.TestCase):
    def test_engine_model_and_files(self):
        jobs = str(os.cpu_count() or 2)
        result = subprocess.run(["make", "-C", str(ROOT), "-j" + jobs, "test"], capture_output=True, text=True, timeout=1200)
        self.assertEqual(result.returncode, 0, result.stdout[-3000:] + result.stderr[-3000:])
        self.assertIn("core ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
