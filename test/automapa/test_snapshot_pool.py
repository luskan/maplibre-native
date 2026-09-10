#!/usr/bin/env python3
"""Retained snapshots remain coherent while shared publication buffers are reused."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SnapshotPoolTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory(prefix="snapshot-pool-") as directory:
            binary = str(Path(directory) / "pool")
            subprocess.run([
                "g++", "-std=c++17", "-pthread", "-DTILE_TRACE_TESTING", "-I", str(ROOT / "include"),
                str(ROOT / "test/automapa/snapshot_pool_harness.cpp"), "-o", binary,
            ], check=True)
            lines = subprocess.check_output([binary], text=True, timeout=60).splitlines()
        cls.samples = {lines[i]: json.loads(lines[i + 1]) for i in range(0, len(lines), 2)}

    def assert_complete(self, value, count):
        self.assertEqual((value["status"], value["mlStatus"]), ("complete", "complete"))
        self.assertEqual((value["total"], value["drawn"], len(value["members"])), (count, count, count))
        self.assertEqual((value["displayUs"], value["mlUs"], value["captureLost"]), (40, 10, 0))
        self.assertEqual(len({m["publication"] for m in value["members"]}), count)

    def test_pool_size_and_initial_partition(self):
        self.assertEqual(self.samples["storage"]["buffers"], 18)
        self.assertLess(self.samples["storage"]["collectorBytes"], 10 * 1024 * 1024)

    def test_all_sixteen_snapshots_remain_readable(self):
        ids = set()
        for i in range(16):
            value = self.samples[f"retained_{i}"]
            self.assert_complete(value, i % 3 + 1)
            ids.add(value["id"])
        self.assertEqual(len(ids), 16)

    def test_seventeenth_batch_only_evicts_oldest_slot(self):
        self.assert_complete(self.samples["seventeenth"], 1)
        evicted = self.samples["evicted"]
        self.assertEqual((evicted["status"], evicted["members"]), ("partial", []))
        self.assertIsNone(evicted["displayUs"])
        for i in range(1, 16):
            value = self.samples[f"after_eviction_{i}"]
            self.assert_complete(value, i % 3 + 1)
            self.assertEqual(value["members"], self.samples[f"retained_{i}"]["members"])

    def test_smaller_batch_does_not_expose_stale_tail(self):
        self.assert_complete(self.samples["small_replacement"], 1)
        self.assertEqual(self.samples["large_evicted"]["members"], [])

    def test_pinned_old_snapshot_stays_immutable_while_writer_progresses(self):
        old, current = self.samples["pinned_old"], self.samples["pinned_current"]
        self.assert_complete(old, 1)
        self.assert_complete(current, 1)
        self.assertEqual(old["members"][0]["failure"], "pending")
        self.assertEqual(current["members"][0]["failure"], "error")
        self.assertEqual(self.samples["pinned_progress"]["publicationsWhilePinned"], 513)

    def test_second_spare_can_be_pinned_without_blocking_writer(self):
        self.assert_complete(self.samples["second_spare_result"], 1)
        self.assertEqual(self.samples["second_spare_progress"]["publicationsWhilePinned"], 512)

    def test_stale_index_and_aba_do_not_mix_batches_or_versions(self):
        for mode in (0, 1):
            value = self.samples[f"stale_index_{mode}"]
            self.assert_complete(value, 1)
            self.assertEqual(value["members"][0]["failure"], "error")
        replaced = self.samples["stale_index_2"]
        self.assertEqual((replaced["status"], replaced["members"]), ("partial", []))

    def test_reset_keeps_pinned_storage_and_publishes_new_session(self):
        old = self.samples["reader_across_reset"]
        self.assertTrue(old["deferred"])
        self.assertEqual(self.samples["old_after_reset"]["members"], [])
        for i in range(16):
            value = self.samples[f"reset_fresh_{i}"]
            self.assert_complete(value, i % 3 + 1)
            self.assertNotEqual(value["session"], old["oldSession"])

    def test_multiple_readers_obtain_coherent_results_during_publication(self):
        for reader in range(3):
            for i in range(64):
                with self.subTest(reader=reader, snapshot=i):
                    value = self.samples[f"concurrent_{reader}_{i}"]
                    expected = self.samples[f"concurrent_expected_{i % 16}"]
                    self.assert_complete(value, (i % 16) % 3 + 1)
                    self.assertEqual(value["id"], expected["id"])
                    self.assertEqual(value["session"], expected["session"])
                    self.assertEqual([m["publication"] for m in value["members"]],
                                     [m["publication"] for m in expected["members"]])


if __name__ == "__main__":
    unittest.main()
