#!/usr/bin/env bash
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
arduino-cli compile -b "$FQBN" "$SKETCH_DIR" "$@"
