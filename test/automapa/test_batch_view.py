#!/usr/bin/env python3
"""Independent cache and visible-batch endpoints, including adversarial event order."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class BatchViewTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory(prefix="batch-view-") as directory:
            executable = str(Path(directory) / "trace")
            subprocess.run([
                "g++", "-std=c++17", "-pthread", "-DTILE_TRACE_TESTING", "-I", str(ROOT / "include"),
                str(ROOT / "src/mln/util/tile_trace.cpp"),
                str(ROOT / "test/automapa/batch_view_harness.cpp"), "-o", executable,
            ], check=True)
            lines = subprocess.check_output([executable], text=True, timeout=30).splitlines()
        cls.samples = {lines[i]: json.loads(lines[i + 1]) for i in range(0, len(lines), 2)}

    def test_stationary_prefetch_never_drawn_does_not_block_screen(self):
        value = self.samples["prefetch_never_drawn"]
        self.assertEqual((value["total"], value["visible"], value["drawn"]), (3, 2, 2))
        self.assertEqual((value["mlStatus"], value["status"]), ("complete", "complete"))
        self.assertEqual((value["mlUs"], value["displayUs"]), (600000, 900000))
        self.assertGreater(value["ageUs"], 80000000)

    def test_required_tile_stays_pending_even_after_production_and_cache_finish(self):
        value = self.samples["visible_pending"]
        self.assertEqual((value["status"], value["drawn"], value["visible"]), ("waiting", 1, 2))
        self.assertEqual(value["inFlight"], 0)
        self.assertEqual(value["mlStatus"], "complete")
        self.assertIsNone(value["displayUs"])

    def test_late_prefetch_cache_does_not_delay_visible_measurement(self):
        value = self.samples["prefetch_cache_pending"]
        self.assertEqual((value["status"], value["mlStatus"]), ("complete", "waiting"))
        later = self.samples["prefetch_cache_late"]
        self.assertEqual(later["displayUs"], value["displayUs"])
        self.assertGreater(later["mlUs"], later["displayUs"])

    def test_offscreen_drop_only_makes_cache_measurement_incomplete(self):
        value = self.samples["offscreen_drop"]
        self.assertEqual((value["status"], value["mlStatus"]), ("complete", "partial"))
        self.assertEqual((value["total"], value["visible"]), (2, 1))

    def test_no_visible_members_has_no_fabricated_screen_time(self):
        for name in ("all_offscreen", "disabled_view"):
            with self.subTest(name=name):
                value = self.samples[name]
                self.assertEqual(value["status"], "offscreen")
                self.assertEqual(value["visible"], 0)
                self.assertIsNone(value["displayUs"])
                self.assertEqual(value["mlStatus"], "complete")

    def test_cache_disabled_and_mixed_bypass_do_not_wait_for_insertion(self):
        for name in ("cache_off", "mixed_cache_off"):
            with self.subTest(name=name):
                value = self.samples[name]
                self.assertEqual(value["mlStatus"], "cache-off")
                self.assertIsNone(value["mlUs"])
                self.assertEqual(value["mlBypassed"], 1)
        self.assertEqual(self.samples["cache_off"]["status"], "complete")

    def test_conversion_does_not_stand_in_for_cache_insertion(self):
        self.assertEqual(self.samples["conversion_is_not_cache"]["mlStatus"], "waiting")
        self.assertEqual(self.samples["cache_after_conversion"]["mlUs"], 750000)

    def test_cache_arrival_survives_receiver_cancel_and_source_retirement(self):
        for name in ("cancel_after_cache", "retire_after_cache"):
            with self.subTest(name=name):
                self.assertEqual(self.samples[name]["mlStatus"], "complete")
                self.assertEqual(self.samples[name]["mlUs"], 750000)
                self.assertIsNone(self.samples[name]["displayUs"])

    def test_stored_error_or_no_receiver_is_not_clean_completion(self):
        for name in ("error_payload", "no_receiver"):
            with self.subTest(name=name):
                self.assertEqual(self.samples[name]["mlStatus"], "partial")
                self.assertEqual(self.samples[name]["status"], "partial")
                self.assertIsNone(self.samples[name]["mlUs"])
                self.assertIsNone(self.samples[name]["displayUs"])

    def test_accepted_filtered_content_is_resolved_without_a_draw(self):
        mixed = self.samples["filtered_and_drawn"]
        self.assertEqual((mixed["drawn"], mixed["noDraw"], mixed["visible"]), (1, 1, 2))
        self.assertEqual(mixed["displayUs"], 900000)
        empty = self.samples["filtered_only"]
        self.assertEqual((empty["status"], empty["drawn"], empty["noDraw"]), ("no-draw", 0, 1))
        self.assertIsNone(empty["displayUs"])

    def test_accepted_drawable_layout_and_failed_swap_do_not_complete(self):
        for name in ("drawable_but_pending", "failed_swap"):
            self.assertEqual(self.samples[name]["status"], "waiting")
            self.assertIsNone(self.samples[name]["displayUs"])
        self.assertEqual(self.samples["swap_recovered"]["displayUs"], 1000000)

    def test_publication_source_map_and_required_key_are_all_checked(self):
        for name in ("replacement_not_original", "fallback_not_ideal", "wrong_map", "wrong_source"):
            with self.subTest(name=name):
                self.assertEqual(self.samples[name]["status"], "waiting")
                self.assertEqual(self.samples[name]["drawn"], 0)
        self.assertEqual(self.samples["correct_generation"]["displayUs"], 1400000)

    def test_order_of_same_required_tiles_does_not_restart_view(self):
        self.assertTrue(self.samples["same_view_id"]["same"])
        self.assertEqual(self.samples["same_content"]["status"], "complete")
        self.assertEqual(self.samples["same_content"]["displayUs"], 900000)

    def test_completed_times_survive_cover_style_content_and_hidden_source_changes(self):
        for name in ("cover_changed", "cover_returned", "style_changed", "content_invalidated", "source_hidden"):
            with self.subTest(name=name):
                self.assertEqual(self.samples[name]["status"], "complete")
                self.assertEqual(self.samples[name]["displayUs"], 900000)
                self.assertEqual(self.samples[name]["mlStatus"], "complete")
        self.assertEqual(self.samples["new_batch_after_invalidation"]["status"], "complete")

    def test_each_required_wrap_needs_its_own_draw(self):
        self.assertEqual(self.samples["second_wrap_pending"]["status"], "waiting")
        done = self.samples["actual_drawable_wrap"]
        self.assertEqual(done["displayUs"], 1100000)
        self.assertEqual(len(done["members"][0]["uses"]), 2)

    def test_two_wraps_in_one_frame_are_not_deduplicated_together(self):
        value = self.samples["same_frame_two_wraps"]
        self.assertEqual(value["status"], "complete")
        self.assertEqual(value["displayUs"], 900000)
        uses = value["members"][0]["uses"]
        self.assertEqual(uses[0]["frame"], uses[1]["frame"])
        self.assertNotEqual(uses[0]["wrap"], uses[1]["wrap"])

    def test_empty_at_one_overscaled_zoom_does_not_resolve_another(self):
        self.assertEqual(self.samples["other_zoom_not_empty"]["status"], "waiting")
        value = self.samples["mixed_overscaled_uses"]
        self.assertEqual(value["status"], "complete")
        self.assertEqual(value["displayUs"], 1100000)
        uses = value["members"][0]["uses"]
        self.assertEqual([use["noDraw"] for use in uses], [1, 0])

    def test_lost_view_update_never_turns_missing_visibility_into_offscreen(self):
        for name in ("lost_view_before_admission", "view_overflow"):
            with self.subTest(name=name):
                self.assertEqual(self.samples[name]["status"], "partial")
                self.assertIsNone(self.samples[name]["displayUs"])
        self.assertEqual(self.samples["stale_demand_view"]["status"], "partial")
        for name in ("lost_view_after_completion", "view_eviction"):
            self.assertEqual(self.samples[name]["status"], "complete")
            self.assertEqual(self.samples[name]["displayUs"], 900000)

    def test_required_use_overflow_is_bounded_and_incomplete(self):
        value = self.samples["use_overflow"]
        self.assertEqual(value["status"], "partial")
        self.assertEqual(len(value["members"][0]["uses"]), 8)
        self.assertIsNone(value["displayUs"])

    def test_two_maps_with_same_tile_coordinates_complete_independently(self):
        self.assertEqual(self.samples["other_map_pending"]["status"], "waiting")
        value = self.samples["two_maps_complete"]
        self.assertEqual((value["visible"], value["drawn"]), (2, 2))
        self.assertEqual(value["displayUs"], 1200000)

    def test_capture_loss_and_reset_cannot_produce_exact_values(self):
        for name in ("pause_after_cache", "reset_session"):
            with self.subTest(name=name):
                self.assertEqual(self.samples[name]["status"], "partial")
                self.assertIsNone(self.samples[name]["displayUs"])
        self.assertEqual(self.samples["pause_after_cache"]["mlStatus"], "complete")
        self.assertEqual(self.samples["loss_before_endpoints"]["status"], "complete")
        self.assertEqual(self.samples["closed_after_loss"]["displayUs"], 900000)

    def test_worker_cache_reuse_has_fresh_arrival_without_extraction(self):
        value = self.samples["worker_cache_reuse"]
        self.assertEqual((value["mlUs"], value["displayUs"]), (50000, 100000))

    def test_invalidation_is_scoped_to_required_canonical_tiles(self):
        for name in ("offscreen_invalidation", "distant_region"):
            self.assertEqual(self.samples[name]["status"], "complete")
        for name in ("intersecting_region", "wrapped_region"):
            self.assertEqual(self.samples[name]["status"], "complete")

    def test_concurrent_views_and_snapshots_do_not_stop_render_work(self):
        self.assertEqual(self.samples["concurrent_work"], {"work": 500, "reads": 500})
        value = self.samples["concurrent_final"]
        self.assertIn(value["status"], ("partial", "complete"))
        if value["status"] == "complete":
            self.assertEqual(value["displayUs"], 900000)
        else:
            self.assertIsNone(value["displayUs"])
        self.assertEqual(value["total"], 1)

    def test_coalesced_delivery_is_partial_until_original_publication_draws(self):
        for name in ("coalesced_delivery", "replacement_cannot_resolve_delivery"):
            self.assertEqual(self.samples[name]["status"], "partial")
            self.assertEqual(self.samples[name]["mlStatus"], "complete")
            self.assertIsNone(self.samples[name]["displayUs"])
        value = self.samples["superseded_delivery_recovery"]
        self.assertEqual((value["status"], value["mlStatus"]), ("complete", "complete"))
        self.assertEqual(value["displayUs"], 900000)

    def test_all_60_valid_cache_failed_swap_and_draw_orderings(self):
        for i in range(60):
            with self.subTest(ordering=i):
                value = self.samples[f"ordering_{i}"]
                expected = self.samples[f"ordering_{i}_expected"]
                self.assertEqual(value["status"], "complete")
                self.assertEqual(value["mlStatus"], "complete")
                self.assertEqual(value["mlUs"], expected["mlUs"])
                self.assertEqual(value["displayUs"], expected["displayUs"])
                self.assertEqual((value["total"], value["visible"]), (3, 2))


if __name__ == "__main__":
    unittest.main()
