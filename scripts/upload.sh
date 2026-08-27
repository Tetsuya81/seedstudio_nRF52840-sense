#!/usr/bin/env bash
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
PORT_="$(detect_port)"
echo "uploading to $PORT_ ..."
arduino-cli compile -b "$FQBN" -u -p "$PORT_" "$SKETCH_DIR" "$@" || {
  echo ""
  echo "書き込みに失敗した場合: 基板のRESETボタンを素早く2回押して"
  echo "ブートローダモード (LEDがゆっくり点滅 / XIAO-SENSE ドライブが出現) にしてから再実行してください。"
  exit 1
}
