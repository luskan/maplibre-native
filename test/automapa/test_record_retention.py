import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RecordRetentionTest(unittest.TestCase):
  @classmethod
  def setUpClass(cls):
    with tempfile.TemporaryDirectory(prefix="record-retention-") as directory:
      binary = str(Path(directory) / "retention")
      subprocess.run(["g++", "-std=c++17", "-O2", "-pthread", "-DTILE_TRACE_TESTING",
                      "-I", str(ROOT / "include"), str(ROOT / "test/automapa/record_retention_harness.cpp"),
                      "-o", binary], check=True)
      lines = subprocess.check_output([binary], text=True, timeout=30).splitlines()
    cls.samples = {lines[i]: json.loads(lines[i + 1]) for i in range(0, len(lines), 2)}

  def record(self, sample, identity):
    return next(r for r in self.samples[sample]["records"] if r["id"] == str(identity))

  def test_sparse_collision_retains_pending_demand_and_later_binding(self):
    for name in ("collision_retained", "late_bound", "late_submitted"):
      self.assertEqual(self.samples[name]["lost"], 0)
      self.assertEqual(self.samples[name]["evicted"], 0)
    self.assertEqual(self.record("late_bound", 55056)["generation"], "58720")
    self.assertEqual(self.record("late_submitted", 55056)["outcome"], "submitted")
    self.assertEqual(self.record("late_submitted", 55056)["timesUs"][13], "7000")
    self.assertTrue(self.record("late_submitted", 55056)["firstUseComplete"])

  def test_wrapped_collision_updates_exact_id_and_exports_each_record_once(self):
    sample = self.samples["wrapped_chain"]
    self.assertEqual(sample["evicted"], 0)
    self.assertEqual(sorted(r["id"] for r in sample["records"]), ["1023", "2047", "3071"])
    self.assertEqual(self.record("wrapped_chain", 2047)["publication"], "12345")
    self.assertEqual(self.record("wrapped_chain", 1023)["publication"], "0")

  def test_relocated_demand_is_found_by_publication_index_at_submission(self):
    self.assertEqual(self.samples["wrapped_submitted"]["lost"], 0)
    self.assertEqual(self.record("wrapped_submitted", 2047)["outcome"], "submitted")
    self.assertEqual(self.record("wrapped_submitted", 2047)["timesUs"][13], "7000")
    self.assertTrue(self.record("wrapped_submitted", 2047)["firstUseComplete"])
    for identity in (1023, 3071):
      self.assertEqual(self.record("wrapped_submitted", identity)["outcome"], "pending")
      self.assertEqual(self.record("wrapped_submitted", identity)["timesUs"][13], "0")

  def test_reset_clears_probe_chains_and_binding(self):
    self.assertEqual(self.samples["reset_chain"]["records"], [])
    self.assertEqual(len(self.samples["reset_rebound"]["records"]), 1)
    self.assertEqual(self.record("reset_rebound", 2047)["publication"], "12346")

  def test_full_table_keeps_capacity_and_counts_real_eviction(self):
    before, after = self.samples["full_before"], self.samples["full_replaced"]
    self.assertEqual(len(before["records"]), 1024)
    self.assertEqual(before["evicted"], 0)
    self.assertEqual(after["evicted"], 1)
    self.assertEqual(self.record("full_replaced", 1024)["outcome"], "truncated")
    self.assertEqual(self.samples["storage"]["capacity"], 1024)

  def test_evicted_demand_is_not_recovered_as_complete(self):
    self.assertEqual(self.samples["evicted_bind_rejected"]["lost"], 1)
    for name, identity in (("archived_history_stays_truncated", 1024),
                           ("unarchived_history_stays_truncated", 1124)):
      row = self.record(name, identity)
      self.assertEqual(row["outcome"], "truncated")
      self.assertFalse(row["firstUseComplete"])
      self.assertFalse(row["firstDrawComplete"])

  def test_full_chain_finds_existing_id_before_replacing_and_bounds_lookup(self):
    before, after = self.samples["full_collision_chain"], self.samples["full_existing_updated"]
    self.assertEqual(len(before["records"]), 1024)
    self.assertEqual(before["evicted"], 0)
    self.assertEqual(after["evicted"], 0)
    self.assertEqual(after["lost"], 0)
    self.assertEqual(sum(r["publication"] == "9999" for r in after["records"]), 1)
    self.assertEqual(self.samples["full_existing_probes"]["probes"], 1024)
    self.assertEqual(self.samples["full_missing_probes"]["probes"], 1024)
    self.assertEqual(self.samples["full_missing_rejected"]["lost"], 1)


if __name__ == "__main__":
  unittest.main()
