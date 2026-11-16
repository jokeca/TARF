#!/usr/bin/env python3
"""Real NAND phase-boundary regressions, not scientific performance results."""
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'codes/tests'))
import support as b
BINARY = Path(os.environ.get('TARF_TEST_BINARY', ROOT / 'codes/MQSim')).resolve()


class MeasurementPhase(unittest.TestCase):
    def run_phase(self, folder, policy, late=False):
        folder.mkdir()
        geom = b.geometry('debug32m')
        config, workload, trace = (folder / f for f in ('ssd.xml', 'workload.xml', 'trace'))
        b.write_config(config, geom, 321)
        b.write_workload(workload, trace, geom)
        tree = ET.parse(config)
        tree.find('.//Preemptible_GC_Enabled').text = 'true'
        tree.write(config)
        start = 1_000_000 if late else 10_000_000_000
        horizon = 100_000_000 if late else 12_000_000_000
        if late:
            records = [(0, 0, 0, 128, 0), (start + 1, 0, 0, 16, 1)]
        else:
            records = [(i * 1_000_000, 0, (i * 64) % 16000, 64, 0) for i in range(1200)]
            records += [(start + 1 + i * 5_000_000, 0, (i * 16) % 16000, 16, 1) for i in range(200)]
        trace.write_text(''.join(' '.join(map(str, r)) + '\n' for r in records))
        env = {k: v for k, v in os.environ.items() if not k.startswith('TARF_')}
        env.update(TARF_POLICY=policy, TARF_OUTPUT_PREFIX=str(folder / 'result'), TARF_HORIZON_NS=str(horizon),
            TARF_MEASUREMENT_START_NS=str(start), TARF_SCAN_NS='3000000000', TARF_TRACE_NORMALIZED='1',
            TARF_INITIAL_PE='300', TARF_INITIAL_AGE_DAYS='365', TARF_INITIAL_TEMP_C='0',
            TARF_THRESHOLD='0.4', TARF_RISK_THRESHOLD='0.5', TARF_PERIOD_DAYS='30',
            TARF_RESOURCE_AWARE='1', TARF_GC_COORDINATION='1', TARF_REFRESH_MAX_DEFER_NS='100000000',
            TARF_REFRESH_MAX_PAGES_PER_CHIP='1' if policy == 'tarf' else '2')
        result = subprocess.run([str(BINARY), '-i', str(config), '-w', str(workload)],
                                cwd=folder, env=env, capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout[-3000:] + result.stderr)
        load = lambda name: json.loads((folder / f'result_{name}.json').read_text())
        boundary, host, reliability = load('measurement_start'), load('host'), load('reliability')
        self.assertEqual(boundary['time_ns'], start)
        self.assertEqual(boundary['refresh_started'], 0)
        self.assertEqual(boundary['refresh_write_pages_completed'], 0)
        with (folder / 'result_background_timeline.csv').open() as timeline:
            for t in csv.DictReader(timeline):
                if int(t['time_ns']) < start:
                    self.assertEqual(int(t['refresh_program_pages']), 0)
        for group in ('all', 'read', 'write'):
            parts = [host['phases'][p][group] for p in ('warmup', 'measurement')]
            for key in ('generated', 'completed', 'pending', 'generated_bytes', 'completed_bytes', 'pending_bytes'):
                self.assertEqual(sum(p[key] for p in parts), host['aggregate'][group][key])
            self.assertEqual(sum(p['latency_ns']['sum_ns'] for p in parts), host['aggregate'][group]['latency_ns']['sum_ns'])
        self.assertEqual(reliability['erased_live_pages'], 0)
        return boundary, host, reliability

    def test_common_physical_state_and_real_warmup(self):
        with tempfile.TemporaryDirectory(prefix='tarf_phase_') as tmp:
            hashes, boundaries = [], []
            for policy in ('none', 'periodic', 'age', 'tempdiff', 'tarf'):
                folder = Path(tmp) / policy
                boundary, host, reliability = self.run_phase(folder, policy)
                hashes.append(hashlib.sha256((folder / 'result_measurement_blocks.csv').read_bytes()).hexdigest())
                boundaries.append(boundary)
                self.assertGreater(boundary['foreground_program_pages_completed'], 0)
                self.assertGreater(boundary['gc_write_pages_completed'], 0)
                self.assertEqual(host['warmup_completions_after_boundary'], 0)
                self.assertEqual(host['phases']['warmup']['all']['pending'], 0)
                self.assertEqual(host['phases']['measurement']['all']['generated'], 200)
                if policy != 'none':
                    self.assertGreater(reliability['refresh_started'], 0)
            self.assertEqual(len(set(hashes)), 1)
            self.assertTrue(all(x == boundaries[0] for x in boundaries))

    def test_late_warmup_completion_stays_in_warmup_cohort(self):
        with tempfile.TemporaryDirectory(prefix='tarf_phase_late_') as tmp:
            _, host, _ = self.run_phase(Path(tmp) / 'none', 'none', late=True)
            self.assertEqual(host['warmup_completions_after_boundary'], 1)
            self.assertEqual(host['phases']['warmup']['all']['completed'], 1)
            self.assertEqual(host['phases']['measurement']['all']['generated'], 1)
            self.assertEqual(host['phases']['measurement']['write']['generated'], 0)


if __name__ == '__main__':
    unittest.main()
