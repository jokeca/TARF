#!/usr/bin/env python3
"""Refit the temperature/wear model and retention model from the included CSV."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path

from calibrate_micron import fit_cross, export_model
from fit_retention import fit_retention

ROOT = Path(__file__).resolve().parents[1]


def fit(data, out):
    cross, retention = [], []
    with data.open() as f:
        for source in csv.DictReader(f):
            row = dict(chip={'Chip1': 'R', 'Chip2': 'M'}[source['chip']],
                       read_mode=source['read_mode'], pe=int(source['pe']),
                       fbc=float(source['fbc']), nfbc=float(source['nfbc']))
            divisor = float(source['normalization_divisor'])
            if not math.isfinite(row['fbc']) or row['fbc'] <= 0 or divisor != 62:
                raise ValueError('Require positive finite FBC and normalization divisor 62')
            if not math.isclose(row['nfbc'], row['fbc'] / divisor, rel_tol=1e-12):
                raise ValueError('NFBC must equal FBC / normalization divisor')
            if source['measurement'] == 'cross_temperature':
                row.update(program_c=float(source['program_temp_c']), read_c=float(source['read_temp_c']))
                if row['read_mode'] == 'default':
                    cross.append(row)
            elif source['measurement'] == 'retention':
                row['age_days'] = float(source['retention_age_days'])
                retention.append(row)
            else:
                raise ValueError('Unknown measurement family')
    full, _ = fit_cross(cross, 'full')
    aging = fit_retention(retention)
    selected = aging['selected_default']
    out.mkdir(parents=True, exist_ok=True)
    export_model(out / 'model.conf', full['coefficients'], selected,
                 hashlib.sha256(data.read_bytes()).hexdigest(), fit_count=len(cross), fit_scope='pooled-RM-default')
    model = out / 'model.conf'
    model.write_text(model.read_text().replace('Micron-workbook-', 'chip-measurements-csv-'))
    rows = []
    for symbol, key in [('beta_0', 'beta0'), ('beta_1', 'beta_pe'), ('beta_2', 'beta_diff'), ('beta_3', 'beta_common')]:
        rows.append(dict(equation=3, symbol=symbol, code_key=key, chip='shared',
                         value=format(full['coefficients'][key], '.17g'), unit='dimensionless'))
    for chip, value in [('Chip1', 0), ('Chip2', full['coefficients']['M_bias'])]:
        rows.append(dict(equation=3, symbol='b_c', code_key='chip_bias', chip=chip,
                         value=format(value, '.17g'), unit='dimensionless'))
    for symbol, key, unit in [('t_0', 't0days', 'days'), ('delta', 'delta', 'dimensionless'), ('gamma', 'gamma', 'dimensionless')]:
        rows.append(dict(equation=5, symbol=symbol, code_key=key, chip='Chip1',
                         value=format(selected[key], '.17g'), unit=unit))
    with (out / 'coefficients.csv').open('w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
    report = dict(cross_temperature_conditions=len(cross), temperature_fit=full,
                  retention_fit=aging, retention_time_unit='days')
    (out / 'fit.json').write_text(json.dumps(report, indent=2, allow_nan=False) + '\n')
    print('Fitted', len(cross), 'cross-temperature conditions; exported model.conf and coefficients.csv.')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--data', type=Path, default=ROOT / 'data/chip_measurements.csv')
    p.add_argument('--out', type=Path, default=ROOT / 'outputs/fit')
    args = p.parse_args()
    fit(args.data, args.out)
