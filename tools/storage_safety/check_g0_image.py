#!/usr/bin/env python3
"""Host-only linked-image gate for the dedicated G0 microphone build."""

import argparse
import hashlib
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "firmware/g0_microphone_smoke/g0_microphone_smoke.ino"
FORBIDDEN_SOURCE = (
    "Adafruit_SPIFlash", "MscBridge", "FatFs", "diskio", "LittleFS",
    "USBDevice.detach", "USBDevice.attach", "NVIC_SystemReset", "Serial.read",
    "Serial.available",
)
FORBIDDEN_SYMBOL = re.compile(
    r"Adafruit_SPIFlash|FlashTransport_QSPI|nrfx_qspi|nrf_qspi|MscBridge|"
    r"TinyUSBMSC|tud_msc|\bf_mount\b|\bdisk_(?:initialize|read|write|ioctl)\b|"
    r"USBDeviceClass::(?:detach|attach)")
FORBIDDEN_SKETCH_REFERENCE = re.compile(
    FORBIDDEN_SYMBOL.pattern + r"|NVIC_SystemReset|Serial_::(?:read|available)")
REQUIRED_SYMBOL = ("PDMClass::begin", "PDMClass::read", "onPdmData")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def find_nm() -> Path:
    candidates = []
    for base in (Path.home() / "Library/Arduino15", Path.home() / ".arduino15"):
        candidates.extend(base.glob(
            "packages/Seeeduino/tools/arm-none-eabi-gcc/*/bin/arm-none-eabi-nm"))
    if not candidates:
        raise SystemExit("G0 CHECK FAIL: arm-none-eabi-nm not found")
    return sorted(candidates)[-1]


def one(build: Path, suffix: str) -> Path:
    matches = sorted(build.glob("*" + suffix))
    if len(matches) != 1:
        raise SystemExit(f"G0 CHECK FAIL: expected one {suffix}, found {len(matches)}")
    return matches[0]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("build", type=Path)
    args = parser.parse_args()
    build = args.build.resolve()

    source_text = SOURCE.read_text(encoding="utf-8")
    bad_source = [token for token in FORBIDDEN_SOURCE if token in source_text]
    if bad_source:
        raise SystemExit("G0 CHECK FAIL: forbidden source token(s): " + ", ".join(bad_source))

    elf = one(build, ".elf")
    image_hex = one(build, ".hex")
    package = one(build, ".zip")
    symbols = subprocess.run([str(find_nm()), "-C", str(elf)], check=True,
                             text=True, capture_output=True).stdout
    forbidden = sorted(set(FORBIDDEN_SYMBOL.findall(symbols)))
    if forbidden:
        raise SystemExit("G0 CHECK FAIL: forbidden linked symbol(s): " + ", ".join(forbidden))
    sketch_objects = sorted((build / "sketch").glob("*.o"))
    if not sketch_objects:
        raise SystemExit("G0 CHECK FAIL: sketch object not found")
    sketch_refs = subprocess.run(
        [str(find_nm()), "-uC", *map(str, sketch_objects)], check=True,
        text=True, capture_output=True).stdout
    forbidden_refs = sorted(set(FORBIDDEN_SKETCH_REFERENCE.findall(sketch_refs)))
    if forbidden_refs:
        raise SystemExit("G0 CHECK FAIL: forbidden sketch reference(s): " +
                         ", ".join(forbidden_refs))
    missing = [symbol for symbol in REQUIRED_SYMBOL if symbol not in symbols]
    if missing:
        raise SystemExit("G0 CHECK FAIL: required symbol(s) missing: " + ", ".join(missing))

    print("G0 IMAGE CHECK PASS")
    print(f"source={SOURCE.relative_to(ROOT)}")
    print(f"elf={elf} sha256={sha256(elf)}")
    print(f"hex={image_hex} sha256={sha256(image_hex)}")
    print(f"package={package} sha256={sha256(package)}")
    print("forbidden_linked_symbols=0")
    print("forbidden_sketch_references=0")


if __name__ == "__main__":
    main()
