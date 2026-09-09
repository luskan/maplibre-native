#!/usr/bin/env python3
"""Cancellation eligibility and contention exercise the real collector locks."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PartialCorrectionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory(prefix="partial-correction-") as directory:
            binary = str(Path(directory) / "trace")
            subprocess.run(["g++", "-std=c++17", "-pthread", "-DTILE_TRACE_TESTING", "-I", str(ROOT / "include"),
                            str(ROOT / "test/automapa/partial_correction_harness.cpp"), "-o", binary], check=True)
            lines = subprocess.check_output([binary], text=True).splitlines()
        cls.samples = {lines[i]: json.loads(lines[i + 1]) for i in range(0, len(lines), 2)}

    def test_cancelled_extraction_is_excluded_in_both_event_orders(self):
        for name in ("cancel_before_left", "cancel_after_left"):
            value = self.samples[name]
            self.assertEqual((value["total"], value["extracted"], value["mlEligible"], value["mlExcluded"]), (4, 3, 3, 1))
            self.assertEqual((value["mlStatus"], value["status"], value["visible"], value["screenExcluded"]), ("complete", "complete", 2, 1))
            self.assertEqual(value["displayUs"], 700000)

    def test_no_eligible_tiles_is_cancelled_without_zero_duration(self):
        for name in ("all_cancelled", "late_no_receiver"):
            value = self.samples[name]
            self.assertEqual((value["mlStatus"], value["status"]), ("cancelled", "cancelled"))
            self.assertIsNone(value["displayUs"])
            self.assertIsNone(value["mlUs"])

    def test_endpoints_before_retirement_remain_historical(self):
        for name in ("cancel_after_cache", "cancel_after_swap", "earlier_cache_late_callback"):
            self.assertEqual(self.samples[name]["mlUs"], 300000)
            self.assertEqual(self.samples[name]["mlExcluded"], 0)
        self.assertEqual(self.samples["cancel_after_swap"]["displayUs"], 700000)
        self.assertEqual(self.samples["draw_before_cancel_swap_after"]["displayUs"], 700000)

    def test_real_errors_are_not_hidden_by_cleanup(self):
        for name in ("unknown_no_receiver", "error_before_cancel", "marked_error_before_cancel", "error_before_cancel_same_clock"):
            self.assertEqual(self.samples[name]["mlStatus"], "partial")
            self.assertEqual(self.samples[name]["mlExcluded"], 0)
        self.assertEqual(self.samples["layout_error_before_cancel"]["status"], "partial")
        self.assertEqual(self.samples["layout_error_after_cancel_same_clock"]["status"], "cancelled")

    def test_optional_and_unrelated_loss_preserve_valid_timings(self):
        for name in ("harmless_finish_contention", "optional_collector_contention", "another_publication_loss",
                     "loss_after_endpoints", "old_writer_after_reset", "reset_after_capacity", "polling_with_real_endpoints"):
            with self.subTest(name=name):
                self.assertEqual((self.samples[name]["mlUs"], self.samples[name]["displayUs"]), (300000, 700000))
                self.assertEqual(self.samples[name]["status"], "complete")
        self.assertGreater(self.samples["polling_counts"]["reads"], 0)
        self.assertTrue(self.samples["no_recopy_completed"]["same"])

    def test_critical_loss_is_scoped_and_not_erased_by_retirement(self):
        value = self.samples["lost_cache_not_excluded"]
        self.assertEqual((value["mlStatus"], value["mlExcluded"], value["mlReason"]), ("partial", 0, "capture"))
        self.assertEqual(self.samples["lost_source_teardown"]["mlStatus"], "partial")
        value = self.samples["simultaneous_loss_categories"]
        self.assertEqual((value["status"], value["mlStatus"]), ("partial", "partial"))
        self.assertEqual((value["screenReason"], value["mlReason"]), ("capture", "capture"))

    def test_view_loss_and_pre_admission_demand_loss_are_not_hidden(self):
        value = self.samples["lost_demand_before_admission"]
        self.assertEqual((value["mlStatus"], value["status"], value["screenReason"]), ("complete", "partial", "capture"))
        self.assertEqual(self.samples["view_loss_before_stamp"]["status"], "partial")
        self.assertEqual(self.samples["view_loss_before_admission_and_stamp"]["status"], "partial")
        self.assertEqual(self.samples["view_loss_after_draw"]["displayUs"], 700000)

    def test_incomplete_loss_publication_defers_snapshot_and_capacity_is_explicit(self):
        self.assertTrue(self.samples["unfinished_loss"]["deferred"])
        self.assertEqual(self.samples["published_loss"]["mlStatus"], "partial")
        self.assertEqual(self.samples["loss_capacity"]["mlStatus"], "partial")
        self.assertGreater(self.samples["loss_capacity"]["captureLost"], 8192)


if __name__ == "__main__":
    unittest.main()
