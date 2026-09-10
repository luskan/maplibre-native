#!/usr/bin/env python3
"""Frame reuse and capture transitions preserve independent batch evidence."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CaptureLifecycleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory(prefix="capture-lifecycle-") as directory:
            binary = str(Path(directory) / "capture")
            subprocess.run([
                "g++", "-std=c++17", "-pthread", "-DTILE_TRACE_TESTING", "-I", str(ROOT / "include"),
                str(ROOT / "test/automapa/capture_lifecycle_harness.cpp"), "-o", binary,
            ], check=True)
            lines = subprocess.check_output([binary], text=True, timeout=60).splitlines()
        cls.samples = {lines[i]: json.loads(lines[i + 1]) for i in range(0, len(lines), 2)}

    def test_empty_and_smaller_frames_do_not_submit_previous_draws(self):
        for name, drawn in [("empty_after_full_failed", 0), ("smaller_after_full", 1),
                            ("empty_after_abandoned", 1)]:
            with self.subTest(name=name):
                value = self.samples[name]
                self.assertEqual((value["status"], value["drawn"]), ("waiting", drawn))
                self.assertIsNone(value["displayUs"])
        value = self.samples["frame_recovered"]
        self.assertEqual((value["status"], value["drawn"], value["displayUs"]), ("complete", 2, 5000))

    def test_frame_slots_are_reused_with_fresh_metadata(self):
        self.assertTrue(self.samples["frame_storage"]["reused"])

    def test_completed_endpoints_survive_long_pause(self):
        before, after = (self.samples[name] for name in ("completed_before_pause", "completed_after_pause"))
        for field in ("status", "mlStatus", "displayUs", "mlUs", "drawn", "screenExcluded", "mlExcluded"):
            self.assertEqual(before[field], after[field], field)
        self.assertEqual(after["captureLost"], 0)

    def test_fresh_batches_are_exact_after_pause(self):
        for name in ("fresh_after_long_pause", "fresh_without_paused_frame"):
            with self.subTest(name=name):
                value = self.samples[name]
                self.assertEqual((value["status"], value["mlStatus"]), ("complete", "complete"))
                self.assertEqual(value["captureLost"], 0)

    def test_interrupted_work_does_not_become_a_departure_or_clean_result(self):
        for name in ("interrupted_same_view", "interrupted_after_fresh", "pause_inside_frame"):
            with self.subTest(name=name):
                value = self.samples[name]
                self.assertEqual(value["status"], "partial")
                self.assertEqual(value["left"], 0)
                self.assertIsNone(value["displayUs"])
                self.assertEqual(value["mlStatus"], "complete")

    def test_delayed_admission_cannot_borrow_pre_pause_view(self):
        for name in ("delayed_current_admission", "delayed_historical_admission"):
            with self.subTest(name=name):
                value = self.samples[name]
                self.assertEqual(value["status"], "partial")
                self.assertFalse(value["members"][0]["viewKnown"])
                self.assertIsNone(value["displayUs"])

    def test_stale_no_draw_acknowledgement_is_rejected(self):
        value = self.samples["stale_no_draw_acknowledgement"]
        self.assertEqual((value["status"], value["noDraw"]), ("partial", 0))

    def test_view_preparation_cannot_cross_capture_or_session_boundary(self):
        for name in ("toggle_during_view_preparation", "reset_during_view_preparation"):
            with self.subTest(name=name):
                value = self.samples[name]
                self.assertEqual((value["status"], value["mlStatus"]), ("complete", "complete"))
                self.assertEqual(value["captureLost"], 0)
        self.assertTrue(self.samples["stale_prepared_keys"]["rejected"])

    def test_old_loss_writer_cannot_poison_fresh_capture(self):
        value = self.samples["old_loss_after_resume"]
        self.assertEqual((value["status"], value["mlStatus"]), ("complete", "complete"))
        self.assertEqual(value["captureLost"], 1)
        old = self.samples["interrupted_with_old_loss"]
        self.assertEqual(old["status"], "partial")
        self.assertIsNone(old["displayUs"])

    def test_draw_cannot_attach_visibility_from_after_pause(self):
        old = self.samples["draw_crossed_capture_boundary"]
        self.assertEqual((old["status"], old["drawn"]), ("partial", 0))
        fresh = self.samples["fresh_after_interrupted_draw"]
        self.assertEqual((fresh["status"], fresh["mlStatus"]), ("complete", "complete"))

    def test_old_loss_writer_cannot_hide_new_loss(self):
        value = self.samples["new_loss_survives_old_writer"]
        self.assertEqual((value["status"], value["mlStatus"]), ("partial", "complete"))
        self.assertEqual(value["captureLost"], 2)
        self.assertIsNone(value["displayUs"])


if __name__ == "__main__":
    unittest.main()
