#!/usr/bin/env python3
"""Run policy, scheduling-ablation, and trigger-threshold experiments on a supplied trace."""
import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import random
import shutil
import subprocess
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
NS = 1_000_000_000
POLICIES = ('none', 'periodic', 'age', 'tempdiff', 'tarf')


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for b in iter(lambda: f.read(1024 * 1024), b''):
            h.update(b)
    return h.hexdigest()


def dump(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')


def read_json(path):
    return json.loads(path.read_text())


def variants(study):
    selected = {}
    if study in ('main', 'all'):
        selected.update({p: {'TARF_POLICY': p} for p in POLICIES})
    else:
        selected['tarf'] = {'TARF_POLICY': 'tarf'}
    if study in ('ablation', 'all'):
        selected['no_scheduling'] = {'TARF_POLICY': 'tarf', 'TARF_RESOURCE_AWARE': '0'}
        selected['no_gc'] = {'TARF_POLICY': 'tarf', 'TARF_GC_COORDINATION': '0'}
    if study in ('threshold', 'all'):
        selected['theta8'] = {'TARF_POLICY': 'tarf', 'TARF_THRESHOLD': '8'}
        selected['theta12'] = {'TARF_POLICY': 'tarf', 'TARF_THRESHOLD': '12'}
    return selected


def geometry(tree):
    tags = ('Flash_Channel_Count', 'Chip_No_Per_Channel', 'Die_No_Per_Chip',
            'Plane_No_Per_Die', 'Block_No_Per_Plane', 'Page_No_Per_Block', 'Page_Capacity')
    dims = [int(tree.find('.//' + k).text) for k in tags]
    if any(x <= 0 for x in dims) or dims[-1] % 512:
        raise ValueError('Invalid NAND dimensions')
    planes = math.prod(dims[:4]); sectors_per_page = dims[-1] // 512
    user_sectors = planes * int(dims[4] * dims[5] * sectors_per_page *
                               (1 - float(tree.find('.//Overprovisioning_Ratio').text)))
    return dims, user_sectors


def prepare(args):
    if args.out.exists():
        raise ValueError('Choose a new --out directory for a new run')
    if not args.trace.is_file() or not args.ssd.is_file() or not args.model.is_file():
        raise ValueError('Trace, SSD configuration, and model files are required')
    tree = ET.parse(args.ssd)
    dims, user_sectors = geometry(tree)
    spp = dims[-1] // 512
    prefix = int((user_sectors // spp) * args.occupancy / 100)
    args.out.mkdir(parents=True)
    inputs = args.out / 'inputs'; inputs.mkdir()
    source_hash = digest(args.trace)
    normalized = inputs / 'normalized.trace'
    first = previous = None
    count = reads = read_bytes = write_bytes = last = 0
    needed, written = set(), {}
    tick = {'ns': 1, '100ns': 100, 'us': 1000}[args.source_unit]
    with args.trace.open() as source, normalized.open('w') as dest:
        for number, line in enumerate(source, 1):
            fields = line.split()
            if len(fields) != 5 or any(not x.isdigit() for x in fields):
                raise ValueError(f'Trace row {number}: expected five unsigned integers')
            stamp, device, lba, size, op = map(int, fields)
            if previous is not None and stamp < previous:
                raise ValueError('Trace timestamps must be nondecreasing')
            if op not in (0, 1) or not 1 <= size <= 65535 or lba + size > user_sectors:
                raise ValueError(f'Trace row {number}: operation, size, or address is invalid')
            if first is None: first = stamp
            previous = stamp
            last = (stamp - first) * tick // args.timestamp_divisor
            dest.write(f'{last} {device} {lba} {size} {op}\n')
            count += 1; reads += op
            if op: read_bytes += size * 512
            else: write_bytes += size * 512
            # Prefill read-before-write data beyond the initial occupied prefix.
            if lba + size > prefix * spp:
                for page in range(max(prefix, lba // spp), (lba + size + spp - 1) // spp):
                    begin = max(lba, page * spp) - page * spp
                    end = min(lba + size, (page + 1) * spp) - page * spp
                    mask = ((1 << (end - begin)) - 1) << begin
                    if op:
                        if mask & ~written.get(page, 0): needed.add(page)
                    else:
                        written[page] = written.get(page, 0) | mask
    if not count:
        raise ValueError('Trace is empty')
    duration = (last // NS + 1) * NS
    if args.duration_seconds is not None:
        duration = int(args.duration_seconds * NS)
        if duration <= last:
            raise ValueError('Explicit replay duration must exceed the last arrival')
    start = duration + int(args.settle_seconds * NS)
    horizon = start + 1 + duration + int(args.drain_seconds * NS)
    trace = inputs / 'replay.trace'
    with trace.open('w') as dest:
        for offset in (0, start + 1):
            with normalized.open() as source:
                for line in source:
                    stamp, rest = line.split(maxsplit=1)
                    dest.write(f'{int(stamp) + offset} {rest}')
    normalized.unlink()
    (inputs / 'coverage.csv').write_text('lpa\n' + ''.join(f'{i}\n' for i in sorted(needed)))
    shutil.copyfile(args.model, inputs / 'model.conf')
    if args.temperature_file:
        shutil.copyfile(args.temperature_file, inputs / 'temperature.csv')
    else:
        (inputs / 'temperature.csv').write_text('time_ns,temp_C\n0,55\n')
    workload = ET.Element('MQSim_IO_Scenarios'); scenario = ET.SubElement(workload, 'IO_Scenario')
    flow = ET.SubElement(scenario, 'IO_Flow_Parameter_Set_Trace_Based')
    values = dict(Priority_Class='HIGH', Device_Level_Data_Caching_Mode='TURNED_OFF',
                  Initial_Occupancy_Percentage=args.occupancy, File_Path='replay.trace',
                  Percentage_To_Be_Executed=100, Relay_Count=1, Total_requests_to_server=0,
                  Choose_type=0, Turn_on=0, Time_Unit='NANOSECOND')
    values.update({k: ','.join(map(str, range(n))) for k, n in zip(('Channel_IDs', 'Chip_IDs', 'Die_IDs', 'Plane_IDs'), dims)})
    for key, value in values.items(): ET.SubElement(flow, key).text = str(value)
    ET.ElementTree(workload).write(inputs / 'workload.xml', encoding='utf-8', xml_declaration=True)
    cases = []
    for seed in args.seeds:
        cfg = ET.parse(args.ssd); cfg.find('.//Seed').text = str(seed)
        cfg.write(inputs / f'ssd_{seed}.xml', encoding='utf-8', xml_declaration=True)
        initial = inputs / f'initial_{seed}.csv'
        if args.initial_state:
            shutil.copyfile(args.initial_state, initial)
        else:
            rng = random.Random(seed)
            with initial.open('w', newline='') as f:
                w = csv.writer(f); w.writerow(('block_index', 'pe', 'age_days', 'program_temp_c'))
                for i in range(math.prod(dims[:5])):
                    pe = rng.choices((300, 1000, 3000), weights=(.4, .3, .3))[0]
                    age = 0 if rng.random() < .7 else rng.uniform(182.5, 1095)
                    w.writerow((i, pe, age, rng.uniform(0, 85)))
        common = dict(TARF_HORIZON_NS=str(horizon), TARF_MEASUREMENT_START_NS=str(start),
                      TARF_SCAN_NS=str(10 * NS), TARF_TRACE_NORMALIZED='1',
                      TARF_INITIAL_PE='300', TARF_INITIAL_AGE_DAYS='0', TARF_INITIAL_TEMP_C='55',
                      TARF_INITIAL_STATE_FILE=f'inputs/initial_{seed}.csv',
                      TARF_INITIAL_READ_COVERAGE_FILE='inputs/coverage.csv', TARF_REJECT_LAZY_INITIAL_READS='1',
                      TARF_TEMPERATURE_FILE='inputs/temperature.csv', TARF_MODEL_FILE='inputs/model.conf',
                      TARF_THRESHOLD='10', TARF_RISK_THRESHOLD='10', TARF_PERIOD_DAYS='182.5',
                      TARF_RESOURCE_AWARE='1', TARF_RESOURCE_AWARE_BASELINES='0',
                      TARF_SELECTION_RANK='benefit_cost', TARF_GC_COORDINATION='1',
                      TARF_GC_RESPECT_CONFIGURED_THRESHOLD='1', TARF_REFRESH_MAX_BLOCKS_PER_CHIP='1',
                      TARF_REFRESH_MAX_DEFER_NS='100000000', TARF_REFRESH_ADMISSION_MAX_DEFER_NS='100000000',
                      TARF_REFRESH_QUIET_NS='0')
        for variant, changes in variants(args.study).items():
            env = dict(common, **changes)
            if env['TARF_POLICY'] == 'tarf':
                env.update(TARF_REFRESH_MAX_PAGES_PER_CHIP='1', TARF_REFRESH_RESERVE_ESCAPE_NS=str(600 * NS))
            else:
                env['TARF_REFRESH_MAX_PAGES_PER_CHIP'] = '2'
            cases.append(dict(id=f'seed{seed}_{variant}', seed=seed, variant=variant, environment=env,
                              ssd=f'inputs/ssd_{seed}.xml', workload='inputs/workload.xml'))
    manifest = dict(study=args.study, cases=cases, source_trace_sha256=source_hash,
                    source_time_unit=args.source_unit, timestamp_divisor=args.timestamp_divisor,
                    replay_duration_ns=duration, measurement_start_ns=start, horizon_ns=horizon,
                    retention_seconds_per_day=86400, requests_per_phase=count,
                    reads_per_phase=reads, read_bytes_per_phase=read_bytes, write_bytes_per_phase=write_bytes,
                    page_bytes=dims[-1], initial_occupancy_percent=args.occupancy,
                    additional_read_coverage_pages=len(needed),
                    input_hashes={p.relative_to(args.out).as_posix(): digest(p) for p in inputs.iterdir()})
    dump(args.out / 'manifest.json', manifest)
    return manifest


def summarize(case, folder, manifest):
    host, rel, boundary = [read_json(folder / f'result_{k}.json') for k in ('host', 'reliability', 'measurement_start')]
    start = manifest['measurement_start_ns']; duration = (manifest['horizon_ns'] - start) / NS
    if boundary['time_ns'] != start or rel['actual_sim_time_ns'] != manifest['horizon_ns']:
        raise ValueError('Simulation endpoint or measurement boundary differs')
    h = host['phases']['measurement']['all']; warm = host['phases']['warmup']['all']
    row = dict(seed=case['seed'], variant=case['variant'], policy=rel['policy'],
               trigger_threshold=rel['threshold'], risk_threshold=rel['risk_threshold'], status='complete')
    if h['generated'] != manifest['requests_per_phase'] or warm['generated'] != h['generated']:
        raise ValueError('Trace replay count differs')
    assert h['generated_bytes'] == warm['generated_bytes'] == manifest['read_bytes_per_phase'] + manifest['write_bytes_per_phase']
    assert h['latency_ns']['count'] == h['completed']
    assert boundary['refresh_started'] == boundary['refresh_write_pages_completed'] == 0
    assert rel['erased_live_pages'] == rel['lazy_initial_pages'] == 0
    row.update(requests_generated=h['generated'], requests_completed=h['completed'], requests_pending=h['pending'],
               bytes_generated=h['generated_bytes'], bytes_completed=h['completed_bytes'], bytes_pending=h['pending_bytes'],
               all_requests_complete=all(q['generated'] == q['completed'] and q['generated_bytes'] == q['completed_bytes']
                                         and q['pending'] == q['pending_bytes'] == 0 for q in (h, warm)),
               warmup_drained=host['warmup_completions_after_boundary'] == boundary['ongoing_gc_blocks'] == warm['pending'] == 0,
               mean_response_ms=None if h['latency_ns']['mean'] is None else h['latency_ns']['mean'] / 1e6,
               p99_response_ms=None if h['latency_ns']['p99'] is None else h['latency_ns']['p99'] / 1e6,
               refresh_program_bytes=rel['refresh_program_bytes_completed'],
               gc_program_bytes=rel['gc_program_bytes_completed'] - boundary['gc_program_bytes_completed'],
               nfbc_exposure=(rel['risk_exposure_integral_seconds'] - boundary['risk_exposure_integral_seconds']) / duration,
               risky_block_seconds=rel['risky_block_seconds'] - boundary['risky_block_seconds'])
    row['migration_bytes'] = row['refresh_program_bytes'] + row['gc_program_bytes']
    waits = []
    with (folder / 'result_risk_episodes.csv').open() as f:
        for e in csv.DictReader(f):
            waits.append(max(0, int(e['end_ns']) - max(start, int(e['first_observed_above_ns']))) / NS)
    assert math.isclose(sum(waits), row['risky_block_seconds'], rel_tol=1e-7, abs_tol=1e-5)
    row['max_threshold_wait_seconds'] = max(waits, default=0)
    for key in ('pending_risky_blocks', 'pending_refresh_blocks', 'pending_triggered_live_pages',
                'refresh_migration_pages_outstanding', 'inflight_refresh_blocks'):
        row[key] = rel[key]
    row['zero_end_backlog'] = all(row[key] == 0 for key in
                                  ('pending_risky_blocks', 'pending_refresh_blocks', 'pending_triggered_live_pages',
                                   'refresh_migration_pages_outstanding', 'inflight_refresh_blocks'))
    if not row['all_requests_complete'] or not row['warmup_drained']:
        row['status'] = 'incomplete'
    row['boundary_state_sha256'] = digest(folder / 'result_measurement_blocks.csv')
    row['boundary_counters_sha256'] = digest(folder / 'result_measurement_start.json')
    return row


def check_risk_budgets(rows):
    references = {r['seed']: r for r in rows if r['variant'] == 'tempdiff' and 'nfbc_exposure' in r}
    for row in rows:
        if row['policy'] != 'tarf' or 'nfbc_exposure' not in row:
            continue
        ref = references.get(row['seed'])
        row['risk_budget_pass'] = None
        row['accepted'] = None
        if ref is None:
            continue
        failed = []
        for key in ('nfbc_exposure', 'risky_block_seconds'):
            if row[key] > ref[key] * 1.05 + 1e-12:
                failed.append(key)
        for key in ('pending_risky_blocks', 'pending_triggered_live_pages', 'refresh_migration_pages_outstanding',
                    'inflight_refresh_blocks', 'requests_pending', 'bytes_pending'):
            if row[key] > ref[key]:
                failed.append(key)
        if row['max_threshold_wait_seconds'] > ref['max_threshold_wait_seconds'] * 1.05 + 10:
            failed.append('max_threshold_wait_seconds')
        row['risk_budget_pass'] = not failed
        row['failed_risk_gates'] = ';'.join(failed)
        row['accepted'] = row['all_requests_complete'] and row['warmup_drained'] and row['zero_end_backlog'] and not failed


def run(args, manifest):
    for name, expected in manifest['input_hashes'].items():
        if digest(args.out / name) != expected: raise ValueError('Prepared input changed: ' + name)
    if not args.binary.is_file(): raise ValueError('Simulator binary missing; run make first')
    binary_hash = digest(args.binary)
    rows, states = [], {}
    for case in manifest['cases']:
        folder = args.out / case['id']; folder.mkdir()
        env = {k: v for k, v in os.environ.items() if not k.startswith('TARF_')}
        env.update(case['environment'])
        for key in ('TARF_INITIAL_STATE_FILE', 'TARF_INITIAL_READ_COVERAGE_FILE', 'TARF_TEMPERATURE_FILE', 'TARF_MODEL_FILE'):
            env[key] = str(args.out / env[key])
        env['TARF_OUTPUT_PREFIX'] = str(folder / 'result')
        command = [str(args.binary), '-i', str(args.out / case['ssd']), '-w', str(args.out / case['workload'])]
        began = time.monotonic()
        with (folder / 'simulator.log').open('w') as f:
            try:
                result = subprocess.run(command, cwd=folder, env=env, stdout=f, stderr=subprocess.STDOUT,
                                        stdin=subprocess.DEVNULL, timeout=args.timeout)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 124
        dump(folder / 'execution.json', dict(returncode=code, wall_seconds=time.monotonic() - began, binary_sha256=binary_hash))
        if code:
            row = dict(seed=case['seed'], variant=case['variant'], policy=case['environment']['TARF_POLICY'], status='failed')
        else:
            row = summarize(case, folder, manifest)
            state = row['boundary_state_sha256'], row['boundary_counters_sha256']
            if case['seed'] in states and states[case['seed']] != state:
                raise ValueError('Policies reached different measurement-boundary states')
            states[case['seed']] = state
        rows.append(row)
        check_risk_budgets(rows)
        with (args.out / 'results.csv').open('w', newline='') as f:
            w = csv.DictWriter(f, fieldnames=list(dict.fromkeys(k for r in rows for k in r)))
            w.writeheader(); w.writerows(rows)
        print(case['id'], row['status'], flush=True)
    if any(r['status'] != 'complete' for r in rows):
        raise SystemExit('One or more simulations failed or left foreground work unfinished; see results.csv and execution.json')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--trace', type=Path, required=True, help='timestamp device LBA sector_count op; 0=write, 1=read')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--study', choices=['main', 'ablation', 'threshold', 'all'], default='all')
    p.add_argument('--seeds', type=int, nargs='+', default=[321, 2027, 4099])
    p.add_argument('--ssd', type=Path, default=ROOT / 'codes/ssd.xml')
    p.add_argument('--model', type=Path, default=ROOT / 'model_parameters/model.conf')
    p.add_argument('--binary', type=Path, default=ROOT / 'codes/MQSim')
    p.add_argument('--initial-state', type=Path)
    p.add_argument('--temperature-file', type=Path)
    p.add_argument('--source-unit', choices=['ns', '100ns', 'us'], default='ns')
    p.add_argument('--timestamp-divisor', type=int, default=1)
    p.add_argument('--duration-seconds', type=float)
    p.add_argument('--settle-seconds', type=float, default=600)
    p.add_argument('--drain-seconds', type=float, default=1800)
    p.add_argument('--occupancy', type=float, default=90)
    p.add_argument('--timeout', type=int, default=21600, help='wall-clock timeout per process, in seconds')
    p.add_argument('--prepare-only', action='store_true')
    args = p.parse_args()
    if (args.timestamp_divisor < 1 or args.timeout <= 0 or len(set(args.seeds)) != len(args.seeds)
            or any(s < 0 or s >= 2**32 for s in args.seeds) or not 0 <= args.occupancy <= 100
            or not all(math.isfinite(v) and v >= 0 for v in (args.settle_seconds, args.drain_seconds))
            or args.duration_seconds is not None and (not math.isfinite(args.duration_seconds) or args.duration_seconds <= 0)):
        p.error('Invalid seed, time, occupancy, divisor, or timeout')
    args.out = args.out.resolve(); args.binary = args.binary.resolve()
    manifest = prepare(args)
    print('Prepared', len(manifest['cases']), 'runs with', manifest['requests_per_phase'], 'requests per replay.', flush=True)
    if not args.prepare_only: run(args, manifest)
