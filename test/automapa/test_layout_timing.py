import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class LayoutTimingTests(unittest.TestCase):
  @classmethod
  def setUpClass(cls):
    with tempfile.TemporaryDirectory(prefix='layout-timing-') as folder:
      binary = str(Path(folder)/'test')
      subprocess.run(['g++','-std=c++17','-O2','-pthread','-DTILE_TRACE_TESTING',
        '-I',str(ROOT/'include'),str(ROOT/'test/automapa/layout_timing_harness.cpp'),'-o',binary],check=True)
      rows = subprocess.check_output([binary],text=True,timeout=30).splitlines()
    cls.samples = {rows[i]:json.loads(rows[i+1]) for i in range(0,len(rows),2)}

  def test_wall_partition_and_cpu_are_separate(self):
    r = self.samples['first']['records'][0]
    self.assertEqual(int(r['ownerAcceptedUs'])-int(r['deliveredUs']),940)
    work = sum(int(p['wallUs']) for p in r['work'].values())
    gaps = sum(int(p['wallUs']) for p in r['gaps'].values())
    self.assertEqual((work,gaps),(170,500))
    self.assertEqual(int(r['resultPostedUs'])-int(r['workerReceivedUs'])-work-gaps,50)
    self.assertEqual(sum(int(p['threadCpuUs']) for p in r['work'].values()),110)
    self.assertEqual(int(r['ownerAcceptedUs'])-int(r['ownerReceivedUs']),40)

  def test_reparse_copies_do_not_mutate_previous_result(self):
    a,b = self.samples['reparse']['records']
    self.assertEqual(a['work']['parse']['wallUs'],'100')
    self.assertEqual(b['work']['parse']['wallUs'],'200')
    self.assertEqual((a['resultsBefore'],b['resultsBefore']),('0','1'))
    self.assertEqual(b['gaps']['coalescing']['wallUs'],'75')
    self.assertEqual(b['gaps']['coalescing']['pendingIntervals'],'1')
    self.assertEqual(b['gaps']['dependencies']['wallUs'],'500')

  def test_terminal_preserves_pending_time_without_an_invented_result(self):
    r = self.samples['terminal']['records'][-1]
    self.assertEqual(r['disposition'],'replaced')
    self.assertEqual((r['layoutId'],r['resultPostedUs'],r['ownerAcceptedUs']),('0','0','0'))
    self.assertEqual(r['terminalUs'],'1300')
    self.assertEqual(r['gaps']['dependencies']['wallUs'],'650')

  def test_clock_failure_does_not_create_cpu_or_negative_durations(self):
    r = self.samples['cpu_missing']['records'][-1]
    self.assertTrue(r['valid'])
    self.assertFalse(r['work']['parse']['cpuAvailable'])
    self.assertIsNone(r['work']['parse']['threadCpuUs'])
    r = self.samples['bad_clock']['records'][-1]
    self.assertFalse(r['valid'])
    self.assertEqual(r['work']['finalize']['wallUs'],'0')
    self.assertEqual(self.samples['exception']['records'][-1]['disposition'],'error')

  def test_top_groups_are_bounded_subsets(self):
    r = self.samples['groups']['records'][-1]
    self.assertEqual(r['groupsSeen'],'12')
    self.assertEqual(sorted(int(g['work']['wallUs']) for g in r['topGroups']),list(range(5,13)))

  def test_split_totals_cover_groups_outside_the_retained_eight(self):
    r = self.samples['split']['records'][-1]
    split = r['groupSplit']
    self.assertEqual((split['inputFeatures'],split['examined'],split['matched']),('1200','1200','300'))
    self.assertEqual((split['countedGroups'],split['uncountedGroups']),('12','0'))
    self.assertEqual(split['work']['selection']['wallUs'],'240')
    self.assertEqual(split['work']['bucket']['wallUs'],'550')
    self.assertEqual(split['work']['deferredPreparation']['wallUs'],'90')
    self.assertEqual(split['work']['deferredBucket']['wallUs'],'40')
    self.assertEqual(sum(int(g['split']['work']['selection']['wallUs']) for g in r['topGroups']),160)
    self.assertNotIn('1',[g['ordinal'] for g in r['topGroups']])
    last = next(g for g in r['topGroups'] if g['ordinal']=='12')
    self.assertEqual(last['split']['work']['deferredBucket']['wallUs'],'40')

  def test_reparse_rejects_deferred_keys_without_changing_prior_result(self):
    old = self.samples['split']['records'][-1]['groupSplit']
    current = self.samples['split_reparse']['records'][-1]['groupSplit']
    self.assertEqual((old['rejectedDeferred'],current['rejectedDeferred']),('0','1'))
    self.assertEqual(current['work']['deferredBucket'],old['work']['deferredBucket'])

  def test_overwrite_and_contention_are_explicit(self):
    s = self.samples['bounded']
    self.assertEqual((s['sequence'],s['overwritten'],len(s['records'])),('129','1',128))
    self.assertEqual(self.samples['contended']['lost'],'1')

  def test_observer_and_capture_transitions_reject_old_profiles(self):
    self.assertFalse(self.samples['disabled']['enabled'])
    for name in ('old_observer','capture_toggle','capture_reset'):
      with self.subTest(name=name):
        self.assertEqual(self.samples[name]['records'],[])
        self.assertGreater(int(self.samples[name]['stale']),0)


if __name__=='__main__':
  unittest.main()
