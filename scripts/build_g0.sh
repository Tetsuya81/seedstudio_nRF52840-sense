#!/usr/bin/env bash
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

for arg in "$@"; do
  case "$arg" in
    -u*|--upload*|-p|--port|--port=*|-l|--protocol|--protocol=*)
      echo "build_g0.sh is compile-only; upload/port/protocol options are forbidden." >&2
      exit 1 ;;
    *)
      echo "build_g0.sh does not accept arguments." >&2
      exit 1 ;;
  esac
done

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
G0_SKETCH="$ROOT_DIR/firmware/g0_microphone_smoke"
G0_BUILD="$ROOT_DIR/build/g0_microphone_smoke"

arduino-cli compile --clean --warnings all -b "$FQBN" \
  --build-property 'compiler.c.extra_flags=-DCFG_TUSB_CONFIG_FILE="g0_tusb_config.h"' \
  --build-property 'compiler.cpp.extra_flags=-DCFG_TUSB_CONFIG_FILE="g0_tusb_config.h"' \
  --build-path "$G0_BUILD" "$G0_SKETCH"
PYTHONDONTWRITEBYTECODE=1 python3 "$ROOT_DIR/tools/storage_safety/check_g0_image.py" "$G0_BUILD"
