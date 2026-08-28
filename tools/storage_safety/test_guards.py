"""No hardware is opened; child Arduino commands are replaced with a shell stub."""
import importlib.util
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('serial_console', ROOT / 'scripts/serial_console.py')
serial_console = importlib.util.module_from_spec(spec)
spec.loader.exec_module(serial_console)


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


if __name__ == '__main__':
    unittest.main()
