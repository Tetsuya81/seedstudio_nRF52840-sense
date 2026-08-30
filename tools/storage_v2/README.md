# Storage v2 host model

This directory models the storage design agreed in `docs/collab/chat.md`
MSG-026/027. It does not open USB, serial ports, block devices, or hardware.

Run from the repository root:

```sh
bash tools/storage_v2/run.sh
```

The suite covers:

- 2 MiB NOR semantics (1-to-0 program, one page program per erase epoch,
  complete and torn page/erase operations)
- PRB1 data headers and PRBH/PRR1 A/B index banks
- COMMIT/DELETE, Tier A full-body CRC verification, tombstone retention,
  high-water marks, and interrupted compaction
- a synthesized read-only FAT12 volume exposing only Tier A recordings while
  removing each physical 32-byte PRB1 header
- shared freestanding format scanning with runtime assertions that index pages
  1..127 and all 496 data blocks were visited exactly once and in order
- a boot mutation gate and allocation only from a fully reconciled ownership map
- reboot fencing, fresh-erase allocation, and device/record/block isolation
- deferred full-body verification and a frozen diagnostic `STATUS.TXT` with
  capacity pressure, incomplete/body-mismatch counts, and backup advice
- a low-battery stop model allowing at most two page programs without retry
- firmware caller obligations and G1/G2 acceptance tests tracked in `docs/storage-v2-firmware-obligations.md`
- FatFs mount/read of the synthesized image, 8.3 names, fragmented physical
  placement, full capacity, and arbitrary reads across 4064-byte seams
- exclusive audio/raw export gates and COMMIT-page reservation invariants

Passing this suite is host-model evidence only. It does not establish real NOR
power-loss behaviour, USB/macOS compatibility, electrical safety, throughput,
or firmware integration correctness. `docs/collab/HARDWARE_HOLD` remains active.
