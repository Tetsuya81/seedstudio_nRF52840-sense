# Host-only storage safety tests

Run from the project root:

```sh
bash tools/storage_safety/run.sh
PYTHONDONTWRITEBYTECODE=1 python3 tools/storage_safety/test_guards.py
```

The C++ suite compiles the **firmware's actual FatFs R0.13c and diskio_flash.cpp**
against an in-memory NOR stub. It checks range overflow, pending-data retention,
erase/write/read/verification failures, fault latching, and deletion sync.
AddressSanitizer and UndefinedBehaviorSanitizer are enabled. Outputs go to a new
temporary directory; nothing is mounted and no device is opened.

It also demonstrates a limit: after a successful sync, a later interrupted erase
of the same 4KiB block can destroy the earlier data. This is a model counterexample,
not a measurement of physical timing or a prediction of a specific device failure.

The Python suite checks the upload/monitor/serial guards without hardware access.
Arduino calls are replaced with a stub; opening a serial port is mocked.

These tests do **not** establish USB electrical safety, host-driver stability,
real NOR power-loss behaviour, or the absence of other firmware defects.
The hardware hold remains active even when every host test passes.
