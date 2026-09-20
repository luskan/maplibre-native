import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CASES = ('batch-lock', 'record-lock', 'multi-use', 'failed-then-success', 'late-demand', 'late-binding',
         'late-member', 'pause-before-swap', 'reset-before-swap', 'reset-unpublished', 'unpublished',
         'later-no-draw', 'later-error', 'later-teardown', 'invalidation', 'reordered', 'equal-time-order',
         'incremental', 'overflow', 'snapshot-race', 'direct-rebind', 'publication-race', 'retry-exhaustion', 'archived-uncertainty', 'sustained-frames', 'direct-rebind-evicted', 'rejected-old', 'surface-isolation', 'short-batch-lock')


class SwapQueueTest(unittest.TestCase):
  @classmethod
  def setUpClass(cls):
    cls.samples = {}
    with tempfile.TemporaryDirectory(prefix='swap-queue-') as directory:
      executable = str(Path(directory) / 'queue')
      subprocess.run(['g++', '-std=c++17', '-O2', '-pthread', '-DTILE_TRACE_TESTING',
        '-I', str(ROOT / 'include'), str(ROOT / 'test/automapa/swap_queue_harness.cpp'), '-o', executable], check=True)
      for case in CASES:
        lines = subprocess.check_output([executable, case], text=True, timeout=15).splitlines()
        cls.samples[case] = {lines[i]: json.loads(lines[i+1]) for i in range(0,len(lines),2)}

  def test_contended_swap_preserves_original_exact_submission(self):
    for case in ('batch-lock', 'record-lock', 'multi-use'):
      with self.subTest(case=case):
        result = self.samples[case]['after']
        self.assertEqual(result['lost'], 0)
        self.assertTrue(result['recordSnapshotComplete'])
        demand = next(r for r in result['records'] if r['kind'] == 0)
        self.assertEqual(demand['timesUs'][13], '5000')
        self.assertTrue(demand['firstUseComplete'])
        self.assertEqual(self.samples[case]['batch']['status'], 'complete')
        self.assertEqual(self.samples[case]['batch']['displayUs'], 4000)
    demand = next(r for r in self.samples['multi-use']['after']['records'] if r['kind'] == 0)
    self.assertEqual(demand['geometrySubmittedUs'], '5000')
    self.assertEqual(demand['symbolSubmittedUs'], '5000')

  def test_failure_and_success_are_separate_events(self):
    data = self.samples['failed-then-success']
    failed = next(r for r in data['failed']['records'] if r['kind'] == 0)
    success = next(r for r in data['after']['records'] if r['kind'] == 0)
    self.assertEqual(failed['failedSwaps'], 1)
    self.assertEqual(failed['timesUs'][13], '0')
    self.assertEqual(success['failedSwaps'], 1)
    self.assertEqual(success['timesUs'][13], '8000')

  def test_replay_cannot_submit_later_demand_binding_or_membership(self):
    for case in ('late-demand','late-binding','late-member'):
      data = self.samples[case]
      later = next(r for r in data['after']['records'] if r['id'] == data['later']['id'])
      self.assertEqual(later['timesUs'][13], '0', case)
      self.assertEqual(later['outcome'], 'pending', case)
    self.assertNotEqual(self.samples['late-member']['batch']['status'], 'complete')

  def test_capture_transition_never_relabels_old_draws(self):
    for case in ('pause-before-swap','reset-before-swap','reset-unpublished'):
      result = self.samples[case]['after']
      self.assertTrue(all(r['timesUs'][13] == '0' for r in result['records']))
    result = self.samples['reset-unpublished']['after']
    self.assertEqual(int(result['swapQueue']['discarded']), 1)
    self.assertEqual(result['records'], [])

  def test_unpublished_or_partial_frames_prevent_complete_snapshots(self):
    for case in ('unpublished','reset-unpublished','incremental','snapshot-race'):
      with self.subTest(case=case):
        self.assertFalse(self.samples[case]['pending']['recordSnapshotComplete'])
        self.assertFalse(self.samples[case]['pending']['swapQueue']['available'])
        self.assertTrue(self.samples[case]['after']['recordSnapshotComplete'])
    self.assertFalse(self.samples['unpublished']['pending_batch']['available'])

  def test_reordering_and_overtaken_terminal_outcomes_are_explicit_uncertainty(self):
    for case in ('later-no-draw','later-error','later-teardown','reordered','equal-time-order'):
      with self.subTest(case=case):
        result = self.samples[case]['after']
        self.assertGreater(int(result['lossDiagnostics']['lifetimeCounts']['swap-replay-order']), 0)
        self.assertFalse(any(r['firstUseComplete'] for r in result['records']))
        self.assertNotEqual(self.samples[case]['batch']['status'], 'complete')

  def test_invalidation_after_draw_keeps_original_draw_requirement(self):
    result = self.samples['invalidation']
    self.assertEqual(result['after']['lost'], 0)
    self.assertTrue(result['after']['recordSnapshotComplete'])

  def test_direct_retained_demand_rebind_is_not_overwritten(self):
    for case in ('direct-rebind', 'direct-rebind-evicted'):
      data = self.samples[case]
      rebound = next(r for r in data['after']['records'] if r['id'] == data['rebound']['id'])
      self.assertEqual(rebound['publication'], data['rebound']['publication'])
      self.assertEqual(rebound['timesUs'][13], '0')
      self.assertFalse(rebound['firstUseComplete'])
      self.assertGreater(int(data['after']['lossDiagnostics']['lifetimeCounts']['swap-replay-order']), 0)

  def test_replay_uncertainty_invalidates_evicted_startup_first_use(self):
    result = self.samples['archived-uncertainty']['after']
    originals = [r for r in result['records'] if r['map'] == '1' and r['source'] == '2']
    self.assertTrue(originals)
    self.assertTrue(any(r['early'] for r in originals))
    self.assertFalse(any(r['firstUseComplete'] or r['firstDrawComplete'] for r in originals))
    self.assertGreater(int(result['lossDiagnostics']['lifetimeCounts']['swap-replay-order']), 0)

  def test_consumer_remains_active_until_batch_publication_finishes(self):
    data = self.samples['publication-race']
    self.assertFalse(data['pending_batch']['available'])
    self.assertTrue(data['after']['recordSnapshotComplete'])
    self.assertEqual(data['batch']['status'], 'complete')

  def test_reservation_retry_exhaustion_is_bounded_and_visible(self):
    data = self.samples['retry-exhaustion']
    self.assertEqual(data['attempts']['conflicts'], 4)
    self.assertEqual(int(data['after']['lossDiagnostics']['lifetimeCounts']['swap-queue-contention']), 1)
    self.assertEqual(int(data['after']['swapQueue']['rejected']), 1)

  def test_sustained_large_frames_do_not_accumulate_without_observer_reads(self):
    value = self.samples['sustained-frames']['after']
    self.assertEqual(value['pendingSwapFrames'], 0)
    self.assertEqual(value['swapQueue']['rejected'], '0')
    self.assertEqual(value['swapQueue']['applied'], '40')
    self.assertEqual(value['lost'], 0)

  def test_rejected_older_swap_revokes_raw_first_use_for_its_epoch(self):
    data = self.samples['rejected-old']
    self.assertFalse(data['after']['swapQueue']['epochQualified'])
    self.assertFalse(any(r['firstUseComplete'] or r['firstDrawComplete'] for r in data['after']['records']))
    self.assertTrue(data['reset']['swapQueue']['epochQualified'])

  def test_surface_identity_does_not_contend_with_record_snapshots(self):
    data=self.samples['surface-isolation']
    self.assertGreater(int(data['surface']['id']),0)
    reasons=('surface-create-lock','swap-surface-lock','surface-destroy-lock')
    for reason in reasons:
      self.assertEqual(data['after']['lossDiagnostics']['lifetimeCounts'][reason],'0')
      self.assertEqual(data['contended']['lossDiagnostics']['lifetimeCounts'][reason],'1')
      self.assertEqual(data['contended']['lossDiagnostics']['snapshotOverlapCounts'][reason],'0')

  def test_raw_replay_releases_batch_lock_but_keeps_snapshot_and_dependency_guards(self):
    data=self.samples['short-batch-lock']
    self.assertFalse(data['pending_batch']['available'])
    self.assertTrue(data['during']['rawBlocked'])
    self.assertEqual(data['after']['lossDiagnostics']['lifetimeCounts']['batch-outcome'],'0')
    self.assertEqual(data['after']['swapQueue']['applied'],'2')
    following=next(r for r in data['after']['records'] if r['id']==data['during']['nextId'])
    self.assertEqual(following['timesUs'][13],'7000')
    self.assertTrue(following['firstUseComplete'])

  def test_overflow_remains_loss_and_storage_accounting_is_bounded(self):
    result = self.samples['overflow']['after']
    self.assertEqual(int(result['lossDiagnostics']['lifetimeCounts']['swap-queue-full']), 1)
    queue = result['swapQueue']
    self.assertEqual(int(queue['reserved']), int(queue['applied']) + int(queue['discarded']))
    self.assertEqual(int(queue['rejected']), 1)
    self.assertLess(queue['storageBytes'], 2 * 1024**2)


if __name__ == '__main__':
  unittest.main()
