#!/usr/bin/env python3
"""Batch latency uses original membership and the actual first successful swap."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class BatchTraceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory(prefix="batch-trace-") as directory:
            executable = str(Path(directory) / "trace")
            subprocess.run([
                "g++", "-std=c++17", "-pthread", "-DTILE_TRACE_TESTING", "-I", str(ROOT / "include"),
                str(ROOT / "src/mln/util/tile_trace.cpp"),
                str(ROOT / "test/automapa/batch_trace_harness.cpp"), "-o", executable,
            ], check=True)
            lines = subprocess.check_output([executable], text=True).splitlines()
        cls.samples = {lines[i]: json.loads(lines[i + 1]) for i in range(0, len(lines), 2)}

    def test_complete_uses_shared_start_and_last_first_swap(self):
        value = self.samples["complete"]
        self.assertEqual(value["batchMs"], 500)
        self.assertEqual(value["displayUs"], 2600000)
        self.assertEqual(value["status"], "complete")
        self.assertEqual(value["drawn"], value["total"])

    def test_failed_or_unrelated_swaps_do_not_complete(self):
        for name in ("waiting", "failed_swap", "unrelated"):
            self.assertEqual(self.samples[name]["status"], "waiting")
            self.assertEqual(self.samples[name]["drawn"], 1)
            self.assertIsNone(self.samples[name]["displayUs"])

    def test_completed_timing_does_not_drift_after_later_loss(self):
        self.assertEqual(self.samples["later_loss"]["displayUs"], 2600000)
        self.assertEqual(self.samples["later_loss"]["status"], "complete")

    def test_swap_can_precede_producer_close(self):
        value = self.samples["swap_before_close"]
        self.assertEqual(value["batchMs"], 500)
        self.assertEqual(value["displayUs"], 100000)

    def test_empty_data_is_explicit_and_never_a_draw(self):
        self.assertEqual(self.samples["empty"]["status"], "no-draw")
        self.assertIsNone(self.samples["empty"]["displayUs"])
        self.assertEqual(self.samples["empty"]["drawn"], 0)
        self.assertEqual(self.samples["mixed_empty"]["status"], "complete")
        self.assertEqual(self.samples["mixed_empty"]["noDraw"], 1)
        self.assertEqual(self.samples["mixed_empty"]["drawn"], 1)

    def test_partial_never_shrinks_the_original_denominator(self):
        for name in ("drop", "missing_admission"):
            value = self.samples[name]
            self.assertEqual(value["status"], "partial")
            self.assertEqual(value["total"], 2)
            self.assertEqual(value["drawn"], 1)
            self.assertIsNone(value["displayUs"])

    def test_downstream_error_can_recover_through_another_receiver(self):
        self.assertEqual(self.samples["layout_error"]["status"], "partial")
        self.assertEqual(self.samples["shared_recovery"]["status"], "complete")

    def test_loss_pause_teardown_and_reset_are_not_exact_completions(self):
        for name in ("pause", "paused_admission", "session_reset"):
            self.assertEqual(self.samples[name]["status"], "partial")
            self.assertIsNone(self.samples[name]["displayUs"])

    def test_optional_loss_and_receiver_cancel_do_not_fail_shared_work(self):
        self.assertEqual(self.samples["loss"]["status"], "complete")
        self.assertEqual(self.samples["evicted_receiver"]["status"], "waiting")

    def test_source_teardown_invalidates_the_view(self):
        self.assertEqual(self.samples["teardown"]["status"], "cancelled")

    def test_receiver_teardown_survives_record_eviction_and_can_recover(self):
        for name in ("teardown_before_binding", "demand_teardown"):
            self.assertEqual(self.samples[name]["status"], "partial")
        self.assertEqual(self.samples["demand_recovery"]["status"], "complete")

    def test_receiver_capacity_cannot_report_exact_completion(self):
        self.assertEqual(self.samples["receiver_capacity"]["drawn"], 1)
        self.assertEqual(self.samples["receiver_capacity"]["status"], "partial")
        self.assertIsNone(self.samples["receiver_capacity"]["displayUs"])

    def test_membership_is_bounded_and_overflow_is_visible(self):
        value = self.samples["capacity"]
        self.assertEqual(value["total"], 257)
        self.assertEqual(len(value["members"]), 256)
        self.assertEqual(value["status"], "partial")

    def test_coherent_metadata_and_live_in_flight(self):
        old = self.samples["old_batch"]
        new = self.samples["next_batch"]
        self.assertNotEqual(old["id"], new["id"])
        self.assertEqual(old["status"], "complete")
        self.assertEqual(new["status"], "waiting")
        self.assertEqual(new["batchMs"], 100)
        self.assertEqual(new["inFlight"], 3)
        self.assertEqual(self.samples["next_complete"]["displayUs"], 300000)


if __name__ == "__main__":
    unittest.main()
