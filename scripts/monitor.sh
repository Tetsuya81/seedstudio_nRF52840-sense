#!/usr/bin/env bash
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
PORT_="$(detect_port)"
echo "monitor $PORT_ @115200   (終了: Ctrl-C)"
arduino-cli monitor -p "$PORT_" -c baudrate=115200
