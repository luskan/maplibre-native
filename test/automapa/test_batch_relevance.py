#!/usr/bin/env python3
"""Batch timing survives overlap and keeps explicit per-use departure history."""
import unittest
import test_batch_view as fixture


class BatchRelevanceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fixture.BatchViewTest.setUpClass()
        cls.samples = fixture.BatchViewTest.samples

    def test_overlap_keeps_pending_survivors_and_original_denominator(self):
        value = self.samples["overlap_pending"]
        self.assertEqual((value["status"], value["drawn"], value["left"], value["visible"]),
                         ("waiting", 1, 1, 3))
        self.assertIsNone(value["displayUs"])
        self.assertEqual(value["mlStatus"], "complete")

    def test_overlap_completes_remaining_uses_from_original_batch_start(self):
        value = self.samples["overlap_complete"]
        self.assertEqual((value["status"], value["drawn"], value["left"], value["visible"]),
                         ("partial-left", 2, 1, 3))
        self.assertEqual(value["displayUs"], 900000)
        uses = [m["uses"][0] for m in value["members"]]
        self.assertEqual(uses[0]["leftUs"], "301000")
        self.assertFalse(uses[0]["failed"])
        self.assertEqual([u["submittedUs"] for u in uses[1:]], ["701000", "901000"])

    def test_new_tile_request_does_not_modify_earlier_batch(self):
        new = self.samples["new_tile_new_batch"]
        old = self.samples["old_batch_after_new_tile"]
        self.assertNotEqual(new["id"], old["id"])
        self.assertEqual((new["visible"], new["displayUs"]), (1, 300000))
        self.assertEqual((old["visible"], old["left"], old["displayUs"]), (3, 1, 900000))

    def test_a_completed_tile_keeps_its_time_after_it_leaves(self):
        value = self.samples["drawn_tile_leaves"]
        self.assertEqual((value["status"], value["drawn"], value["left"]), ("complete", 2, 0))
        self.assertEqual(value["displayUs"], 700000)
        self.assertEqual(value["members"][0]["uses"][0]["submittedUs"], "201000")

    def test_completed_batch_survives_hide_failure_teardown_and_loss(self):
        value = self.samples["completed_stays_historical"]
        self.assertEqual((value["status"], value["displayUs"]), ("complete", 700000))
        self.assertEqual(value["mlStatus"], "complete")

    def test_normal_cancellation_after_departure_does_not_become_failure(self):
        value = self.samples["left_then_cancelled"]
        self.assertEqual((value["status"], value["left"]), ("departed", 1))
        use = value["members"][0]["uses"][0]
        self.assertFalse(use["failed"])
        self.assertTrue(use["exact"])
        self.assertIsNone(value["displayUs"])

    def test_reentry_does_not_restart_a_departed_measurement(self):
        value = self.samples["reentry_does_not_revive"]
        self.assertEqual((value["status"], value["left"], value["drawn"]), ("departed", 1, 0))
        self.assertIsNone(value["displayUs"])

    def test_draw_before_departure_can_finish_after_reentry(self):
        value = self.samples["draw_before_left_swap_after_reentry"]
        self.assertEqual((value["status"], value["left"], value["drawn"]), ("complete", 0, 1))
        self.assertEqual(value["displayUs"], 700000)

    def test_failed_old_frame_is_not_completed_by_a_new_reentry_draw(self):
        value = self.samples["failed_old_frame_not_rescued_by_reentry"]
        self.assertEqual((value["status"], value["left"], value["drawn"]), ("departed", 1, 0))
        self.assertIsNone(value["displayUs"])

    def test_departure_does_not_erase_an_earlier_failure(self):
        value = self.samples["failure_before_departure"]
        self.assertEqual((value["status"], value["left"]), ("partial", 1))
        self.assertIsNone(value["displayUs"])

    def test_original_publication_error_before_draw_stays_partial(self):
        value = self.samples["original_error_then_draw"]
        self.assertEqual((value["status"], value["drawn"]), ("partial", 1))
        self.assertIsNone(value["displayUs"])
        self.assertTrue(value["members"][0]["uses"][0]["originalFailed"])

    def test_error_after_success_does_not_revoke_the_success(self):
        value = self.samples["original_error_after_draw"]
        self.assertEqual((value["status"], value["displayUs"]), ("complete", 200000))

    def test_late_admission_recovers_original_membership_and_departure_time(self):
        value = self.samples["late_overlap_admission"]
        self.assertEqual((value["status"], value["visible"], value["left"]), ("partial-left", 2, 1))
        self.assertEqual(value["displayUs"], 700000)
        self.assertEqual(value["members"][0]["uses"][0]["leftUs"], "201000")

    def test_late_admission_keeps_pre_departure_content_invalidation(self):
        value = self.samples["late_invalidated_departure"]
        self.assertEqual(value["status"], "partial")
        use = value["members"][0]["uses"][0]
        self.assertTrue(use["failed"])
        self.assertEqual(use["leftUs"], "201000")
        self.assertEqual(value["mlStatus"], "complete")

    def test_late_admission_cannot_hide_missing_transition_history(self):
        for name in ("late_departure_with_missing_history", "evicted_origin_before_admission"):
            with self.subTest(name=name):
                self.assertEqual(self.samples[name]["status"], "partial")
                self.assertIsNone(self.samples[name]["displayUs"])

    def test_recorded_departure_survives_later_history_eviction_and_loss(self):
        value = self.samples["departure_survives_history_eviction"]
        self.assertEqual((value["status"], value["left"]), ("departed", 1))
        self.assertEqual(value["members"][0]["uses"][0]["leftUs"], "201000")

    def test_missing_overlap_event_does_not_fabricate_departure_or_success(self):
        value = self.samples["missing_overlap_keeps_uncertainty"]
        self.assertEqual((value["status"], value["left"], value["visible"]), ("partial", 0, 2))
        self.assertIsNone(value["displayUs"])

    def test_accepted_empty_layout_can_finish_a_surviving_tile(self):
        value = self.samples["empty_after_overlap"]
        self.assertEqual((value["status"], value["noDraw"], value["left"]), ("no-draw", 1, 0))
        self.assertIsNone(value["displayUs"])

    def test_stale_empty_layout_does_not_clear_content_invalidation(self):
        value = self.samples["stale_empty_after_content_invalidation"]
        self.assertEqual((value["status"], value["noDraw"]), ("partial", 0))
        self.assertEqual(self.samples["retained_draw_after_content_invalidation"]["displayUs"], 700000)

    def test_eligibility_change_does_not_discard_valid_current_empty_evidence(self):
        value = self.samples["empty_after_eligibility_change"]
        self.assertEqual((value["status"], value["noDraw"]), ("no-draw", 1))

    def test_no_draw_evidence_uses_current_wrapped_key(self):
        value = self.samples["actual_key_empty_after_wrap_change"]
        self.assertEqual((value["visible"], value["noDraw"], value["left"]), (2, 1, 1))
        self.assertEqual([u["noDraw"] for u in value["members"][0]["uses"]], [0, 1])

    def test_one_wrap_can_leave_while_another_keeps_timing(self):
        value = self.samples["one_wrap_left_one_drawn"]
        self.assertEqual((value["status"], value["visible"], value["left"]), ("partial-left", 2, 1))
        self.assertEqual(value["displayUs"], 700000)

    def test_departure_in_one_map_does_not_stop_another_map(self):
        value = self.samples["only_one_map_loses_tile"]
        self.assertEqual((value["status"], value["visible"], value["left"]), ("partial-left", 2, 1))
        self.assertEqual(value["displayUs"], 700000)

    def test_known_departure_ignores_later_publication_failure_and_teardown(self):
        value = self.samples["departure_ignores_later_failure"]
        self.assertEqual((value["status"], value["left"]), ("departed", 1))
        self.assertEqual(value["mlStatus"], "complete")

    def test_same_required_key_keeps_draw_timer_across_style_revision(self):
        value = self.samples["same_key_style_change_keeps_draw_timer"]
        self.assertEqual((value["status"], value["displayUs"]), ("complete", 700000))


if __name__ == "__main__":
    unittest.main()
