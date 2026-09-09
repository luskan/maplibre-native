#!/usr/bin/env python3
"""Standalone collector invariants, including a controlled 250 ms publication tail."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class TileTraceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory(prefix="tile-trace-") as directory:
            executable = str(Path(directory) / "trace")
            subprocess.run([
                "g++", "-std=c++17", "-pthread", "-DTILE_TRACE_TESTING",
                "-I", str(ROOT / "include"),
                str(ROOT / "src/mln/util/tile_trace.cpp"),
                str(ROOT / "test/automapa/tile_trace_harness.cpp"), "-o", executable,
            ], check=True)
            lines = subprocess.check_output([executable], text=True).splitlines()
        cls.samples = {lines[i]: json.loads(lines[i + 1]) for i in range(0, len(lines), 2)}

    def demands(self, name):
        return [r for r in self.samples[name]["records"] if r["kind"] == 0]

    def test_failed_and_unrelated_swap_do_not_complete(self):
        for name in ("failed", "unrelated_swap"):
            self.assertEqual(self.demands(name)[0]["outcome"], "pending")
            self.assertEqual(self.demands(name)[0]["timesUs"][13], "0")

    def test_success_and_controlled_interval(self):
        row = self.demands("submitted")[0]
        self.assertEqual(row["outcome"], "submitted")
        times = list(map(int, row["timesUs"]))
        self.assertEqual(times[4] - times[3], 250000)
        self.assertEqual(times[13], 340000)
        self.assertEqual(times[12], 330000)
        self.assertEqual(row["elapsedUs"][1], 338000)
        self.assertEqual(row["readyUs"], 258000)
        self.assertEqual(row["firstDrawUs"], 268000)
        self.assertEqual(row["partitionUs"], [250000, 1000, 7000, 10000, 60000, 10000])
        self.assertEqual(sum(row["partitionUs"]) + row["unknownUs"], row["elapsedUs"][1])

    def test_cache_reuse_has_no_fresh_extraction(self):
        rows = self.demands("reuse")
        cached = next(r for r in rows if r["origin"] == "renderer")
        self.assertEqual(cached["outcome"], "submitted")
        self.assertEqual(cached["timesUs"][3], "0")
        self.assertIsNone(cached["elapsedUs"][1])
        self.assertEqual(cached["elapsedUs"][0], 30000)
        fresh = next(r for r in rows if r["origin"] == "fresh")
        self.assertNotEqual(cached["surface"], fresh["surface"])
        self.assertEqual(cached["generation"], fresh["generation"])

    def test_old_generation_does_not_complete_new_demand(self):
        self.assertEqual(sum(r["outcome"] == "pending" for r in self.demands("wrong_generation")), 1)
        self.assertEqual(sum(r["outcome"] == "cancelled" for r in self.demands("cancelled")), 1)

    def test_pause_before_first_use_marks_observed_time_incomplete(self):
        row = next(r for r in self.demands("pause_gap") if r["consumer"] == "16")
        self.assertEqual(row["outcome"], "submitted")
        self.assertFalse(row["firstUseComplete"])
        self.assertEqual(self.samples["pause_gap"]["captureGaps"], 1)

    def test_shared_publication_does_not_finish_another_receiver(self):
        rows = self.samples["fanout"]["records"]
        demand_a = next(r for r in rows if r["kind"] == 0 and r["consumer"] == "14")
        demand_b = next(r for r in rows if r["kind"] == 0 and r["consumer"] == "15")
        delivery_a = next(r for r in rows if r["kind"] == 1 and r["consumer"] == "14" and r["id"] != r["publication"])
        producer = next(r for r in rows if r["id"] == demand_b["publication"])
        self.assertEqual(demand_a["outcome"], "pending")
        self.assertEqual(delivery_a["outcome"], "pending")
        self.assertEqual(demand_b["outcome"], "submitted")
        self.assertEqual(producer["outcome"], "submitted")
        self.assertEqual(demand_b["wrap"], 1)
        self.assertEqual(demand_b["overscaledZ"], 12)
        self.assertTrue(demand_b["firstUseComplete"])

    def test_loss_does_not_claim_an_exact_first_submission(self):
        row = next(r for r in self.demands("loss_before_submit") if r["consumer"] == "13")
        self.assertEqual(row["outcome"], "submitted")
        self.assertFalse(row["firstUseComplete"])
        self.assertTrue(self.demands("submitted")[0]["firstUseComplete"])

    def test_untracked_retained_content_is_unknown_not_stalled(self):
        row = next(r for r in self.demands("untracked_reuse") if r["consumer"] == "11")
        self.assertEqual(row["outcome"], "truncated")
        self.assertIsNone(row["elapsedUs"][0])

    def test_skipped_swap_does_not_complete(self):
        row = next(r for r in self.demands("skipped_swap") if r["consumer"] == "12")
        self.assertEqual(row["outcome"], "pending")
        self.assertEqual(row["timesUs"][13], "0")

    def test_missing_stage_is_explicit_unknown(self):
        row = next(r for r in self.demands("missing_stage") if r["publication"] == "302")
        self.assertEqual(row["partitionUs"], [None, None, 10000, 10000, 10000, 10000])
        self.assertEqual(row["unknownUs"], 10000)
        self.assertEqual(sum(v for v in row["partitionUs"] if v is not None) + row["unknownUs"], row["elapsedUs"][1])

    def test_empty_is_not_terminal_before_accepted_layout(self):
        before = next(r for r in self.demands("empty_not_accepted") if r["consumer"] == "9")
        after = next(r for r in self.demands("empty_superseded") if r["consumer"] == "9")
        self.assertEqual(before["outcome"], "pending")
        self.assertEqual(after["outcome"], "submitted")
        self.assertEqual(after["publication"], "301")

    def test_lifecycle_keeps_completed_provenance(self):
        before = self.demands("submitted")[0]
        after = self.demands("lifecycle_after_submit")[0]
        for key in ("publication", "generation", "origin", "outcome", "timesUs"):
            self.assertEqual(before[key], after[key])

    def test_distinct_geometry_and_symbol_submission(self):
        row = self.demands("symbols")[0]
        self.assertEqual(row["geometrySubmittedUs"], "340000")
        self.assertEqual(row["symbolSubmittedUs"], "370000")
        self.assertEqual(row["timesUs"][13], "340000")

    def test_redraw_does_not_rewrite_closed_startup_record(self):
        before = {r["id"]: r for r in self.samples["submitted"]["records"]}
        after = {r["id"]: r for r in self.samples["redraw_after_eviction"]["records"]}
        for identifier, row in before.items():
            if row["outcome"] == "submitted":
                self.assertEqual(after[identifier]["timesUs"][13], row["timesUs"][13])
                self.assertEqual(after[identifier]["submittedDrawUs"], row["submittedDrawUs"])

    def test_reset_allows_explicit_retained_reuse(self):
        row = self.demands("reset_reuse")[0]
        self.assertEqual(row["outcome"], "submitted")
        self.assertEqual(row["elapsedUs"][0], 30000)
        self.assertIsNone(row["elapsedUs"][1])

    def test_reset_mixed_buckets_keep_first_generation(self):
        before = self.demands("reset_reuse")[0]
        after = self.demands("reset_reuse_symbols")[0]
        self.assertEqual(before["generation"], after["generation"])
        self.assertEqual(before["timesUs"][13], after["timesUs"][13])
        self.assertNotEqual(after["geometryGeneration"], after["symbolGeneration"])
        self.assertEqual(after["symbolSubmittedUs"], "593000")

    def test_no_reentrant_lock_loss(self):
        self.assertEqual(self.samples["submitted"]["lost"], 0)

    def test_reset_fences_old_events(self):
        self.assertEqual(self.samples["reset"]["records"], [])

    def test_empty_drop_and_teardown(self):
        self.assertEqual(self.demands("empty")[0]["outcome"], "ready-empty")
        self.assertTrue(any(r["outcome"] == "overflow" for r in self.samples["drop"]["records"]))
        self.assertTrue(any(r["outcome"] == "pending" for r in self.demands("drop")))
        self.assertFalse(any(r["outcome"] == "pending" for r in self.demands("teardown")))

    def test_bounded_storage_retains_startup_and_reports_loss(self):
        for name in ("bounded", "concurrent"):
            sample = self.samples[name]
            self.assertLessEqual(len(sample["records"]), 1088)
            self.assertGreater(sample["evicted"], 0)
            self.assertTrue(any(r["early"] for r in sample["records"]))


if __name__ == "__main__":
    unittest.main()
