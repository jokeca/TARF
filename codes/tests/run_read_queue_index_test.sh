#!/usr/bin/env bash
set -euo pipefail
queue_test_dir=$(mktemp -d /tmp/tarf_read_index.XXXXXX)
trap 'rm -rf "$queue_test_dir"' EXIT
mapfile -t queue_test_objects < <(rg --files build -g '*.o' | rg -v '(^|/)main\.o$')
g++ -std=c++11 -O2 -g tests/test_read_queue_index.cpp "${queue_test_objects[@]}" -o "$queue_test_dir/test"
"$queue_test_dir/test"
