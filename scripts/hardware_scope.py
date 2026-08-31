#!/usr/bin/env python3
"""One-shot HARDWARE_HOLD exception guard for an approved G0 run.

The ordinary upload/monitor tools intentionally remain blocked. This module is
used only by the dedicated, receive-only G0 tools. A use is consumed before
the first device/port access; a failed operation therefore cannot be retried.
"""

from __future__ import annotations

import argparse
import datetime as dt
import fcntl
import hashlib
import json
import os
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TOKEN = ROOT / "docs/collab/G0_SCOPE.json"
EXPECTED_USES = {"upload": 1, "serial-read": 1}


class ScopeError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as src:
        for chunk in iter(lambda: src.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _canonical_json_sha256(document: dict) -> str:
    encoded = json.dumps(document, ensure_ascii=False, sort_keys=True,
                         separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _parse_expiry(value: object, now: dt.datetime) -> None:
    if not isinstance(value, str) or not value.endswith("Z"):
        raise ScopeError("expires_at must be an RFC3339 UTC timestamp ending in Z")
    try:
        expires = dt.datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError as exc:
        raise ScopeError("expires_at is not a valid RFC3339 timestamp") from exc
    if expires <= now:
        raise ScopeError("scope token has expired")


def _resolve_log_path(value: object, operation: str) -> Path:
    if not isinstance(value, str) or not value:
        raise ScopeError(f"missing log path for {operation}")
    relative = Path(value)
    if relative.is_absolute() or ".." in relative.parts:
        raise ScopeError(f"log path for {operation} must stay below the repository")
    resolved = (ROOT / relative).resolve()
    if ROOT != resolved and ROOT not in resolved.parents:
        raise ScopeError(f"log path for {operation} escapes the repository")
    return resolved


def _load_token(token_path: Path, image_path: Path | None,
                operation: str, now: dt.datetime) -> tuple[dict, str, Path]:
    hold = ROOT / "docs/collab/HARDWARE_HOLD"
    if not hold.is_file():
        raise ScopeError("scoped operation requires HARDWARE_HOLD to remain active")
    if token_path.is_symlink() or not token_path.is_file():
        raise ScopeError("approved G0 scope token is missing or is a symlink")
    try:
        token = json.loads(token_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ScopeError("cannot read a valid G0 scope token") from exc
    if not isinstance(token, dict):
        raise ScopeError("scope token must be a JSON object")
    if token.get("version") != 1 or token.get("scope") != "G0-G1A":
        raise ScopeError("unsupported scope token version or scope")
    approval_ref = token.get("approval_ref")
    if not isinstance(approval_ref, str) or not re.fullmatch(r"MSG-\d{3}", approval_ref):
        raise ScopeError("approval_ref must identify the recorded approval message")
    _parse_expiry(token.get("expires_at"), now)
    if token.get("allowed_uses") != EXPECTED_USES:
        raise ScopeError("allowed_uses must be exactly one upload and one serial-read")
    if operation not in EXPECTED_USES:
        raise ScopeError(f"operation is outside G0 scope: {operation}")

    image_hash = token.get("image_sha256")
    if not isinstance(image_hash, str) or not re.fullmatch(r"[0-9a-f]{64}", image_hash):
        raise ScopeError("image_sha256 must be a lowercase SHA-256 digest")
    if operation == "upload":
        if image_path is None or not image_path.is_file():
            raise ScopeError("upload image is missing")
        if sha256_file(image_path) != image_hash:
            raise ScopeError("upload image SHA-256 does not match the approved token")

    logs = token.get("logs")
    if not isinstance(logs, dict) or set(logs) != set(EXPECTED_USES):
        raise ScopeError("logs must specify only upload and serial-read destinations")
    log_path = _resolve_log_path(logs.get(operation), operation)
    return token, _canonical_json_sha256(token), log_path


def consume(operation: str, *, image_path: Path | None = None,
            token_path: Path = DEFAULT_TOKEN, state_path: Path | None = None,
            now: dt.datetime | None = None) -> Path:
    """Validate and irreversibly consume one operation before device access."""
    current = now or dt.datetime.now(dt.timezone.utc)
    if current.tzinfo is None:
        raise ScopeError("now must be timezone-aware")
    # Keep the final path component unresolved so _load_token can reject a
    # symlink instead of silently following it.
    token_path = token_path.absolute()
    state_path = state_path or token_path.with_suffix(token_path.suffix + ".uses.jsonl")
    if state_path.is_symlink():
        raise ScopeError("scope use record must not be a symlink")
    token, token_hash, log_path = _load_token(token_path, image_path, operation, current)

    state_path.parent.mkdir(parents=True, exist_ok=True)
    lock_path = state_path.with_suffix(state_path.suffix + ".lock")
    if lock_path.is_symlink():
        raise ScopeError("scope lock must not be a symlink")
    with lock_path.open("a+", encoding="utf-8") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        events = []
        if state_path.exists():
            try:
                events = [json.loads(line) for line in state_path.read_text(encoding="utf-8").splitlines()
                          if line.strip()]
            except (OSError, json.JSONDecodeError) as exc:
                raise ScopeError("scope use record is invalid") from exc
        for event in events:
            if event.get("token_sha256") != token_hash:
                raise ScopeError("scope token changed after its first use")
            if event.get("operation") not in EXPECTED_USES:
                raise ScopeError("scope use record contains an unknown operation")
        used = [event.get("operation") for event in events]
        if operation in used:
            raise ScopeError(f"{operation} scope has already been consumed")
        if operation == "serial-read" and "upload" not in used:
            raise ScopeError("serial-read is forbidden until the approved upload is consumed")

        event = {
            "approval_ref": token["approval_ref"],
            "consumed_at": current.astimezone(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
            "operation": operation,
            "token_sha256": token_hash,
        }
        line = json.dumps(event, ensure_ascii=False, sort_keys=True) + "\n"
        fd = os.open(state_path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
        try:
            os.write(fd, line.encode("utf-8"))
            os.fsync(fd)
        finally:
            os.close(fd)
    return log_path


def main() -> None:
    parser = argparse.ArgumentParser(description="Consume one approved G0 hardware operation")
    parser.add_argument("operation", choices=sorted(EXPECTED_USES))
    parser.add_argument("--image", type=Path)
    args = parser.parse_args()
    try:
        log_path = consume(args.operation, image_path=args.image)
    except ScopeError as exc:
        parser.exit(1, f"HARDWARE HOLD: {exc}\n")
    print(log_path)


if __name__ == "__main__":
    main()
