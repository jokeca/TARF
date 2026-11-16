#!/usr/bin/python3
"""Fit the supplied aggregate Micron workbook, preserving source cells and gaps.

No observations are synthesized; missing cells are exported as missing records.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import sys
from zipfile import ZipFile
from xml.etree import ElementTree as ET

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
NS = {'x': 'http://schemas.openxmlformats.org/spreadsheetml/2006/main',
      'r': 'http://schemas.openxmlformats.org/officeDocument/2006/relationships'}


def dump(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + '\n')


def write_csv(path, records):
    columns = list(dict.fromkeys(k for row in records for k in row))
    with Path(path).open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(records)


def read_workbook(path):
    book = {}
    with ZipFile(path) as archive:
        strings = []
        if 'xl/sharedStrings.xml' in archive.namelist():
            strings = [''.join(t.text or '' for t in si.findall('.//x:t', NS))
                       for si in ET.fromstring(archive.read('xl/sharedStrings.xml'))]
        relationships = {r.attrib['Id']: r.attrib['Target'] for r in
                         ET.fromstring(archive.read('xl/_rels/workbook.xml.rels'))}
        for sheet in ET.fromstring(archive.read('xl/workbook.xml')).find('x:sheets', NS):
            target = relationships[sheet.attrib['{' + NS['r'] + '}id']]
            target = target.lstrip('/') if target.startswith('/') else 'xl/' + target
            cells = {}
            for cell in ET.fromstring(archive.read(target)).findall('.//x:sheetData/x:row/x:c', NS):
                if cell.find('x:f', NS) is not None:
                    raise ValueError('Formula cell requires evaluated source: ' + sheet.attrib['name'] + '!' + cell.attrib['r'])
                value = cell.find('x:v', NS)
                if cell.get('t') == 'inlineStr':
                    text = ''.join(t.text or '' for t in cell.findall('.//x:t', NS))
                elif cell.get('t') == 's':
                    text = strings[int(value.text)]
                else:
                    text = value.text if value is not None else ''
                cells[cell.attrib['r']] = text
            book[sheet.attrib['name']] = cells
    return book


def parse_pe(value):
    value = value.upper().replace(' ', '').replace('PE', '')
    if value.endswith('K'):
        return int(float(value[:-1]) * 1000)
    return int(value)


def parse_temp(value):
    match = re.fullmatch(r'\s*(-?\d+(?:\.\d+)?)\s*°?\s*C\s*', value, re.IGNORECASE)
    if not match:
        raise ValueError('Temperature must explicitly use Celsius: ' + value)
    return float(match.group(1))


def _axis_header(value):
    return re.sub(r'[\s\\/]+', '/', value.strip().upper())


def _positive_fbc(value, source_cell):
    result = float(value)
    if not math.isfinite(result) or result <= 0:
        raise ValueError('Require finite positive FBC at ' + source_cell)
    return result


def _cross_header(cells, sheet, header, chip, pe, mode):
    context = sheet + '!A' + str(header - 1)
    title = cells.get('A' + str(header - 1), '')
    product = re.search(r'\bB47R_([RM])\b', title)
    wear = re.search(r'\(\s*PE\s*([0-9]+(?:\.[0-9]+)?[Kk]?)\s*\)', title)
    actual_mode = ('best_retry' if 'BestRetry' in title else
                   'default' if 'Default' in title else None)
    if (not product or product.group(1) != chip or 'TLC' not in title
            or 'Cross Temp' not in title or not wear or parse_pe(wear.group(1)) != pe
            or actual_mode != mode or ('Default' in title and 'BestRetry' in title)):
        raise ValueError('Cross-temperature product/PE/read-mode title changed at ' + context)
    if _axis_header(cells.get('A' + str(header), '')) != 'PT/RT':
        raise ValueError('Expected PT rows / RT columns at ' + sheet + '!A' + str(header))
    expected_grid = [-25., 0., 55., 85.] if chip == 'R' else [-25., 0., 25., 85.]
    actual_rows = [parse_temp(cells['A' + str(row)]) for row in range(header + 1, header + 5)]
    actual_columns = [parse_temp(cells[col + str(header)]) for col in 'BCDE']
    if actual_rows != expected_grid or actual_columns != expected_grid:
        raise ValueError('Cross-temperature Celsius grid changed at ' + context)


def _retention_header(cells, sheet, header, chip, mode):
    product = re.search(r'\bB47R_([RM])\b', cells.get('A1', ''))
    title = cells.get('A' + str(header - 1), '')
    actual_mode = ('best_retry' if 'BestRetry' in title else
                   'default' if 'Default' in title else None)
    if (not product or product.group(1) != chip or 'TLC' not in cells.get('A1', '')
            or actual_mode != mode or ('Default' in title and 'BestRetry' in title)):
        raise ValueError('Retention product/read-mode title changed at ' + sheet + '!A' + str(header - 1))
    if _axis_header(cells.get('A' + str(header), '')) != 'PE/DR':
        raise ValueError('Expected PE rows / DR columns at ' + sheet + '!A' + str(header))
    if [cells[col + str(header)] for col in 'BCDE'] != ['0H', '6M', '1Y', '3Y']:
        raise ValueError('Retention age labels changed at ' + sheet + '!' + str(header))
    if [parse_pe(cells['A' + str(row)]) for row in range(header + 1, header + 5)] != [300, 1000, 3000, 5000]:
        raise ValueError('Retention PE grid changed at ' + sheet)


def extract(book):
    cross, retention, missing = [], [], []
    thresholds = []
    for address in ('B19', 'C19'):
        text = book['02-测试信息对比'][address]
        match = re.fullmatch(r'\s*(\d+(?:\.\d+)?)\s*bits?/KB\s*', text, re.IGNORECASE)
        if not match or float(match.group(1)) != 62:
            raise ValueError('Expected HD=62 bit/KB at 02-测试信息对比!' + address)
        thresholds.append(float(match.group(1)))
    hd = thresholds[0]

    def cross_block(sheet, header, chip, pe, mode):
        cells = book[sheet]
        _cross_header(cells, sheet, header, chip, pe, mode)
        for row in range(header + 1, header + 5):
            pt = parse_temp(cells['A' + str(row)])
            for col in 'BCDE':
                rt = parse_temp(cells[col + str(header)])
                cell = col + str(row)
                record = dict(chip=chip, read_mode=mode, pe=pe, program_c=pt,
                              read_c=rt, source_cell=sheet + '!' + cell)
                value = cells[cell]
                if value.lower() == 'truncated':
                    missing.append(dict(record, status='truncated'))
                    continue
                fbc = _positive_fbc(value, record['source_cell'])
                cross.append(dict(record, fbc=fbc, nfbc=fbc / hd,
                                  reported_unit='bit/K', retention_age='not reported'))

    for header, pe in zip((4, 12, 20, 28), (300, 1000, 3000, 5000)):
        cross_block('06-跨温风险矩阵_R', header, 'R', pe, 'default')
    for header, pe in ((4, 300), (12, 1000)):
        cross_block('07-跨温BestRetry_R', header, 'R', pe, 'best_retry')
    for header, pe in ((20, 300), (28, 5000)):
        cross_block('07-跨温BestRetry_R', header, 'M', pe, 'default')
    age_days = {'0H': 0., '6M': 182.5, '1Y': 365., '3Y': 1095.}
    for sheet, header, chip, mode in (('04-DR风险矩阵_R', 4, 'R', 'default'),
                                     ('04-DR风险矩阵_R', 14, 'R', 'best_retry'),
                                     ('05-DR风险矩阵_M', 4, 'M', 'default')):
        cells = book[sheet]
        _retention_header(cells, sheet, header, chip, mode)
        for row in range(header + 1, header + 5):
            pe = parse_pe(cells['A' + str(row)])
            base = _positive_fbc(cells['B' + str(row)], sheet + '!B' + str(row))
            for col in 'BCDE':
                label = cells[col + str(header)]
                cell = col + str(row)
                record = dict(chip=chip, read_mode=mode, pe=pe, age_label=label,
                              age_days=age_days[label], source_cell=sheet + '!' + cell)
                value = cells[cell]
                if value.lower() == 'truncated':
                    missing.append(dict(record, status='truncated'))
                    continue
                fbc = _positive_fbc(value, record['source_cell'])
                retention.append(dict(record, fbc=fbc, nfbc=fbc / hd,
                                      growth=fbc / base, baseline_fbc=base,
                                      baseline_cell=sheet + '!B' + str(row)))
    if len(cross) != 116 or len(retention) != 36 or len(missing) != 24:
        raise ValueError('Source layout/count changed; review extraction before fitting')
    keys = [(r['chip'], r['read_mode'], r['pe'], r['program_c'], r['read_c']) for r in cross]
    if len(keys) != len(set(keys)):
        raise ValueError('Duplicate cross-temperature condition')
    return cross, retention, missing, hd


def design(records, variant):
    names = ['beta0', 'beta_pe']
    if variant != 'no_difference':
        names.append('beta_diff')
    if variant != 'no_common':
        names.append('beta_common')
    if any(r['chip'] == 'M' for r in records):
        names.append('M_bias')
    rows = []
    for r in records:
        features = dict(beta0=1, beta_pe=np.log1p(r['pe']),
                        beta_diff=abs(r['program_c'] - r['read_c']) / 100,
                        beta_common=((r['program_c'] + r['read_c']) / 2 - 30) / 100,
                        M_bias=int(r['chip'] == 'M'))
        rows.append([features[n] for n in names])
    return np.asarray(rows), names


def metrics(observed, predicted):
    observed, predicted = np.asarray(observed), np.asarray(predicted)
    return dict(n=len(observed), mae=float(np.mean(np.abs(predicted - observed))),
                rmse=float(np.sqrt(np.mean((predicted - observed)**2))),
                log_rmse=float(np.sqrt(np.mean((np.log(predicted) - np.log(observed))**2))),
                mape_percent=float(100 * np.mean(np.abs(predicted / observed - 1))))


def fit_cross(records, variant):
    x, names = design(records, variant)
    y = np.log([r['nfbc'] for r in records])
    coef, _, rank, singular = np.linalg.lstsq(x, y, rcond=None)
    if rank != len(names):
        raise ValueError('Rank-deficient full fit')
    groups = [tuple(sorted((r['program_c'], r['read_c']))) for r in records]
    oof, folds = np.full(len(y), np.nan), []
    for group in sorted(set(groups)):
        hold = np.asarray([g == group for g in groups])
        beta, _, fold_rank, _ = np.linalg.lstsq(x[~hold], y[~hold], rcond=None)
        if fold_rank != len(names):
            raise ValueError('Rank-deficient temperature-pair CV fold: ' + str(group))
        oof[hold] = np.exp(x[hold] @ beta)
        folds.append(dict(held_temperature_pair_c=group, train_n=int(sum(~hold)),
                          test_n=int(sum(hold)), coefficients=dict(zip(names, map(float, beta))),
                          metrics=metrics(np.exp(y[hold]), oof[hold])))
    predicted = np.exp(x @ coef)
    result = dict(coefficients=dict(zip(names, map(float, coef))),
                  train=metrics(np.exp(y), predicted), cv=metrics(np.exp(y), oof),
                  cv_folds=folds, condition_number=float(singular[0] / singular[-1]))
    result['cv_by_chip'] = {chip: metrics([r['nfbc'] for r in records if r['chip'] == chip],
                                         [oof[i] for i, r in enumerate(records) if r['chip'] == chip])
                            for chip in sorted(set(r['chip'] for r in records))}
    predictions = [dict(r, variant=variant, prediction_fit=float(predicted[i]),
                        prediction_oof=float(oof[i]),
                        held_temperature_pair_c='/'.join(map(str, groups[i])))
                   for i, r in enumerate(records)]
    return result, predictions


def export_model(path, coefficients, retention, source_hash, chip='R', transfer=False, *, fit_count, fit_scope):
    # Missing M retention is never exported as an independently calibrated fit.
    if not isinstance(fit_count, int) or fit_count <= 0 or not fit_scope or any(c in fit_scope for c in '\r\n;'):
        raise ValueError('Model export requires a positive fit_count and one-line fit_scope')
    if chip not in ('R', 'M') or (chip == 'M' and not transfer):
        raise ValueError('M retention can only be exported as an explicitly uncalibrated transfer assumption')
    params = {k: coefficients.get(k, 0.) for k in ('beta0', 'beta_pe', 'beta_diff', 'beta_common')}
    params.update({k: retention[k] for k in ('t0days', 'delta', 'gamma')})
    params['chip_bias'] = coefficients.get('M_bias', 0.) if chip == 'M' else 0.
    lines = ['# FBC/62 HD-budget ratio; aggregate conditions, not chip replicates.',
             '# R retention uses nominal report age; no thermal time acceleration.',
             '# Cross-temperature age and temperature/retention separability are unverified.',
             '# <6 months (including post-refresh early retention) is unvalidated interpolation.',
             '# M transfer, when present, is an assumption and is not measured M retention.']
    lines += [f'{key}={value:.17g}' for key, value in params.items()]
    lines += ['calibrated=' + ('false' if transfer else 'true'),
              f'provenance=Micron-workbook-{source_hash};Eq3-{fit_count}-default-points;temperature-fit-{fit_scope};{chip}-temperature;'
              + ('ASSUMED-R-retention-transfer-NOT-M-calibration' if transfer else 'R-default-Eq5-delta0')
              + ';nominal-age-days;separability-assumed;early-retention-unvalidated']
    path.write_text('\n'.join(lines) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workbook', type=Path, required=True)
    parser.add_argument('--out', type=Path, default=ROOT / 'outputs/calibration')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    source_hash = hashlib.sha256(args.workbook.read_bytes()).hexdigest()
    book = read_workbook(args.workbook)
    cross, retention, missing, hd = extract(book)
    dump(args.out / 'source_cells.json', book)
    dump(args.out / 'missing.json', missing)
    write_csv(args.out / 'cross_points.csv', cross)
    write_csv(args.out / 'retention_points.csv', retention)
    dump(args.out / 'retention_records.json', retention)
    primary = [r for r in cross if r['read_mode'] == 'default']
    common = [r for r in primary if r['pe'] in (300, 5000)
              and r['program_c'] in (-25, 0, 85) and r['read_c'] in (-25, 0, 85)]
    if len(primary) != 96 or len(common) != 36:
        raise ValueError('Unexpected default-condition counts')
    cross_results, predictions = {}, []
    for variant in ('full', 'no_common', 'no_difference'):
        result, rows = fit_cross(primary, variant)
        cross_results[variant] = result
        predictions.extend(rows)
    common_result, _ = fit_cross(common, 'full')
    r_only_result, _ = fit_cross([r for r in primary if r['chip'] == 'R'], 'full')
    from fit_retention import fit_retention
    retention_result = fit_retention(retention)
    dump(args.out / 'retention_fit.json', retention_result)
    selected = retention_result['selected_default']
    write_csv(args.out / 'cross_predictions.csv', predictions)
    report = dict(schema_version=1, workbook_sha256=source_hash,
                  source_workbook='input_workbook',
                  normalization=dict(formula='NFBC = reported Max FBC / 62', denominator=hd,
                                     interpretation='ratio to HD correction budget, not measurement width or UECC probability',
                                     unit_assumption='report bit/K and ECC bit/KB use the same per-K convention'),
                  counts=dict(cross_default=96, cross_retry_separate_not_fitted=20,
                              retention_R_default=16, retention_R_retry=16, retention_M_baseline_only=4,
                              missing_M_retention=12, missing_R_retry_cross=12),
                  cross_fit=cross_results, common36_sensitivity=common_result, R_only_sensitivity=r_only_result,
                  retention_selected=selected,
                  fit_method='OLS natural-log NFBC; equal weight per aggregate condition; no synthetic observations',
                  cross_validation='Leave one unordered PT/RT pair out across all P/E and products; refit all coefficients each fold',
                  cv_scope='14 temperature-pair folds; same products, no unseen-chip validation; 36-point subset has 6 folds',
                  replicate_warning='Reported maxima of unknown aggregation size; conditions are not independent chip replicates',
                  assumptions=['6M=182.5 days, 1Y=365 days, 3Y=1095 days are analysis conventions.',
                               'Source does not specify cross-temperature age; treated as reference surface, provisionally BOL.',
                               'R default retention growth is normalized by its own same-PE 0H, separately from temperature.',
                               'Multiplication of temperature surface and retention growth has no joint-grid validation.',
                               'Runtime age increases one physical day per simulated day, with no Arrhenius acceleration.',
                               'Variable storage temperature effects and self-recovery are not calibrated.',
                               'Post-refresh age below 6M is unvalidated interpolation, not identified early-retention kinetics.',
                               'M retention transfer is a sensitivity assumption only and excluded from primary R experiments.'])
    dump(args.out / 'fit.json', report)
    export_model(args.out / 'micron_R.conf', cross_results['full']['coefficients'], selected, source_hash,
                 fit_count=len(primary), fit_scope='pooled-RM-default')
    export_model(args.out / 'micron_M_transfer_R.conf', cross_results['full']['coefficients'], selected, source_hash, 'M', True,
                 fit_count=len(primary), fit_scope='pooled-RM-default')
    export_model(args.out / 'micron_R_only_temperature.conf', r_only_result['coefficients'], selected, source_hash,
                 fit_count=sum(r['chip'] == 'R' for r in primary), fit_scope='R-only-default')
    print(json.dumps(dict(cross={k: v['cv'] for k, v in cross_results.items()}, retention=selected), indent=2))


if __name__ == '__main__':
    main()
