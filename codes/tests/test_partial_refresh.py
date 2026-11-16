#!/usr/bin/env python3
"""Real-binary regression for refreshing a partly filled data write frontier.

One percent occupancy leaves less than one live block. Read-only arrivals never
fill that block, so progress requires handling the partial frontier itself.
These tiny synthetic cases test executor correctness, not measured reliability.
"""
import csv
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('tarf_experiments', ROOT / 'codes/tests/support.py')
EXPERIMENTS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXPERIMENTS)
BINARY = Path(os.environ.get('TARF_TEST_BINARY', ROOT / 'codes/MQSim')).resolve()


class PartialRefreshTests(unittest.TestCase):
    def run_case(self, policy='tarf', overwrite=False, horizon=1_000_000_000,
                 thermal_trigger_during_program=False):
        temporary = tempfile.TemporaryDirectory(prefix='tarf_partial_refresh_')
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        config, workload, trace = [directory / x for x in ('ssd.xml', 'workload.xml', 'requests.trace')]
        geometry = EXPERIMENTS.geometry('debug32m')
        EXPERIMENTS.write_config(config, geometry, 321)
        EXPERIMENTS.write_workload(workload, trace, geometry)
        tree = ET.parse(config)
        tree.find('.//Preemptible_GC_Enabled').text = 'true'
        if thermal_trigger_during_program:
            # This fixture's thermal transition and short cutoff must both fall
            # inside the 0.8 ms NAND program, without changing device timing.
            for page_type in ('LSB', 'CSB', 'MSB'):
                self.assertEqual(int(tree.find('.//Page_Program_Latency_' + page_type).text), 800_000)
        tree.write(config, encoding='utf-8', xml_declaration=True)
        tree = ET.parse(workload)
        tree.find('.//Initial_Occupancy_Percentage').text = '1'
        tree.write(workload, encoding='utf-8', xml_declaration=True)
        # All LBAs are in the existing initial snapshot, including concurrent
        # overwrites. No newly allocated logical region can fill the old frontier.
        count, spacing = (48, 100_000) if overwrite else (8, 2_000_000)
        trace.write_text('0 0 0 16 0\n' if thermal_trigger_during_program else
                         ''.join(f'{i * spacing} 0 {(i % 16) * 16} 16 '
                                 f'{0 if overwrite and i % 2 == 0 else 1}\n' for i in range(count)))
        temperature = directory / 'temperature.csv'
        temperature.write_text('time_ns,temp_C\n0,30\n200000,70\n' if thermal_trigger_during_program
                               else 'time_ns,temp_C\n0,70\n')
        prefix = directory / 'result'
        env = {k: v for k, v in os.environ.items() if not k.startswith('TARF_')}
        env.update(TARF_POLICY=policy, TARF_HORIZON_NS=str(horizon), TARF_SCAN_NS='10000000',
                   TARF_OUTPUT_PREFIX=str(prefix), TARF_TRACE_NORMALIZED='1', TARF_INITIAL_PE='300',
                   TARF_INITIAL_AGE_DAYS='100', TARF_INITIAL_TEMP_C='30', TARF_THRESHOLD='0.3',
                   TARF_RISK_THRESHOLD='0.3', TARF_PERIOD_DAYS='30', TARF_TEMPERATURE_FILE=str(temperature),
                   TARF_RESOURCE_AWARE='0', TARF_REFRESH_MAX_PAGES_PER_CHIP='2',
                   TARF_REFRESH_MAX_BLOCKS_PER_CHIP='1', TARF_REFRESH_MAX_DEFER_NS='100000000')
        if thermal_trigger_during_program:
            env.update(TARF_THRESHOLD='0.5', TARF_RISK_THRESHOLD='0.5')
        result = subprocess.run([str(BINARY), '-i', str(config), '-w', str(workload)], cwd=directory,
                                env=env, stdin=subprocess.DEVNULL, capture_output=True,
                                text=True, errors='replace', timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout[-5000:] + result.stderr)
        rel = json.loads(Path(str(prefix) + '_reliability.json').read_text())
        host = json.loads(Path(str(prefix) + '_host.json').read_text())
        model = json.loads(Path(str(prefix) + '_model.json').read_text())
        def read_csv(suffix):
            with Path(str(prefix) + suffix).open() as f:
                return list(csv.DictReader(f))
        events = read_csv('_refresh_events.csv')
        episodes = read_csv('_risk_episodes.csv')
        migrations = read_csv('_migrations.csv')
        live, page_bytes = rel['initial_live_pages'], geometry['dimensions'][-1]
        self.assertGreater(live, 16)
        self.assertLess(live, geometry['dimensions'][-2])
        self.assertEqual(rel['initial_risky_blocks'], 0 if thermal_trigger_during_program else 1)
        self.assertEqual(rel['actual_sim_time_ns'], horizon)
        self.assertEqual(model['aging_time_scale'], 1)
        self.assertEqual(rel['erased_live_pages'], 0)
        self.assertEqual(rel['stale_program_completions'], 0)
        self.assertGreaterEqual(rel['pe_min'], 300)
        self.assertLessEqual(rel['refresh_migration_pages_per_chip_peak'], 2)
        self.assertEqual(rel['refresh_read_pages_submitted'] - rel['refresh_write_pages_completed'],
                         rel['refresh_migration_pages_outstanding'])
        self.assertEqual(rel['refresh_started'], rel['refresh_completed'] + rel['inflight_refresh_blocks'])
        self.assertEqual(rel['refresh_program_bytes_completed'], rel['refresh_write_pages_completed'] * page_bytes)
        self.assertEqual(rel['all_program_bytes'], sum(rel[k] for k in (
            'foreground_program_bytes_completed', 'mapping_program_bytes_completed',
            'refresh_program_bytes_completed', 'gc_program_bytes_completed')))
        for row in migrations:
            self.assertEqual(int(row['destination_pe']), 300)
            self.assertEqual(float(row['initial_age_days']), 0)
            self.assertEqual(float(row['program_temp_c']), 70)
            self.assertEqual(row['program_ns'], row['completion_ns'])
            self.assertEqual(row['source_live_after'], '0')
            self.assertEqual(row['destination_live'], '1')
        self.assertEqual(sum(int(e['censored_at_horizon']) for e in episodes), rel['pending_risky_blocks'])
        return rel, host, events, episodes

    def test_no_refresh_control_keeps_partial_old_data_risky(self):
        rel, host, _, _ = self.run_case(policy='none')
        self.assertEqual(rel['refresh_started'], 0)
        self.assertEqual(rel['all_program_bytes'], 0)
        self.assertEqual(rel['pending_risky_blocks'], 1)
        self.assertEqual(host['aggregate']['all']['pending'], 0)

    def test_all_refresh_policies_migrate_only_real_live_pages(self):
        for policy in ('periodic', 'age', 'tempdiff', 'tarf'):
            with self.subTest(policy=policy):
                rel, host, events, _ = self.run_case(policy=policy)
                live = rel['initial_live_pages']
                self.assertEqual(rel['refresh_read_pages_completed'], live)
                self.assertEqual(rel['refresh_write_pages_completed'], live)
                self.assertEqual(rel['partial_frontiers_retired'], 1)
                self.assertEqual(rel['source_tail_sealed_pages'], 64 - live)
                self.assertEqual(rel['all_program_bytes'], live * 8192)
                self.assertEqual(rel['refresh_completed'], 1)
                self.assertEqual(rel['refresh_erase_completed'], 1)
                self.assertEqual(rel['pe_max'], 301)
                self.assertEqual(rel['pending_risky_blocks'], 0)
                self.assertEqual(rel['pending_refresh_blocks'], 0)
                self.assertEqual(rel['inflight_refresh_blocks'], 0)
                self.assertEqual(host['aggregate']['all']['pending'], 0)
                # A metadata-only tail seal must not create read/write events
                # for pages that never held real data.
                writes = [e for e in events if e['event'] == 'write_completed']
                self.assertEqual(len(writes), live)
                self.assertTrue(all(int(e['source_page']) < live for e in writes))
                erase = [int(e['time_ns']) for e in events if e['event'] == 'erase_submitted']
                self.assertEqual(len(erase), 1)
                self.assertGreater(erase[0], max(int(e['time_ns']) for e in writes))

    def test_partial_frontier_with_concurrent_overwrites(self):
        rel, host, _, _ = self.run_case(overwrite=True)
        self.assertEqual(host['aggregate']['all']['pending'], 0)
        self.assertGreater(host['aggregate']['write']['completed'], 0)
        self.assertGreater(rel['refresh_write_pages_completed'], 0)
        self.assertEqual(rel['pending_risky_blocks'], 0)
        self.assertEqual(rel['pending_refresh_blocks'], 0)
        self.assertEqual(rel['inflight_refresh_blocks'], 0)

    def test_horizon_retains_partial_refresh_work_and_risk(self):
        horizon = 1_000_000
        rel, _, _, episodes = self.run_case(horizon=horizon)
        self.assertEqual(rel['partial_frontiers_retired'], 1)
        self.assertGreater(rel['refresh_read_pages_submitted'], 0)
        self.assertGreater(rel['refresh_migration_pages_outstanding'], 0)
        self.assertGreater(rel['pending_triggered_live_pages'], 0)
        self.assertEqual(rel['refresh_erase_completed'], 0)
        self.assertEqual(rel['pending_risky_blocks'], 1)
        censored = [e for e in episodes if e['censored_at_horizon'] == '1']
        self.assertEqual(len(censored), 1)
        self.assertEqual(int(censored[0]['observed_wait_ns']), horizon)

    def test_inflight_source_program_delays_frontier_retirement(self):
        # The only host request overwrites an initial page at t=0. The partial
        # source frontier receives that program before RT rises at 0.2 ms.
        short, host, events, episodes = self.run_case(
            horizon=400_000, thermal_trigger_during_program=True)
        self.assertEqual(host['aggregate']['write']['generated'], 1)
        self.assertEqual(host['aggregate']['write']['completed'], 0)
        self.assertEqual(host['aggregate']['write']['pending'], 1)
        self.assertEqual(short['foreground_program_pages_completed'], 0)
        self.assertEqual(short['partial_frontiers_retired'], 0)
        self.assertEqual(short['source_tail_sealed_pages'], 0)
        self.assertEqual(short['refresh_started'], 0)
        self.assertEqual(short['pending_risky_blocks'], 1)
        self.assertGreater(short['pending_triggered_live_pages'], 0)
        self.assertFalse(any(e['event'] == 'partial_frontier_retired' for e in events))
        censored = [e for e in episodes if e['censored_at_horizon'] == '1']
        self.assertEqual(len(censored), 1)
        self.assertEqual(int(censored[0]['first_observed_above_ns']), 200_000)
        self.assertEqual(int(censored[0]['observed_wait_ns']), 200_000)

        # Same request/temperature/SSD, longer horizon: the source becomes safe
        # after the host program completes, and refresh must then make progress.
        long, host, events, _ = self.run_case(thermal_trigger_during_program=True)
        self.assertEqual(host['aggregate']['write']['completed'], 1)
        self.assertEqual(host['aggregate']['all']['pending'], 0)
        self.assertEqual(long['foreground_program_pages_completed'], 1)
        self.assertEqual(long['partial_frontiers_retired'], 1)
        self.assertEqual(long['refresh_completed'], 1)
        self.assertEqual(long['refresh_erase_completed'], 1)
        self.assertEqual(long['pending_risky_blocks'], 0)
        self.assertEqual(long['pending_refresh_blocks'], 0)
        self.assertEqual(long['inflight_refresh_blocks'], 0)
        retired = [int(e['time_ns']) for e in events if e['event'] == 'partial_frontier_retired']
        self.assertEqual(len(retired), 1)
        self.assertGreater(retired[0], 400_000)
        self.assertGreaterEqual(retired[0], 800_000)
        self.assertGreaterEqual(retired[0], host['aggregate']['write']['latency_ns']['max'])


if __name__ == '__main__':
    unittest.main()
