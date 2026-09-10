#!/usr/bin/env python3
"""Exercise delayed evidence with deterministic and production clocks."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class EvidenceBoundaryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runs = []
        with tempfile.TemporaryDirectory(prefix="evidence-boundaries-") as directory:
            for clock in ("deterministic", "production"):
                binary = str(Path(directory) / clock)
                flags = ["-DTILE_TRACE_TESTING"] if clock == "deterministic" else []
                subprocess.run(["g++", "-std=c++17", "-pthread", *flags, "-I", str(ROOT / "include"),
                                str(ROOT / "test/automapa/evidence_boundary_harness.cpp"), "-o", binary], check=True)
                lines = subprocess.check_output([binary], text=True, timeout=30).splitlines()
                cls.runs.append((clock, {lines[i]: json.loads(lines[i + 1]) for i in range(0, len(lines), 2)}))

    def test_unknown_membership_survives_every_retirement_order(self):
        for clock, samples in self.runs:
            for name in ("unknown_before_swap", "unknown_retired_before_draw", "unknown_retired_before_swap",
                         "unknown_retired_after_swap", "all_unknown_retired", "overflow_uses_retired"):
                with self.subTest(clock=clock, name=name):
                    value = samples[name]
                    self.assertEqual((value["status"], value["screenReason"]), ("partial", "membership"))
                    self.assertIsNone(value["displayUs"])
            self.assertEqual(samples["overflow_uses_retired"]["screenExcluded"], 8)
            self.assertEqual(samples["all_unknown_retired"]["screenExcluded"], 0)

    def test_cache_boundary_is_not_hidden_by_retirement(self):
        for clock, samples in self.runs:
            for name in ("delayed_cache_lost", "zero_cache_lost", "zero_cache_recorded", "overflow_cache"):
                with self.subTest(clock=clock, name=name):
                    value = samples[name]
                    self.assertEqual((value["mlStatus"], value["mlReason"], value["mlExcluded"]), ("partial", "capture", 0))
                    self.assertIsNone(value["mlUs"])
            self.assertEqual(samples["delayed_cache_recorded"]["mlStatus"], "complete")
            self.assertEqual(samples["delayed_cache_recorded"]["mlExcluded"], 0)

    def test_draw_boundary_prevents_exclusion_after_lost_swap(self):
        for clock, samples in self.runs:
            for name in ("delayed_swap_lost", "overflow_swap", "delayed_no_draw_lost"):
                with self.subTest(clock=clock, name=name):
                    value = samples[name]
                    self.assertEqual((value["status"], value["screenReason"], value["screenExcluded"]), ("partial", "capture", 0))
                    self.assertIsNone(value["displayUs"])

    def test_departure_does_not_hide_pre_departure_draw(self):
        for clock, samples in self.runs:
            for name in ("departure_before", "departure_equal", "departure_before_reentry", "departure_equal_reentry"):
                with self.subTest(clock=clock, name=name):
                    value = samples[name]
                    self.assertEqual((value["status"], value["screenReason"]), ("partial", "capture"))
                    self.assertIsNone(value["displayUs"])
            for name in ("departure_after", "departure_after_reentry"):
                self.assertEqual(samples[name]["status"], "partial-left")
                self.assertIsNotNone(samples[name]["displayUs"])

    def test_later_losses_preserve_valid_results(self):
        for clock, samples in self.runs:
            for name in ("loss_after_retirement", "loss_after_completion", "error_after_recorded_draw",
                         "overlapping_frame_loss_after_completion", "layout_error_before_recorded_draw"):
                with self.subTest(clock=clock, name=name):
                    self.assertEqual(samples[name]["status"], "complete")
                    self.assertIsNotNone(samples[name]["displayUs"])

    def test_overflow_publication_is_atomic_to_readers(self):
        for clock, samples in self.runs:
            for point in ("loss_reserved", "loss_overflow_event"):
                for writers in (1, 2):
                    name = f"{point}_{writers}"
                    with self.subTest(clock=clock, name=name):
                        self.assertTrue(samples[name]["deferred"])
                        value = samples[name + "_published"]
                        self.assertEqual((value["mlStatus"], value["mlReason"], value["mlExcluded"]), ("partial", "capture", 0))
                        self.assertEqual(value["captureLost"], 8192 + writers)
            self.assertTrue(samples["writer_between_reads"]["deferred"])
            self.assertEqual(samples["reset_with_old_overflow_writer"]["status"], "complete")
            self.assertEqual(samples["reset_with_old_overflow_writer"]["captureLost"], 0)

    def test_delayed_successful_error_write_keeps_original_boundary(self):
        for clock, samples in self.runs:
            with self.subTest(clock=clock):
                value = samples["delayed_error_success"]
                self.assertEqual((value["mlStatus"], value["mlReason"], value["mlExcluded"]), ("partial", "error", 0))

    def test_late_reported_errors_revoke_only_endpoints_they_precede(self):
        for clock, samples in self.runs:
            for name in ("error_before_recorded_draw", "earliest_error_wins", "earlier_no_receiver_same_clock"):
                with self.subTest(clock=clock, name=name):
                    value = samples[name]
                    self.assertEqual(value["status"], "partial")
                    self.assertIsNone(value["displayUs"])
                    if name != "layout_error_before_recorded_draw":
                        self.assertEqual(value["mlStatus"], "partial")
                        self.assertEqual(value["mlExcluded"], 0)


if __name__ == "__main__":
    unittest.main()
