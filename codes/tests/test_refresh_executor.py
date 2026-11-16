#!/usr/bin/env python3
"""Real-binary checks of bounded page refresh, progress and NAND causality.

Tiny synthetic cases verify execution contracts, not NAND-model accuracy or
publication performance. No large traces or accelerated retention clock are used.
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
SPEC = importlib.util.spec_from_file_location("tarf_experiments", ROOT / "codes/tests/support.py")
EXPERIMENTS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXPERIMENTS)
BINARY = Path(os.environ.get("TARF_TEST_BINARY", ROOT / "codes/MQSim")).resolve()


class RefreshExecutorTests(unittest.TestCase):
    def run_case(self, policy="tarf", window=2, busy=True, horizon=2_000_000_000, extra_environment=None, coverage=False):
        temporary = tempfile.TemporaryDirectory(prefix="tarf_refresh_executor_")
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        config, workload, trace = [directory / name for name in ("ssd.xml", "workload.xml", "requests.trace")]
        geometry = EXPERIMENTS.geometry("debug32m")
        EXPERIMENTS.write_config(config, geometry, 321)
        EXPERIMENTS.write_workload(workload, trace, geometry)
        tree = ET.parse(config)
        for key, value in {"Preemptible_GC_Enabled": "true", "IO_Queue_Depth": "4096", "Queue_Fetch_Size": "64"}.items():
            tree.find(".//" + key).text = value
        tree.write(config, encoding="utf-8", xml_declaration=True)
        # Sustained arrival pressure includes reads to the initial data and
        # overwrites of pages whose block may be awaiting refresh migration.
        count, spacing = (20000, 50000) if busy else (100, 2000000)
        trace.write_text("".join(f"{i * spacing} 0 {(i * 37 % 2500) * 16} 16 {1 if i % 3 else 0}\n" for i in range(count)))
        if coverage:
            tree = ET.parse(workload)
            tree.find('.//Initial_Occupancy_Percentage').text = '0'
            tree.write(workload, encoding='utf-8', xml_declaration=True)
            trace.write_text('0 0 48000 16 1\n1000000 0 48016 16 1\n')
            (directory/'coverage.csv').write_text('lpa\n3000\n3001\n')
        temperature = directory / "temperature.csv"
        temperature.write_text("time_ns,temp_C\n0,70\n")
        prefix = directory / "result"
        environment = {key: value for key, value in os.environ.items() if not key.startswith("TARF_")}
        environment.update(TARF_POLICY=policy, TARF_HORIZON_NS=str(horizon), TARF_SCAN_NS="10000000",
                           TARF_OUTPUT_PREFIX=str(prefix), TARF_TRACE_NORMALIZED="1", TARF_INITIAL_PE="300",
                           TARF_INITIAL_AGE_DAYS="100", TARF_INITIAL_TEMP_C="30", TARF_THRESHOLD="0.4",
                           TARF_RISK_THRESHOLD="0.5", TARF_PERIOD_DAYS="30", TARF_TEMPERATURE_FILE=str(temperature),
                           TARF_REFRESH_MAX_PAGES_PER_CHIP=str(window), TARF_REFRESH_MAX_BLOCKS_PER_CHIP="1",
                           TARF_REFRESH_MAX_DEFER_NS="100000000")
        environment.update(extra_environment or {})
        if coverage:
            environment.update(TARF_INITIAL_READ_COVERAGE_FILE=str(directory/'coverage.csv'),
                               TARF_REJECT_LAZY_INITIAL_READS='1')
        result = subprocess.run([str(BINARY), "-i", str(config), "-w", str(workload)], cwd=directory,
                                env=environment, stdin=subprocess.DEVNULL, capture_output=True,
                                text=True, errors="replace", timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout[-5000:] + result.stderr)
        reliability = json.loads(Path(str(prefix) + "_reliability.json").read_text())
        host = json.loads(Path(str(prefix) + "_host.json").read_text())
        with Path(str(prefix) + "_refresh_events.csv").open() as stream:
            events = list(csv.DictReader(stream))
        self.assertEqual(reliability["erased_live_pages"], 0)
        self.assertEqual(reliability["stale_program_completions"], 0)
        self.assertLessEqual(reliability["refresh_migration_pages_per_chip_peak"], window)
        self.assertEqual(reliability["refresh_read_pages_submitted"] - reliability["refresh_write_pages_completed"],
                         reliability["refresh_migration_pages_outstanding"])
        self.assertEqual(reliability["refresh_started"], reliability["refresh_completed"] + reliability["inflight_refresh_blocks"])
        self.assertEqual(reliability["refresh_read_bytes_completed"], reliability["refresh_read_pages_completed"] * 8192)
        self.assertEqual(reliability["refresh_program_bytes_submitted"], reliability["refresh_write_pages_submitted"] * 8192)
        self.assertEqual(reliability["refresh_program_bytes_completed"], reliability["refresh_write_pages_completed"] * 8192)
        self.assertEqual(reliability["all_read_bytes_completed"], sum(reliability[key] for key in
                         ("foreground_read_bytes_completed", "mapping_read_bytes_completed",
                          "refresh_read_bytes_completed", "gc_read_bytes_completed")))
        self.assertEqual(reliability["all_program_bytes"], sum(reliability[key] for key in
                         ("foreground_program_bytes_completed", "mapping_program_bytes_completed",
                          "refresh_program_bytes_completed", "gc_program_bytes_completed")))
        return reliability, host, events

    def test_busy_foreground_and_refresh_both_progress_with_bounded_pages(self):
        reliability, host, _ = self.run_case()
        self.assertGreater(reliability["refresh_write_pages_completed"], 0)
        self.assertGreater(host["aggregate"]["all"]["completed"], 0)
        self.assertGreater(reliability["pending_triggered_live_pages"], 0)
        self.assertGreater(reliability["refresh_forced_progress_admissions"], 0)

    def test_policies_share_executor_window(self):
        for policy in ("periodic", "tempdiff", "tarf"):
            with self.subTest(policy=policy):
                reliability, _, _ = self.run_case(policy=policy, window=1)
                self.assertEqual(reliability["refresh_max_pages_per_chip"], 1)
                self.assertEqual(reliability["refresh_max_blocks_per_chip"], 1)
                self.assertGreater(reliability["refresh_write_pages_completed"], 0)

    def test_real_reads_precede_programs_and_erase_follows_all_programs(self):
        reliability, _, events = self.run_case(policy="periodic", busy=False)
        self.assertGreater(reliability["refresh_erase_completed"], 0)
        reads, writes, starts = {}, {}, {}
        for event in events:
            block = tuple(event[key] for key in ("channel", "chip", "die", "plane", "source_block"))
            page = block + (event["source_page"], event["destination_block"], event["destination_page"])
            now = int(event["time_ns"])
            if event["event"] == "block_started":
                starts[block] = now
            elif event["event"] == "read_completed":
                reads[page] = now
            elif event["event"] == "write_completed":
                self.assertIn(page, reads)
                self.assertGreater(now, reads[page])
                writes[page] = now
            elif event["event"] == "erase_submitted":
                block_reads = {key for key in reads if key[:5] == block and reads[key] >= starts[block]}
                self.assertTrue(block_reads)
                for key in block_reads:
                    self.assertIn(key, writes)
                    self.assertLess(writes[key], now)

    def test_cutoff_keeps_inflight_work_and_page_barriers(self):
        reliability, _, _ = self.run_case(policy="periodic", horizon=1_000_000)
        self.assertGreater(reliability["refresh_read_pages_submitted"], 0)
        self.assertGreater(reliability["refresh_migration_pages_outstanding"], 0)
        self.assertEqual(reliability["refresh_page_barriers_current"], reliability["refresh_migration_pages_outstanding"])
        self.assertGreater(reliability["pending_triggered_live_pages"], 0)

    def test_quiet_window_waits_but_cannot_starve_refresh(self):
        for policy in ('tarf','tempdiff'):
            with self.subTest(policy=policy):
                reliability,host,events=self.run_case(policy=policy,extra_environment={
                    'TARF_REFRESH_QUIET_NS':'5000000','TARF_RESOURCE_AWARE_BASELINES':'1'})
                self.assertGreater(reliability['refresh_quiet_deferred_attempts'],0)
                self.assertGreater(reliability['refresh_write_pages_completed'],0)
                self.assertGreater(host['aggregate']['all']['completed'],0)
                self.assertGreater(reliability['refresh_forced_progress_admissions'],0)
                first=min(int(e['time_ns']) for e in events if e['event']=='read_submitted')
                self.assertGreaterEqual(first,5_000_000)

    def test_admission_timeout_does_not_change_shared_background_deadline(self):
        # Keep the chip unable to qualify as quiet; admission must eventually
        # progress at its own bound without rewriting the TSU/GC bound.
        reliability, host, events = self.run_case(extra_environment={
            'TARF_REFRESH_QUIET_NS': '10000000000',
            'TARF_REFRESH_ADMISSION_MAX_DEFER_NS': '20000000',
            'TARF_REFRESH_MAX_DEFER_NS': '100000000'})
        self.assertEqual(reliability['refresh_admission_max_defer_ns'],20_000_000)
        self.assertEqual(reliability['refresh_max_defer_ns'],100_000_000)
        first=min(int(e['time_ns']) for e in events if e['event']=='read_submitted')
        self.assertGreaterEqual(first,20_000_000)
        self.assertLess(first,100_000_000)
        self.assertGreater(reliability['refresh_write_pages_completed'],0)
        self.assertGreater(host['aggregate']['all']['completed'],0)

    def test_configured_gc_reserve_caps_concurrency_instead_of_inflating_threshold(self):
        legacy, _, _ = self.run_case(extra_environment={'TARF_GC_RESPECT_CONFIGURED_THRESHOLD':'0'})
        corrected, _, _ = self.run_case(extra_environment={'TARF_GC_RESPECT_CONFIGURED_THRESHOLD':'1'})
        self.assertEqual(legacy['gc_effective_soft_blocks'],10)
        # debug geometry has64 blocks/plane; floor(64 *0.15)=9, below
        # the inherited ten-slot concurrency. The requested reserve wins.
        self.assertEqual(corrected['gc_effective_soft_blocks'],9)
        self.assertEqual(corrected['gc_effective_max_concurrent_per_plane'],9)
        self.assertGreater(corrected['refresh_write_pages_completed'],0)

    def test_read_coverage_is_preinitialized_at_zero_without_host_programs(self):
        reliability, host, _ = self.run_case(policy='none', coverage=True)
        self.assertEqual(reliability['initial_live_pages'], 2)
        self.assertEqual(reliability['lazy_initial_pages'], 0)
        self.assertEqual(reliability['foreground_program_bytes_completed'], 0)
        self.assertEqual(host['aggregate']['all']['completed'], 2)


if __name__ == "__main__":
    unittest.main()
