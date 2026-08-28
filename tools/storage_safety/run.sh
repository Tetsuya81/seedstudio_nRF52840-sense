#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
trial_dir="$(mktemp -d "${TMPDIR:-/tmp}/pebble-storage-safety.XXXXXX")"
echo "Host-only test artifacts: $trial_dir"
cc -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$project_dir/firmware/pebble_ring/src/fatfs" \
  -c "$project_dir/firmware/pebble_ring/src/fatfs/ff.c" -o "$trial_dir/ff.o"
c++ -std=c++11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer -I"$project_dir/tools/storage_safety/stubs" \
  "$project_dir/tools/storage_safety/test_storage.cpp" "$trial_dir/ff.o" \
  -o "$trial_dir/test_storage"
"$trial_dir/test_storage"
