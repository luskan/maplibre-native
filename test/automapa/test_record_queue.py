import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RecordQueueTest(unittest.TestCase):
  @classmethod
  def setUpClass(cls):
    with tempfile.TemporaryDirectory(prefix="record-queue-") as directory:
      executable = str(Path(directory) / "queue")
      subprocess.run(["g++", "-std=c++17", "-O2", "-pthread", "-DTILE_TRACE_TESTING",
                      "-I", str(ROOT / "include"), str(ROOT / "test/automapa/record_queue_harness.cpp"),
                      "-o", executable], check=True)
      lines = subprocess.check_output([executable], text=True, timeout=20).splitlines()
    cls.samples = {lines[i]: json.loads(lines[i + 1]) for i in range(0, len(lines), 2)}

  def test_contended_request_is_present_before_binding(self):
    value = self.samples["retained_request"]
    self.assertEqual(value["lost"], 0)
    self.assertEqual(len(value["records"]), 1)
    self.assertEqual(value["records"][0]["timesUs"][0], "1000")
    self.assertEqual(self.samples["bound_request"]["records"][0]["publication"], "123")

  def test_deferred_request_and_layout_keep_exact_submission(self):
    value = self.samples["submitted_after_deferral"]
    demand = next(r for r in value["records"] if r["kind"] == 0)
    self.assertEqual(value["lost"], 0)
    self.assertEqual(demand["outcome"], "submitted")
    self.assertEqual(demand["timesUs"][13], "5000")
    self.assertTrue(demand["firstUseComplete"])

  def test_replay_does_not_hide_intervening_loss_or_capture_pause(self):
    value = self.samples["baseline_loss_preserved"]
    self.assertEqual(value["lost"], 1)
    self.assertFalse(value["records"][0]["firstDrawComplete"])
    self.assertFalse(self.samples["paused_with_pending"]["enabled"])
    self.assertEqual(len(self.samples["paused_with_pending"]["records"]), 1)
    self.assertEqual(self.samples["resumed"]["captureGaps"], 1)
    self.assertFalse(self.samples["resumed"]["records"][0]["firstDrawComplete"])

  def test_overflow_and_reservation_contention_remain_real_losses(self):
    before, after = self.samples["before_full"], self.samples["full_queue"]
    self.assertEqual(after["lost"], 1)
    self.assertEqual(int(after["recordQueue"]["applied"]) - int(before["recordQueue"]["applied"]), 64)
    self.assertEqual(int(after["recordQueue"]["rejected"]) - int(before["recordQueue"]["rejected"]), 1)
    self.assertEqual(self.samples["reservation_contention"]["lost"], 1)
    self.assertEqual(self.samples["attempt_bound"]["conflicts"], 4)

  def test_unpublished_head_and_dependent_mutations_do_not_claim_completion(self):
    value = self.samples["unpublished_head"]
    self.assertFalse(value["recordSnapshotComplete"])
    self.assertEqual(value["pendingRecordUpdates"], 1)
    self.assertFalse(value["recordQueue"]["available"])
    self.assertFalse(value["lossDiagnostics"]["available"])
    self.assertGreater(self.samples["dependent_refused"]["lost"], 0)
    self.assertTrue(self.samples["head_recovered"]["recordSnapshotComplete"])

  def test_reset_does_not_reuse_an_unpublished_slot_or_publish_old_records(self):
    self.assertFalse(self.samples["reset_with_old_reservation"]["recordSnapshotComplete"])
    after = self.samples["old_epoch_discarded"]
    self.assertTrue(after["recordSnapshotComplete"])
    self.assertEqual(after["records"], [])
    self.assertEqual(after["lost"], 0)
    self.assertGreater(int(after["recordQueue"]["discarded"]), 0)

  def test_reservations_during_copy_or_timestamp_invalidate_snapshot(self):
    for prefix, recovered in [("snapshot_copy", "copy_recovered"), ("record_snapshot_timestamp", "timestamp_recovered")]:
      with self.subTest(prefix=prefix):
        self.assertFalse(self.samples[prefix]["recordSnapshotComplete"])
        self.assertFalse(self.samples[prefix]["recordQueue"]["available"])
        self.assertTrue(self.samples[recovered]["recordSnapshotComplete"])
        self.assertEqual(len(self.samples[recovered]["records"]), 1)

  def test_concurrent_accounting_reconciles_with_bounded_storage(self):
    before, after = self.samples["before_concurrent"], self.samples["after_concurrent"]
    a, b = before["recordQueue"], after["recordQueue"]
    self.assertTrue(after["recordSnapshotComplete"])
    self.assertEqual(int(b["reserved"]), int(b["applied"]) + int(b["discarded"]))
    applied = int(b["applied"]) - int(a["applied"])
    rejected = int(b["rejected"]) - int(a["rejected"])
    self.assertEqual(applied + rejected, 800)
    self.assertEqual(after["lost"], rejected)
    self.assertLess(self.samples["storage"]["queueBytes"], 32768)


if __name__ == "__main__":
  unittest.main()
