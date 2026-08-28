# 実機フォーマットと検証のログ (2026-08-28)

対象: XIAO nRF52840 Sense / USBシリアル `282E7C4D710CDACB` / `/dev/cu.usbmodem1101`
フラッシュ: JEDEC `0x856015` (P25Q16H) / 2,097,152 bytes

## 1. 事前スキャン（非破壊, `S`）

```
  sectors (4KiB) : 512
  all-0xFF       : 511
  non-0xFF       : 1
  non-0xFF bytes : 20
  first used sectors: #0
    4F 76 65 72 61 6C 6C 20 54 65 73 74 3A 20 50 41
    53 53 00 00 FF FF ...
  -> DATA PRESENT. back up before formatting.
```

内容は `"Overall Test: PASS\0\0"`（Seeed工場マーカー）のみ。
全内容を `docs/backup/flash-preformat-20260828.md` に記録して退避とした。

## 2. フォーマット（`Z` → `Y`。ユーザーが手動実行）

```
---- format -----------------------------------
  -- gate (raw is re-read after begin) --
    raw_read_ok   : true
    raw_jedec     : 0x856015
    begin_ok      : true
    descriptor_id : 0x856015
    size          : 2097152
    GATE          : PASS
  f_mkfs        : FR_OK  FRESULT=0  362 ms
  io errors     : 0  last=none @0x0
  -- layout read back from media (no cache, no FatFs) --
    BPB: byts/sec=512 sec/clus=2 rsvd=1 nfats=2 rootent=512 fatsz=8 totsec=4033
    physical LBA: MBR 0 / BPB 63 / FAT1 64 / FAT2 72 / root 80 / data 112
    4KiB alignment: FAT1=ok FAT2=ok root=ok data=ok
    clusters      : 1992  (expect 1992)
    data bytes    : 2039808
    RESULT        : MATCHES host measurement
----------------------------------------------
```

## 3. マウント（`M`）

```
  f_mount       : FR_OK
  fat type      : FAT12
  cluster size  : 1024 bytes
  total clusters: 1992
  free clusters : 1992
  free bytes    : 2039808
  files:  (empty)
  io errors     : 0
```

## 4. 書き込みと同一起動での照合（`W` → `C`）

```
    rec_001.adp  60000 B  crc32=0xC0BC0466  889 ms  sync=ok  io_err=0
    rec_002.adp  60000 B  crc32=0xB6474878  934 ms  sync=ok  io_err=0
    rec_003.adp  60000 B  crc32=0x12000169  934 ms  sync=ok  io_err=0

    REC_001.ADP  60000 B  crc32=0xC0BC0466  pattern=ok
    REC_002.ADP  60000 B  crc32=0xB6474878  pattern=ok
    REC_003.ADP  60000 B  crc32=0x12000169  pattern=ok
  files=3  bad=0  io_err=0
```

## 5. ソフトリセット後の照合（`R` → 再接続 → `M` → `C`）

```
  free clusters : 1815   (= 1992 - 59x3。60,000B は 59クラスタ)
    REC_001.ADP  60000 B  crc32=0xC0BC0466  pattern=ok
    REC_002.ADP  60000 B  crc32=0xB6474878  pattern=ok
    REC_003.ADP  60000 B  crc32=0x12000169  pattern=ok
  files=3  bad=0  io_err=0
```

## 6. 満杯試験（`X`）

```
    rec_010.adp  240000 B  crc32=0xAAE77440  3558 ms  sync=ok  io_err=0
    rec_011.adp  240000 B  crc32=0x50F2F981  2978 ms  sync=ok  io_err=0
    rec_012.adp  240000 B  crc32=0x55157FFD  3602 ms  sync=ok  io_err=0
    rec_013.adp  240000 B  crc32=0x29CF449C  3381 ms  sync=ok  io_err=0
    rec_014.adp  240000 B  crc32=0x2E50160B  2978 ms  sync=ok  io_err=0
    rec_015.adp  240000 B  crc32=0x2213F4DD  3559 ms  sync=ok  io_err=0
    rec_016.adp  240000 B  crc32=0x7E2E776A  3604 ms  sync=ok  io_err=0
    rec_017.adp  174080 B  crc32=0xC374A3EE  2217 ms  sync=ok  io_err=0   <- FULL
  free clusters after fill: 0  = 0 bytes
```

算術: 1815 − 235×7 = 170クラスタ = **174,080 B**。8本目が正確にその値で停止した。
I/Oエラーではなく、`f_write` の短書き込みによるクリーンな容量切れ。

## 7. 満杯状態での全照合（`C`）

11ファイルすべて `pattern=ok`、CRC32 は書き込み時と一致、`bad=0 io_err=0`。

## 性能の実測（Phase 3 に効く）

読み戻し照合を有効にした状態で:

| 操作 | 実測 |
|---|---|
| 60,000 B 書き込み | 889〜934 ms |
| 240,000 B 書き込み | 2,978〜3,604 ms |
| 実効書き込み速度 | **約 65〜80 kB/s** |
| `f_mkfs` | 362 ms |

16kHz IMA ADPCM の必要レートは 8 kB/s なので、**約8〜10倍の余裕**がある。
