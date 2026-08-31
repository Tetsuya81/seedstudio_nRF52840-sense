#!/usr/bin/env python3
"""Upload the single approved G1A image; no retry is possible after start."""

import argparse
import datetime as dt
import json
import subprocess
from pathlib import Path

from hardware_scope import ScopeError, consume


FQBN = "Seeeduino:nrf52:xiaonRF52840Sense"
BOOTLOADER_PID = "0045"


def detect_one_port(log) -> str:
    result = subprocess.run(
        ["arduino-cli", "board", "list", "--format", "json"],
        text=True, capture_output=True, timeout=10, check=False)
    log.write(result.stdout)
    log.write(result.stderr)
    log.flush()
    if result.returncode:
        raise RuntimeError(f"port listing failed with exit {result.returncode}")
    document = json.loads(result.stdout)
    entries = document.get("detected_ports", document) if isinstance(document, dict) else document
    ports = []
    for entry in entries:
        port = entry.get("port", {})
        props = port.get("properties", {}) or {}
        vid = props.get("vid", "").lower().removeprefix("0x")
        pid = props.get("pid", "").lower().removeprefix("0x")
        if vid == "2886" and pid == BOOTLOADER_PID:
            ports.append(port.get("address", ""))
    ports = sorted({port for port in ports if port})
    if len(ports) != 1:
        raise RuntimeError(
            f"expected exactly one XIAO bootloader port (2886:{BOOTLOADER_PID}), "
            f"found {len(ports)}")
    return ports[0]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    try:
        log_path = consume("upload", image_path=args.image.resolve())
    except ScopeError as exc:
        parser.exit(1, f"HARDWARE HOLD: {exc}\n")

    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("a", encoding="utf-8") as log:
        log.write(f"\n[{dt.datetime.now(dt.timezone.utc).isoformat()}] G0 upload consumed\n")
        try:
            port = detect_one_port(log)
            command = ["arduino-cli", "upload", "-b", FQBN, "-p", port,
                       "--upload-property", "upload.use_1200bps_touch=false",
                       "--upload-property", "upload.wait_for_upload_port=false",
                       "--input-file", str(args.image.resolve())]
            log.write("command: " + " ".join(command) + "\n")
            log.flush()
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=120, check=False)
        except Exception as exc:
            log.write(f"STOP: {type(exc).__name__}: {exc}\n")
            raise SystemExit(f"G0 upload stopped; scope is consumed: {exc}")
        log.write(f"exit: {result.returncode}\n")
        log.flush()
        if result.returncode:
            raise SystemExit("G0 upload failed; retry is forbidden and scope is consumed")


if __name__ == "__main__":
    main()
