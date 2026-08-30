#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
trial_dir="$(mktemp -d "${TMPDIR:-/tmp}/pebble-format.XXXXXX")"
echo "Shared-format artifacts: $trial_dir"

c++ -std=c++11 -Wall -Wextra -Werror -O1 -g \
  -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$project_dir/pebble_format" \
  "$project_dir/pebble_format/pebble_format.cpp" \
  "$project_dir/pebble_format/test_pebble_format.cpp" \
  -o "$trial_dir/test_pebble_format"

"$trial_dir/test_pebble_format"
