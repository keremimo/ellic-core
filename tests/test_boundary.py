# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
"""The core stands alone: it includes itself, the C++ and POSIX libraries and
two JUCE modules, never a product's code or a hardware runtime (AGENTS.md)."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
FOLDERS = {"engine", "model", "shell", "web", "plugin", "dsp"}
QUOTED = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)
ANGLED = re.compile(r'^\s*#\s*include\s+<([^>]+)>', re.MULTILINE)
JUCE = {"juce_core/juce_core.h", "juce_audio_formats/juce_audio_formats.h"}


def sources():
    return sorted(p for p in SRC.rglob("*") if p.suffix in (".h", ".cpp"))


class BoundaryTests(unittest.TestCase):
    def test_only_melodee_and_its_folders(self):
        self.assertEqual({p.name for p in SRC.iterdir()}, {"melodee"})
        self.assertEqual({p.name for p in (SRC / "melodee").iterdir() if p.is_dir()}, FOLDERS)
        self.assertEqual([p.name for p in (SRC / "melodee").iterdir() if p.is_file()], [])

    def test_quoted_includes_are_the_core_own(self):
        for path in sources():
            for name in QUOTED.findall(path.read_text()):
                with self.subTest(file=str(path.relative_to(ROOT)), include=name):
                    self.assertTrue(name.startswith("melodee/"), "the core includes only melodee/...")
                    self.assertIn(name.split("/")[1], FOLDERS)
                    self.assertTrue((SRC / name).is_file(), "not in the core")

    def test_juce_modules_are_the_two(self):
        # Any other library is the platform's (C++, POSIX, Linux, NEON).
        for path in sources():
            for name in ANGLED.findall(path.read_text()):
                if name.startswith("juce"):
                    with self.subTest(file=str(path.relative_to(ROOT)), include=name):
                        self.assertIn(name, JUCE, "a JUCE module the products may not build: add it to core.mk on purpose")

if __name__ == "__main__":
    unittest.main()
