#!/usr/bin/env bash
set -euo pipefail
tarf_test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tarf_test_binary=$(mktemp "${TMPDIR:-/tmp}/tarf-reliability-test.XXXXXX")
trap 'rm -f "$tarf_test_binary"' EXIT
"${CXX:-g++}" -std=c++11 -O2 \
  "$tarf_test_dir/reliability_model_test.cpp" \
  "$tarf_test_dir/../src/ssd/Reliability.cpp" -o "$tarf_test_binary"
"$tarf_test_binary" "$tarf_test_dir/temperature_step.csv"
