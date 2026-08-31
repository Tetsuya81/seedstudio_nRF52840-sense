"""No hardware is opened; child Arduino commands are replaced with a shell stub."""
import importlib.util
import datetime as dt
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('serial_console', ROOT / 'scripts/serial_console.py')
serial_console = importlib.util.module_from_spec(spec)
spec.loader.exec_module(serial_console)
scope_spec = importlib.util.spec_from_file_location('hardware_scope', ROOT / 'scripts/hardware_scope.py')
hardware_scope = importlib.util.module_from_spec(scope_spec)
scope_spec.loader.exec_module(hardware_scope)


class HardwareHoldTests(unittest.TestCase):
    def test_serial_open_is_blocked_before_os_open(self):
        with patch.object(serial_console.os, 'open') as opened:
            with self.assertRaisesRegex(SystemExit, 'HARDWARE HOLD'):
                serial_console.open_port('/dev/DO_NOT_OPEN')
            opened.assert_not_called()

    def test_serial_detection_is_blocked_before_glob(self):
        with patch.object(serial_console.glob, 'glob') as globbed:
            with self.assertRaisesRegex(SystemExit, 'HARDWARE HOLD'):
                serial_console.find_port()
            globbed.assert_not_called()

    def run_shell(self, command):
        stub = 'function arduino-cli { echo UNEXPECTED_ARDUINO_CALL; return 99; }; export -f arduino-cli; '
        return subprocess.run(['bash', '-c', stub + command], cwd=ROOT,
                              text=True, capture_output=True, timeout=5)

    def test_upload_and_monitor_stop_before_arduino(self):
        for script in ['upload.sh', 'monitor.sh']:
            with self.subTest(script=script):
                result = self.run_shell('PORT=/dev/DO_NOT_OPEN bash scripts/' + script)
                self.assertEqual(result.returncode, 1)
                self.assertIn('HARDWARE HOLD', result.stderr)
                self.assertNotIn('UNEXPECTED_ARDUINO_CALL', result.stdout)

    def test_build_cannot_upload(self):
        for option in ['-u', '--upload', '--upload=true', '--port=/dev/DO_NOT_OPEN']:
            with self.subTest(option=option):
                result = self.run_shell('bash scripts/build.sh ' + option)
                self.assertEqual(result.returncode, 1)
                self.assertIn('compile-only', result.stderr)
                self.assertNotIn('UNEXPECTED_ARDUINO_CALL', result.stdout)


class ScopedHardwareTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.image = self.base / 'g1a.hex'
        self.image.write_bytes(b'approved image')
        self.token = self.base / 'scope.json'
        self.state = self.base / 'uses.jsonl'
        self.now = dt.datetime(2026, 8, 31, 6, 0, tzinfo=dt.timezone.utc)

    def tearDown(self):
        self.temp.cleanup()

    def write_token(self, **changes):
        token = {
            'version': 1,
            'scope': 'G0-G1A',
            'approval_ref': 'MSG-058',
            'expires_at': '2026-08-31T07:00:00Z',
            'image_sha256': hashlib.sha256(self.image.read_bytes()).hexdigest(),
            'allowed_uses': {'upload': 1, 'serial-read': 1},
            'logs': {
                'upload': 'artifacts/g0/upload.log',
                'serial-read': 'artifacts/g0/serial.log',
            },
        }
        token.update(changes)
        self.token.write_text(json.dumps(token), encoding='utf-8')

    def consume(self, operation, image=None):
        return hardware_scope.consume(
            operation, image_path=image, token_path=self.token,
            state_path=self.state, now=self.now)

    def test_missing_token_rejects_operation(self):
        with self.assertRaisesRegex(hardware_scope.ScopeError, 'token is missing'):
            self.consume('upload', self.image)

    def test_scoped_wrappers_stop_before_device_access_without_token(self):
        for command in [
                ['python3', 'scripts/g0_scoped_upload.py', str(self.image)],
                ['python3', 'scripts/g0_scoped_serial_read.py', '--seconds', '1']]:
            with self.subTest(command=command[1]):
                result = subprocess.run(command, cwd=ROOT, text=True,
                                        capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 1)
                self.assertIn('HARDWARE HOLD', result.stderr)

    def test_scope_rejects_extra_operation_and_wrong_image(self):
        self.write_token(allowed_uses={'upload': 1, 'serial-read': 1, 'monitor': 1})
        with self.assertRaisesRegex(hardware_scope.ScopeError, 'allowed_uses'):
            self.consume('upload', self.image)
        self.write_token(image_sha256='0' * 64)
        with self.assertRaisesRegex(hardware_scope.ScopeError, 'does not match'):
            self.consume('upload', self.image)

    def test_serial_requires_upload_and_each_use_is_one_shot(self):
        self.write_token()
        with self.assertRaisesRegex(hardware_scope.ScopeError, 'until the approved upload'):
            self.consume('serial-read')
        self.consume('upload', self.image)
        with self.assertRaisesRegex(hardware_scope.ScopeError, 'already been consumed'):
            self.consume('upload', self.image)
        self.consume('serial-read')
        with self.assertRaisesRegex(hardware_scope.ScopeError, 'already been consumed'):
            self.consume('serial-read')
        records = [json.loads(line) for line in self.state.read_text().splitlines()]
        self.assertEqual([r['operation'] for r in records], ['upload', 'serial-read'])

    def test_expired_or_changed_token_is_rejected(self):
        self.write_token(expires_at='2026-08-31T05:59:59Z')
        with self.assertRaisesRegex(hardware_scope.ScopeError, 'expired'):
            self.consume('upload', self.image)
        self.write_token()
        self.consume('upload', self.image)
        document = json.loads(self.token.read_text())
        document['expires_at'] = '2026-08-31T06:30:00Z'
        self.token.write_text(json.dumps(document), encoding='utf-8')
        with self.assertRaisesRegex(hardware_scope.ScopeError, 'changed after its first use'):
            self.consume('serial-read')


if __name__ == '__main__':
    unittest.main()
