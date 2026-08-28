#!/usr/bin/env bash
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
for arg in "$@"; do
  case "$arg" in
    -u*|--upload*|-p|--port|--port=*)
      echo "build.sh is compile-only; upload/port options are forbidden." >&2
      exit 1 ;;
  esac
done
arduino-cli compile -b "$FQBN" "$SKETCH_DIR" "$@"
