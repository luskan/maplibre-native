import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class LossDiagnosticsTest(unittest.TestCase):
  @classmethod
  def setUpClass(cls):
    with tempfile.TemporaryDirectory(prefix="loss-diagnostics-") as directory:
      executable = str(Path(directory) / "loss")
      subprocess.run(["g++", "-std=c++17", "-O2", "-pthread", "-DTILE_TRACE_TESTING",
                      "-I", str(ROOT / "include"),
                      str(ROOT / "test/automapa/loss_diagnostics_harness.cpp"), "-o", executable], check=True)
      lines = subprocess.check_output([executable], text=True, timeout=20).splitlines()
    cls.samples = {lines[i]: json.loads(lines[i + 1]) for i in range(0, len(lines), 2)}

  def diagnostics(self, name):
    value = self.samples[name]
    return value.get("lossDiagnostics", value)

  def delta(self, before, after, group="lifetimeCounts"):
    a, b = self.diagnostics(before)[group], self.diagnostics(after)[group]
    return {key: int(b[key]) - int(a[key]) for key in a}

  def test_lock_paths_and_capacity_are_distinct(self):
    stages = ["initial", "store_lock", "activity_lock", "bind_lock", "bind_missing", "retire_lock",
              "retained_lock", "swap_surface_lock", "swap_records_lock", "surface_create_lock",
              "surface_capacity", "surface_destroy_lock", "member_capacity"]
    reasons = ["record-store-lock", "activity-lock", "bind-demand-lock", "bind-demand-missing",
               "retire-source-lock", "retained-draw-lock", "swap-surface-lock", "swap-records-lock",
               "surface-create-lock", "surface-capacity", "surface-destroy-lock", "batch-member-capacity"]
    for before, after, reason in zip(stages, stages[1:], reasons):
      with self.subTest(reason=reason):
        delta = self.delta(before, after)
        self.assertEqual(delta[reason], 0 if reason in ("record-store-lock", "swap-records-lock") else 1)
        self.assertEqual(sum(delta.values()), self.samples[after]["lost"] - self.samples[before]["lost"])

  def test_batch_categories_and_use_capacity(self):
    self.assertEqual(self.delta("reset", "use_capacity")["batch-use-capacity"], 1)
    delta = self.delta("use_capacity", "batch_and_explicit")
    self.assertEqual({key for key, value in delta.items() if value},
                     {"batch-cache", "batch-screen", "batch-outcome", "batch-view", "explicit"})
    self.assertEqual(sum(delta.values()), 5)

  def test_capture_reset_does_not_erase_lifetime_diagnostics(self):
    self.assertEqual(sum(self.delta("member_capacity", "reset").values()), 0)
    self.assertEqual(self.samples["reset"]["lost"], 0)
    self.assertNotEqual(self.samples["reset"]["session"], self.samples["member_capacity"]["session"])

  def test_snapshot_contention_is_reproduced_and_identified(self):
    self.assertEqual(self.samples["before_observer"]["lost"], 0)
    self.assertEqual(self.samples["observer_loss"]["lost"], 1)
    self.assertEqual(self.delta("before_observer", "after_observer")["activity-lock"], 1)
    self.assertEqual(self.delta("before_observer", "after_observer", "snapshotOverlapCounts")["activity-lock"], 1)

  def test_in_progress_and_reset_crossing_reads_are_unavailable(self):
    for name in ("during_writer", "read_crossing_reset", "during_reset"):
      with self.subTest(name=name):
        self.assertFalse(self.diagnostics(name)["available"])
    self.assertTrue(self.diagnostics("after_writer")["available"])
    self.assertTrue(self.diagnostics("after_reset")["available"])

  def test_late_context_epoch_and_unknown_provenance_are_explicit(self):
    self.assertEqual(int(self.diagnostics("foreign_epoch")["foreignEpochCounts"]["explicit"]), 1)
    self.assertGreater(int(self.diagnostics("after_observer")["unknownEpochCounts"]["activity-lock"]), 0)

  def test_concurrent_losses_reconcile_and_consistent_samples_match_legacy(self):
    self.assertEqual(self.delta("before_concurrent", "after_concurrent")["explicit"], 400)
    self.assertEqual(self.samples["after_concurrent"]["lost"] - self.samples["before_concurrent"]["lost"], 400)
    for sample in self.samples.values():
      value = sample.get("lossDiagnostics")
      if value and value["available"]:
        self.assertEqual(value["session"], sample["session"])
        self.assertEqual(value["aggregateLost"], sample["lost"])
        self.assertLessEqual(int(value["sampleStartUs"]), int(value["sampleEndUs"]))
    self.assertLessEqual(self.samples["size"]["lossDiagnosticsBytes"], 1536)


if __name__ == "__main__":
  unittest.main()
