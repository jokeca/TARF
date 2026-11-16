#!/usr/bin/env bash
set -euo pipefail
tarf_test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tarf_test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/tarf-numeric-test.XXXXXX")
trap 'rm -rf "$tarf_test_tmp"' EXIT
"${CXX:-g++}" -std=c++11 -O2 \
  "$tarf_test_dir/reliability_numeric_test.cpp" \
  "$tarf_test_dir/../src/ssd/Reliability.cpp" -o "$tarf_test_tmp/test"
python3 - "$tarf_test_tmp" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
for name, beta0, delta, gamma in [('high', -3, 1000, 0.5), ('low', -1e300, 0, 0)]:
    (root / (name + '.conf')).write_text(f'beta0={beta0}\nbeta_pe=0.3\nbeta_diff=2\nbeta_common=-1\nt0days=1e-300\ndelta={delta}\ngamma={gamma}\ncalibrated=false\nprovenance=synthetic-numerical-regression-only\n')
PY
"$tarf_test_tmp/test" "$tarf_test_tmp/high.conf" high
"$tarf_test_tmp/test" "$tarf_test_tmp/low.conf" low
