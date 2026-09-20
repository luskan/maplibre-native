import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SITES = ("finish-demand-lock", "bind-demand-lock", "bind-demand-capacity", "draw-capacity",
         "draw-view-lock", "swap-submit-lock", "no-draw-layout-lock")


class BatchScreenLossTest(unittest.TestCase):
  @classmethod
  def setUpClass(cls):
    cls.samples = {}
    with tempfile.TemporaryDirectory(prefix="batch-screen-") as directory:
      executable = str(Path(directory) / "screen")
      subprocess.run(["g++", "-std=c++17", "-O2", "-pthread", "-DTILE_TRACE_TESTING",
                      "-I", str(ROOT / "include"), str(ROOT / "test/automapa/batch_screen_loss_harness.cpp"),
                      "-o", executable], check=True)
      for case in (*SITES, "multi-use-swap", "reset-persistence", "concurrent-claims", "claim-order"):
        lines = subprocess.check_output([executable, case], text=True, timeout=15).splitlines()
        cls.samples[case] = {lines[i]: json.loads(lines[i + 1]) for i in range(0, len(lines), 2)}

  def test_each_production_branch_has_distinct_reconciled_attribution(self):
    for site in (s for s in SITES if s != "swap-submit-lock"):
      with self.subTest(site=site):
        value = self.samples[site]["after"]
        diagnostics = value["lossDiagnostics"]
        screen = diagnostics["batchScreen"]
        counts = {key: int(count) for key, count in screen["lifetimeCounts"].items()}
        self.assertTrue(diagnostics["available"])
        self.assertEqual(value["lost"], 1)
        self.assertEqual(sum(counts.values()), 1)
        self.assertEqual(counts[site], 1)
        self.assertEqual(diagnostics["lifetimeCounts"]["batch-screen"], "1")
        first = screen["firstRecordedEvent"]
        self.assertEqual(screen["firstEventState"], "ready")
        self.assertEqual(first["site"], site)
        self.assertEqual(first["ordinal"], "1")
        self.assertEqual(first["epoch"], value["session"])
        self.assertEqual(first["collectorEpoch"], value["session"])
        self.assertEqual(first["context"]["map"], "11")
        self.assertEqual(first["context"]["source"], "22")
        self.assertEqual(first["context"]["publication"], "44")

  def test_capacity_and_no_draw_evidence_use_safe_original_values(self):
    for site, limit in (("bind-demand-capacity", 8), ("draw-capacity", 512)):
      first = self.samples[site]["after"]["lossDiagnostics"]["batchScreen"]["firstRecordedEvent"]
      self.assertEqual(first["count"], limit)
      self.assertEqual(first["limit"], limit)
    first = self.samples["no-draw-layout-lock"]["after"]["lossDiagnostics"]["batchScreen"]["firstRecordedEvent"]
    self.assertEqual(first["evidenceUs"], "900")
    self.assertEqual(first["detectedUs"], "1000")

  def test_contended_swap_is_deferred_without_losing_its_uses(self):
    value = self.samples["multi-use-swap"]["after"]
    self.assertEqual(value["lost"], 0)
    self.assertEqual(value["lossDiagnostics"]["batchScreen"]["lifetimeCounts"]["swap-submit-lock"], "0")
    self.assertEqual(value["swapQueue"]["applied"], "1")
    self.assertEqual(self.samples["swap-submit-lock"]["after"]["lost"], 0)

  def test_first_event_and_origin_epoch_survive_capture_reset(self):
    values = self.samples["reset-persistence"]
    a, b = values["first"]["lossDiagnostics"]["batchScreen"], values["after"]["lossDiagnostics"]["batchScreen"]
    self.assertEqual(a["firstRecordedEvent"], b["firstRecordedEvent"])
    self.assertNotEqual(b["firstRecordedEvent"]["epoch"], values["after"]["session"])
    self.assertEqual(sum(int(n) for n in b["lifetimeCounts"].values()), 2)
    self.assertEqual(values["after"]["lost"], 1)
    self.assertEqual(values["after"]["lossDiagnostics"]["foreignEpochCounts"]["batch-screen"], "1")

  def test_in_progress_first_event_is_not_read_and_concurrent_losers_are_counted(self):
    values = self.samples["concurrent-claims"]
    writing = values["writing"]["lossDiagnostics"]
    self.assertFalse(writing["available"])
    self.assertEqual(writing["batchScreen"]["firstEventState"], "writing")
    self.assertIsNone(writing["batchScreen"]["firstRecordedEvent"])
    after = values["after"]["lossDiagnostics"]
    self.assertTrue(after["available"])
    self.assertEqual(after["batchScreen"]["firstRecordedEvent"]["site"], "draw-view-lock")
    self.assertEqual(after["batchScreen"]["lifetimeCounts"]["bind-demand-lock"], "4")
    self.assertEqual(after["lifetimeCounts"]["batch-screen"], "5")
    self.assertLessEqual(values["size"]["diagnosticsBytes"], 1536)

  def test_first_recorder_claim_retains_its_actual_atomic_ordinal(self):
    value = self.samples["claim-order"]["after"]
    first = value["lossDiagnostics"]["batchScreen"]["firstRecordedEvent"]
    self.assertEqual(value["lost"], 2)
    self.assertEqual(first["site"], "bind-demand-lock")
    self.assertEqual(first["ordinal"], "2")
    self.assertEqual(first["evidenceOrder"], "67")


if __name__ == "__main__":
  unittest.main()
