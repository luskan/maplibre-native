#!/usr/bin/env python3
"""Compare indexed capture with the pre-optimization collector and count work."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RecordLookupTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        baseline_path = os.environ.get("TILE_TRACE_BASELINE_SOURCE")
        source = Path(baseline_path).read_text() if baseline_path else ""
        source = source.replace(
            "ID updateView(ID map, ID source, ID style, const ViewTile* tiles, size_t count) noexcept",
            "ID updateView(ID map, ID source, ID style, const ViewTile* tiles, size_t count, uint64_t) noexcept")
        source = source.replace(
            "      std::copy_n(batch.members.begin(), batch.count, destination.batch.members.begin());",
            '      std::copy_n(batch.members.begin(), batch.count, destination.batch.members.begin());\n'
            '      TILE_TRACE_TEST_HOOK("batch_published");\n'
            '      TILE_TRACE_TEST_COUNT("batch_members_copied", batch.count);')
        source = source.replace("  if (settled(use)) return;", '  if (settled(use)) return;\n  TILE_TRACE_TEST_HOOK("use_refresh");')
        source = source.replace("    for (auto& demand : c.records)\n    {",
                                '    for (auto& demand : c.records)\n    {\n      TILE_TRACE_TEST_HOOK("record_candidate");')
        cls.samples = {}
        with tempfile.TemporaryDirectory(prefix="record-lookup-") as directory:
            baseline = Path(directory) / "baseline.cpp"
            baseline.write_text(source)
            for variant in (("baseline", "indexed") if baseline_path else ("indexed",)):
                binary = str(Path(directory) / variant)
                flags = [f'-DTILE_TRACE_SOURCE="{baseline}"'] if variant == "baseline" else ["-DTILE_TRACE_INDEX_TESTS"]
                subprocess.run(["g++", "-std=c++17", "-pthread", "-DTILE_TRACE_TESTING", *flags,
                                "-I", str(ROOT / "include"), str(ROOT / "test/automapa/record_lookup_harness.cpp"),
                                "-o", binary], check=True)
                lines = subprocess.check_output([binary], text=True, timeout=60).splitlines()
                cls.samples[variant] = {lines[i]: json.loads(lines[i + 1]) for i in range(0, len(lines), 2)}

    def test_snapshots_match_original_collector(self):
        if "baseline" not in self.samples:
            self.skipTest("Set TILE_TRACE_BASELINE_SOURCE for the optional historical comparison")
        for name, expected in self.samples["baseline"].items():
            if name.startswith("semantic_"):
                with self.subTest(name=name):
                    self.assertEqual(self.samples["indexed"][name], expected)

    def test_zero_publication_and_late_receivers_submit(self):
        data = self.samples["indexed"]
        for name in ("semantic_zero_publication", "semantic_zero_retained"):
            demands = [r for r in data[name]["records"] if r["kind"] == 0]
            self.assertEqual(len(demands), 1)
            self.assertEqual(demands[0]["outcome"], "submitted")
            self.assertEqual(demands[0]["publication"], "0")
        demands = [r for r in data["semantic_late_receivers"]["records"] if r["kind"] == 0]
        self.assertEqual(len(demands), 3)
        self.assertTrue(all(r["outcome"] == "submitted" for r in demands))

    def test_stable_redraw_avoids_full_scan_and_index_changes(self):
        after = self.samples["indexed"]["stable_redraw"]
        if "baseline" in self.samples:
            self.assertEqual(self.samples["baseline"]["stable_redraw"]["visits"], 1024)
        self.assertLess(after["visits"], 16)
        self.assertEqual(after["relinks"], 0)
        self.assertEqual(self.samples["indexed"]["low_occupancy"]["visits"],
                         self.samples["indexed"]["unrelated_occupancy"]["visits"])
        self.assertTrue(self.samples["indexed"]["collision_invariants"]["ok"])

    def test_completed_invalidation_copies_nothing(self):
        after = self.samples["indexed"]["completed_invalidation"]
        if "baseline" in self.samples:
            self.assertGreater(self.samples["baseline"]["completed_invalidation"]["bytes"], 0)
        self.assertEqual((after["copies"], after["bytes"], after["refreshes"]), (0, 0, 0))

    def test_scoped_and_repeated_invalidations_do_only_needed_work(self):
        after = self.samples["indexed"]["scoped_pending"]
        if "baseline" in self.samples:
            self.assertEqual(self.samples["baseline"]["scoped_pending"]["refreshes"], 2)
        self.assertEqual(after["refreshes"], 1)
        self.assertEqual(after["copies"], 1)
        repeated = self.samples["indexed"]["repeated_pending"]
        self.assertEqual((repeated["copies"], repeated["bytes"]), (0, 0))
        empty = self.samples["indexed"]["no_overlap"]
        self.assertEqual((empty["copies"], empty["refreshes"]), (0, 0))

    def test_dirty_state_and_concurrent_loss_are_not_discarded(self):
        value = self.samples["indexed"]["semantic_already_dirty"]
        self.assertTrue(value["members"][0]["uses"][0]["failed"])
        self.assertEqual(self.samples["indexed"]["already_dirty"]["copies"], 1)
        lost = self.samples["indexed"]["loss_during_scope"]
        self.assertEqual((lost["status"], lost["screenReason"]), ("partial", "capture"))

    def test_swap_evicts_bucket_head_but_updates_surviving_receiver(self):
        rows = self.samples["indexed"]["semantic_primary_evicts_candidate"]["records"]
        survivor = next(r for r in rows if int(r["id"]) == 100 * 1024 + 100)
        evicted = next(r for r in rows if int(r["id"]) == 401 * 1024 + 10)
        self.assertEqual(survivor["outcome"], "submitted")
        self.assertEqual(survivor["timesUs"][13], "3000")
        self.assertNotEqual(evicted["outcome"], "submitted")
        self.assertEqual(evicted["timesUs"][13], "0")

    def test_full_capacity_copies_only_the_changed_batch(self):
        values = self.samples["indexed"]
        self.assertEqual(values["capacity_completed"]["bytes"], 0)
        self.assertEqual(values["capacity_completed"]["refreshes"], 0)
        self.assertEqual(values["capacity_one_pending"]["copies"], 1)
        self.assertEqual(values["capacity_one_pending"]["refreshes"], 1)
        self.assertEqual(values["capacity_repeated"]["bytes"], 0)
        if "baseline" in self.samples:
            self.assertEqual(self.samples["baseline"]["capacity_completed"]["bytes"], 3964928)
            self.assertEqual(values["capacity_one_pending"]["bytes"] * 16,
                             self.samples["baseline"]["capacity_one_pending"]["bytes"])

    def test_wrapped_pending_use_is_refreshed_and_empty_view_is_noop(self):
        value = self.samples["indexed"]
        self.assertEqual(value["wrapped_scope"]["refreshes"], 1)
        uses = value["semantic_wrapped_scope"]["members"][0]["uses"]
        self.assertEqual(len(uses), 2)
        self.assertFalse(uses[0]["failed"])
        self.assertTrue(uses[1]["failed"])
        self.assertEqual((value["empty_view"]["copies"], value["empty_view"]["refreshes"]), (0, 0))


if __name__ == "__main__":
    unittest.main()
