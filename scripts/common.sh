#!/usr/bin/env bash
# 共通設定
set -euo pipefail

# adafruit-nrfutil (PyInstaller製) が ASCII ロケールだと落ちるため必須
export LC_ALL=${LC_ALL:-en_US.UTF-8}
export LANG=${LANG:-en_US.UTF-8}

FQBN="Seeeduino:nrf52:xiaonRF52840Sense"
SKETCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/firmware/pebble_ring"

# XIAO nRF52840 のポートを自動検出 (VID 0x2886)
detect_port() {
  if [[ -n "${PORT:-}" ]]; then echo "$PORT"; return; fi
  local p
  p=$(arduino-cli board list --format json 2>/dev/null \
      | python3 -c 'import sys,json
d=json.load(sys.stdin)
ports=d.get("detected_ports", d) if isinstance(d,dict) else d
for e in ports:
    pp=e.get("port",{})
    props=pp.get("properties",{}) or {}
    if props.get("vid","").lower() in ("0x2886","2886"):
        print(pp.get("address","")); break
' || true)
  if [[ -z "$p" ]]; then
    p=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
  fi
  if [[ -z "$p" ]]; then
    echo "XIAO nRF52840 が見つかりません。USBを接続してください。" >&2
    exit 1
  fi
  echo "$p"
}
