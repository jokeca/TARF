#!/usr/bin/env python3
"""NAND integration regression for the soft-reserve admission dead zone."""
import csv
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'codes/tests'))
import support as b
BINARY=Path(os.environ.get('TARF_TEST_BINARY',ROOT/'codes/MQSim'))


class ReserveProgress(unittest.TestCase):
    def run_case(self,occupancy=90,escape=0,policy='tarf'):
        tmp=tempfile.TemporaryDirectory(prefix='tarf_reserve_');self.addCleanup(tmp.cleanup)
        p=Path(tmp.name);g=b.geometry('debug32m')
        b.write_config(p/'ssd.xml',g,321);b.write_workload(p/'workload.xml',p/'trace',g)
        tree=ET.parse(p/'ssd.xml');tree.find('.//Preemptible_GC_Enabled').text='true'
        tree.find('.//GC_Exec_Threshold').text='0.125';tree.write(p/'ssd.xml')
        tree=ET.parse(p/'workload.xml');tree.find('.//Initial_Occupancy_Percentage').text=str(occupancy);tree.write(p/'workload.xml')
        (p/'trace').write_text('1000000000 0 0 16 1\n')
        env={k:v for k,v in os.environ.items() if not k.startswith('TARF_')}
        env.update(TARF_POLICY=policy,TARF_HORIZON_NS='10000000000',TARF_SCAN_NS='10000000',
            TARF_INITIAL_PE='300',TARF_INITIAL_AGE_DAYS='365',TARF_INITIAL_TEMP_C='0',TARF_TRACE_NORMALIZED='1',
            TARF_THRESHOLD='0.4',TARF_RISK_THRESHOLD='0.4',TARF_GC_RESPECT_CONFIGURED_THRESHOLD='1',
            TARF_REFRESH_RESERVE_ESCAPE_NS=str(escape),TARF_RESOURCE_AWARE='0',TARF_OUTPUT_PREFIX=str(p/'result'))
        proc=subprocess.run([str(BINARY),'-i',str(p/'ssd.xml'),'-w',str(p/'workload.xml')],cwd=p,env=env,capture_output=True,timeout=30)
        self.assertEqual(proc.returncode,0,proc.stdout[-1500:]+proc.stderr[-1500:])
        rel=json.loads((p/'result_reliability.json').read_text());host=json.loads((p/'result_host.json').read_text())
        with (p/'result_refresh_events.csv').open() as f:events=list(csv.DictReader(f))
        with (p/'result_migrations.csv').open() as f:migrations=list(csv.DictReader(f))
        self.assertEqual(host['aggregate']['all']['pending'],0)
        self.assertEqual(rel['erased_live_pages'],0);self.assertEqual(rel['stale_program_completions'],0)
        self.assertEqual(rel['refresh_read_pages_submitted'],rel['refresh_write_pages_completed']+rel['refresh_migration_pages_outstanding'])
        self.assertEqual(rel['refresh_started'],rel['refresh_completed']+rel['inflight_refresh_blocks'])
        for row in migrations:
            self.assertGreaterEqual(int(row['destination_pe']),300)
            self.assertEqual(float(row['initial_age_days']),0)
            self.assertEqual(row['program_ns'],row['completion_ns'])
        return rel,events,host

    def test_closed_sources_progress_but_partial_reservation_remains_protected(self):
        blocked,_,_=self.run_case()
        progress,events,_=self.run_case(escape=60_000_000)
        self.assertEqual(blocked['refresh_started'],0)
        self.assertGreater(blocked['pending_risky_blocks'],50)
        self.assertEqual(progress['refresh_completed'],53)
        self.assertEqual(progress['pending_risky_blocks'],1) # partial frontier needs an extra reserve
        self.assertEqual(progress['refresh_reserve_escape_admissions'],53)
        self.assertEqual(progress['refresh_write_pages_completed'],53*64)
        for e in events:
            if e['event']=='soft_reserve_escape':self.assertGreaterEqual(int(e['time_ns']),60_000_000)

    def test_never_borrow_below_the_soft_boundary(self):
        r,_,_=self.run_case(occupancy=92,escape=60_000_000)
        self.assertEqual(r['refresh_reserve_escape_admissions'],0)
        self.assertEqual(r['refresh_started'],0)

    def test_baseline_policy_is_unchanged(self):
        off,events_off,host_off=self.run_case(policy='tempdiff')
        on,events_on,host_on=self.run_case(policy='tempdiff',escape=60_000_000)
        off.pop('refresh_reserve_escape_ns');on.pop('refresh_reserve_escape_ns')
        self.assertEqual(off,on);self.assertEqual(events_off,events_on)
        host_off.pop('flows');host_on.pop('flows') # temporary input path differs
        self.assertEqual(host_off,host_on)


if __name__=='__main__':unittest.main()
