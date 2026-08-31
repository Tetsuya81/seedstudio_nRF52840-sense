# Storage v2 firmware integration obligations

Status: host-proven contract; firmware integration is not yet implemented.
`docs/collab/HARDWARE_HOLD` remains authoritative. This document does not authorize
upload, USB/serial access, MSC enumeration, mount, erase, or device tests.

## Purpose

The host model proves format and recovery rules only when the firmware caller preserves
the following I/O invariants. Each obligation has an explicit gate so that moving work
from P1/P2 to firmware integration cannot silently drop it.

## Required obligations and gate tests

| ID | Firmware obligation | G1: recording-flash read-only | G2: destructive geometry/basic-write |
|---|---|---|---|
| F1 | Scan index pages 1..127 exactly once, in physical order. Call `finishBankScan()` and reject `pagesScanned != 127`. | Diagnostic scan reports 127 pages for both banks; a host/mock shortened or skipped scan returns DEVICE_SAFE before the firmware image is accepted. | After one write and restart, the fenced gap is crossed and the later COMMIT is recovered by the same full scan. |
| F2 | Hold a flash-arbiter mutation lock from the final all-FF read through the page-program attempt. No other program/erase may interleave. | Review/instrument the arbiter path without mutating recording flash; competing mutation requests must remain blocked in the mock backend. | Inject a competing request at the read/program boundary; exactly the reserved target may be programmed. |
| F3 | For a data allocation, complete fresh erase in the current boot, observe WIP=0, verify all 4096 bytes are FF, then program page0 immediately under F2. | Verify command ordering against a mock/read-only trace. | Exercise a previously used deleted block; erase, WIP completion, full-FF verification, and page0 program must occur in that order. A torn erase must not reach page0 program. |
| F4 | Consume EraseToken/page reservation on every program attempt, success or failure. Never program the same page twice in one erase epoch. | Host/mock fault injection must show the token consumed after a failed attempt. | Interrupt data-page and index-page programs independently; reboot must choose a different page/block and must not resume an incomplete recording. |
| F5 | During all of EXPORT_AUDIO and EXPORT_RAW, program and erase counters remain exactly zero. | Enter/leave both export modes and compare counters before/after; only reads are permitted. | Repeat after G2 has Tier A, incomplete, deleted, and quarantined fixtures; counter delta must still be zero. |
| F6 | Before every index program, read the selected 256-byte page and require all FF. On failure, perform no mutation and latch DEVICE_SAFE for the boot. | Mock a non-FF selected page and require zero program/erase delta. | Reboot-fence fixture plus injected occupied target must fail closed without touching another page. |
| F7 | Keep every data/index mutation gate closed until both index banks have passed F1, all 496 data blocks have been visited exactly once in order, and the complete ownership map has been reconciled with the index. Allocate only from that verified map; never from partial scan results. | A recording request before scan completion is rejected; diagnostics show scan completion before allocation enablement. Host/mock shortened, skipped, or duplicate data-block scans latch DEVICE_SAFE. | With Tier B and COMMITTED_UNVERIFIED fixtures present, an immediate recording request causes zero program/erase delta on every protected block. After scan completion, protected blocks remain ineligible while a verified free/deleted block may be fresh-erased under F2/F3. |

G1 remains “recording-flash non-destructive,” not globally non-destructive: firmware upload
itself changes device state. G2 requires G0 completion, explicit user approval, and a
verified backup as defined by the roadmap and incident review.

FORMAT is an explicit G2 provisioning operation, not a normal runtime allocation path.
It requires its separate approval and must leave the runtime mutation gate closed until
the new PRBH/READY state and all 496 data blocks have been scanned and reconciled.

Capacity reporting uses the reboot-safe index budget derived from `lastOccupied + 2`.
During the current boot this can under-report the in-RAM cursor by one page; the conservative
value is intentional so `STATUS.TXT` never promises capacity that a restart would consume.

## Host-proof boundary

The P1/P2 suite proves the following only in the host model:

- PRB1/PRBH/PRR1 encoding and strict decoding, CRC, Tier and conflict decisions
- boot fencing, fresh-erase ordering rules, and device/record/block isolation
- index/data scan and capacity invariants, ownership reconciliation, and boot mutation gating
- synthesized read-only FAT12 output and frozen STATUS.TXT behavior
- low-battery write count, reservation behavior, and modeled interruption points

It does not prove:

- the production firmware flash driver, arbiter, allocator, or F1..F7 integration
- TinyUSB MSC callback behavior or macOS mount, re-enumeration, and raw-LUN behavior
- real-NOR power-loss behavior or marginal-cell retention
- low-battery electrical time/voltage margin
- production throughput or actual RAM/flash usage

A host PASS must not be cited as device evidence for any item in the second list.

## Low-battery acceptance condition for G0 planning

The low-battery host state model proves only sequencing: a reserved COMMIT, no erase or
compaction, at most one partial-data-page program plus one COMMIT-page program, COMMIT
readback, and no retry after failure.

Electrical acceptance remains unproven until a separately approved device test demonstrates
the following worst-case condition:

> From low-battery trip through partial data page program, COMMIT page program, and COMMIT
> readback, supply voltage never crosses the nRF52840 brownout-reset threshold.

Measure operation duration, VBAT/VDD droop during both page programs, trip threshold, and
brownout margin under the lowest supported temperature, minimum allowed state of charge,
maximum expected cell resistance, and concurrent microphone/CPU/LED load. The provisional
decision rule is:

`trip threshold >= brownout threshold + measured droop + discharge-slope allowance`

with safety factor at least 3 applied to the measured margin. Page Program max 3 ms × 2 is
a timing input, not a substitute for measurement.

`POWER->POFCON` is only a candidate detection path. Its usefulness depends on the XIAO
battery/charger/regulator path and must be established from the schematic plus the approved
G0-or-later measurement; it is not selected by this document.

## Fault and capacity semantics (MSG-051 / MSG-052)

- An erase command failure, WIP timeout, 4,096-byte all-FF read failure, or all-FF mismatch
  latches a **media I/O `FAULT`** for the boot. Do not reuse `DEVICE_SAFE`, which means
  ownership/reconciliation inconsistency.
- The failed attempt consumes its token and invalidates the whole admission token. No page
  program on that block or on any spare block. No automatic retry, no same-boot fallback.
- If the threshold flush had already started, the written data blocks are classified on the
  next boot by the normal scan (Tier B `INCOMPLETE` or quarantine). They are never published
  as Tier A.
- When data blocks run out mid-recording, stop PDM, drain accepted samples, finalize
  `byteLen`/`bodyCrc32` at the savable boundary, and COMMIT using the reservation held since
  press. The saved prefix becomes Tier A after body verification; the stop reason is
  `STOP_CAPACITY`.
- Capacity evaluation lives in one shared helper used by `STATUS.TXT`, admission, and the
  device indication, so the three can never disagree.
- Host/mock gates test **semantic indications** (`NORMAL`, `CAPACITY_WARNING`,
  `STOP_CAPACITY`, `IO_FAULT`), not colours or millisecond values. Firmware tests separately
  verify the mapping from semantic state to LED pattern.
