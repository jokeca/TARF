#!/usr/bin/env bash
set -euo pipefail
tarf_test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tarf_test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/tarf-profile-test.XXXXXX")
trap 'rm -rf "$tarf_test_tmp"' EXIT
"${CXX:-g++}" -std=c++11 -O2 \
  "$tarf_test_dir/reliability_profile_test.cpp" \
  "$tarf_test_dir/../src/ssd/Reliability.cpp" -o "$tarf_test_tmp/test"
python3 - "$tarf_test_tmp" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
header = 'block_index,pe,age_days,program_temp_c\n'
rows = [f'{i},{100*(i+1)},{10+i},{[30,55,0,70][i%4]}\n' for i in range(8)]
(root / 'valid.csv').write_text(header + ''.join(rows))
cases = {
    'duplicate': rows + [rows[0]],
    'missing_middle': rows[:3] + rows[4:],
    'missing_final': rows[:-1],
    'extra': rows + ['8,100,0,30\n'],
    'negative_age': ['0,100,-1,30\n'] + rows[1:],
    'nonfinite': ['0,100,nan,30\n'] + rows[1:],
    'negative_pe': ['0,-1,10,30\n'] + rows[1:],
    'fractional_pe': ['0,1.5,10,30\n'] + rows[1:],
    'underzero': ['0,100,10,-273.15\n'] + rows[1:],
    'too_big_pe': ['0,4294967296,10,30\n'] + rows[1:],
    'empty': [],
}
for name, lines in cases.items():
    (root / (name + '.csv')).write_text(header + ''.join(lines))
(root / 'badheader.csv').write_text('wrong,header\n' + ''.join(rows))
(root / 'temperature.csv').write_text('time_ns,temp_C\n0,30\n1000000000,70\n2000000000,0\n3000000000,55\n5000000000,30\n')
PY
"$tarf_test_tmp/test" "$tarf_test_tmp/valid.csv" "$tarf_test_tmp/temperature.csv" valid
for tarf_case in duplicate missing_middle missing_final extra negative_age nonfinite negative_pe fractional_pe underzero too_big_pe empty badheader; do
  "$tarf_test_tmp/test" "$tarf_test_tmp/$tarf_case.csv" "$tarf_test_tmp/temperature.csv" invalid
done
