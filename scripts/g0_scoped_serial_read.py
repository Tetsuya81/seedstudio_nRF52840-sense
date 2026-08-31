#!/usr/bin/env python3
"""One receive-only G0 serial session, fixed at 115200 baud and <=60 seconds."""

import argparse
import codecs
import datetime as dt
import json
import os
import select
import subprocess
import termios
import time

from hardware_scope import ScopeError, consume


APPLICATION_PID = "8045"


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
        if vid == "2886" and pid == APPLICATION_PID:
            ports.append(port.get("address", ""))
    ports = sorted({port for port in ports if port})
    if len(ports) != 1:
        raise RuntimeError(
            f"expected exactly one XIAO application port (2886:{APPLICATION_PID}), "
            f"found {len(ports)}")
    return ports[0]


def open_115200(path: str) -> int:
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=float, default=60.0)
    args = parser.parse_args()
    if not 0 < args.seconds <= 60:
        parser.error("--seconds must be greater than 0 and at most 60")
    try:
        log_path = consume("serial-read")
    except ScopeError as exc:
        parser.exit(1, f"HARDWARE HOLD: {exc}\n")

    log_path.parent.mkdir(parents=True, exist_ok=True)
    decoder = codecs.getincrementaldecoder("utf-8")("replace")
    fd = None
    with log_path.open("a", encoding="utf-8") as log:
        log.write(f"\n[{dt.datetime.now(dt.timezone.utc).isoformat()}] G0 serial-read consumed; baud=115200\n")
        log.flush()
        try:
            port = detect_one_port(log)
            fd = open_115200(port)
            end = time.monotonic() + args.seconds
            while time.monotonic() < end:
                ready, _, _ = select.select([fd], [], [], 0.1)
                if ready:
                    data = os.read(fd, 4096)
                    if data:
                        log.write(decoder.decode(data))
                        log.flush()
        except Exception as exc:
            log.write(f"\nSTOP: {type(exc).__name__}: {exc}\n")
            raise SystemExit(f"G0 serial-read stopped; scope is consumed: {exc}")
        finally:
            if fd is not None:
                os.close(fd)
        log.write("\nserial-read closed\n")
        log.flush()


if __name__ == "__main__":
    main()
