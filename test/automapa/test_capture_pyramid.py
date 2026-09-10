#!/usr/bin/env python3
"""Exercise the real tile-pyramid capture caller using a configured native build."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CapturePyramidTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        database = Path(os.environ.get("TILE_TRACE_NATIVE_COMPILE_COMMANDS",
                                       ROOT / "build-linux-opengl/compile_commands.json"))
        if not database.is_file():
            raise unittest.SkipTest("Configure the native Linux build or set TILE_TRACE_NATIVE_COMPILE_COMMANDS")
        entry = next(row for row in json.loads(database.read_text()) if row["file"].endswith("/tile_pyramid.cpp"))
        original = shlex.split(entry["command"])
        flags = [original[0]]
        index = 1
        while index < len(original):
            flag = original[index]
            if flag in ("-I", "-isystem"):
                flags += original[index:index + 2]
                index += 2
                continue
            if flag.startswith(("-I", "-D", "-std=")):
                flags.append(flag)
            index += 1
        flags += ["-UNDEBUG", "-O1", "-pthread", "-ffunction-sections", "-fdata-sections", "-fno-access-control"]
        sources = ["src/mln/renderer/tile_pyramid.cpp", "src/mln/tile/tile_cache.cpp", "src/mln/util/tile_trace.cpp",
                   "test/automapa/capture_pyramid_harness.cpp"]
        with tempfile.TemporaryDirectory(prefix="capture-pyramid-") as directory:
            binary = str(Path(directory) / "pyramid")
            subprocess.run(flags + [str(ROOT / source) for source in sources] +
                           ["-Wl,--gc-sections", "-o", binary], check=True, timeout=120)
            lines = subprocess.check_output([binary], text=True, timeout=30).splitlines()
        cls.samples = {lines[i]: json.loads(lines[i + 1]) for i in range(0, len(lines), 2)}

    def test_resume_invalidates_cached_view_without_a_paused_frame(self):
        self.assertTrue(self.samples["resume_without_frame"]["fresh"])

    def test_paused_pyramid_does_not_allocate_or_change_view_serial(self):
        self.assertEqual(self.samples["paused_pyramid"], {"allocations": 0, "viewChanges": 0})

    def test_toggle_during_key_allocation_cannot_publish_stale_view(self):
        self.assertTrue(self.samples["toggle_while_preparing_keys"]["staleRejected"])


if __name__ == "__main__":
    unittest.main()
