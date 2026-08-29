#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
trial_dir="$(mktemp -d "${TMPDIR:-/tmp}/pebble-storage-v2.XXXXXX")"
echo "Host-only storage v2 artifacts: $trial_dir"

cc -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$project_dir/tools/fatfs_host/fatfs" \
  -c "$project_dir/tools/fatfs_host/fatfs/ff.c" -o "$trial_dir/ff.o"

c++ -std=c++17 -Wall -Wextra -Werror -O1 -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$project_dir/tools/storage_v2" \
  -I"$project_dir/tools/fatfs_host/fatfs" \
  "$project_dir/tools/storage_v2/storage_model.cpp" \
  "$project_dir/tools/storage_v2/test_storage_v2.cpp" \
  "$trial_dir/ff.o" -o "$trial_dir/test_storage_v2"

"$trial_dir/test_storage_v2"
