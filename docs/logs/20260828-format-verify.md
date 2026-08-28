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

---

# USB MSC の検証（同日、追記）

## 列挙

```
bInterfaceClass = 8 / IOUSBMassStorageDriver がロード
/dev/disk4 (external, physical)   FDisk_partition_scheme  *2.1 MB
  disk4s1  DOS_FAT_12  NO NAME
```

## read-only であること

```
/dev/disk4s1 on /Volumes/NO NAME
  (msdos, local, nodev, nosuid, read-only, noowners, noatime, fskit)

$ touch "/Volumes/NO NAME/writetest.txt"
touch: /Volumes/NO NAME/writetest.txt: Read-only file system
```

`tud_msc_is_writable_cb()` を false にする方式が意図どおり機能し、
**書き込みはEROFSでクリーンに拒否**される。

## macOS のメタデータが作られないこと

ボリューム上のエントリは REC_*.ADP の11本のみ。
`._<name>`（AppleDouble）も `.Trashes` も `.Spotlight-V100` も `.fseventsd` も
作られていない。→ F-24 の寄生を read-only 化で実際に防げている。

## 基板 → Mac のコピーと内容照合

```
file               size     expect     actual  result
REC_001.ADP       60000 0xC0BC0466 0xC0BC0466  OK
REC_002.ADP       60000 0xB6474878 0xB6474878  OK
REC_003.ADP       60000 0x12000169 0x12000169  OK
REC_010.ADP      240000 0xAAE77440 0xAAE77440  OK
REC_011.ADP      240000 0x50F2F981 0x50F2F981  OK
REC_012.ADP      240000 0x55157FFD 0x55157FFD  OK
REC_013.ADP      240000 0x29CF449C 0x29CF449C  OK
REC_014.ADP      240000 0x2E50160B 0x2E50160B  OK
REC_015.ADP      240000 0x2213F4DD 0x2213F4DD  OK
REC_016.ADP      240000 0x7E2E776A 0x7E2E776A  OK
REC_017.ADP      174080 0xC374A3EE 0xC374A3EE  OK

合計 2,034,080 bytes  不一致 0 件
```

期待値は基板が書き込み時に計算した CRC32。**全11本が一致**した。
`diskutil eject` も正常に完了。

## メディア出現の検知について

起動時に `setUnitReady(false)` にしておくと、macOS は列挙時の初回プローブで
メディア無しと判定し、その後 `setUnitReady(true)` にしても**自発的に再プローブしない**。
起動時から ready にするか、`USBDevice.detach()/attach()` で再列挙させる必要がある。

## 誤診の記録（自戒）

途中で「read-only ボリュームへの書き込み試行でボリュームが wedge した」と
判断してユーザーに物理的な抜き差しを依頼したが、**これは誤りだった**。

実際には `touch` は `Read-only file system` で即座に失敗しており、
`diskutil unmountDisk force` も成功していた。
ハングに見えたのは、こちらが復旧コマンドを重ねて発行したために
`diskutil` / `mount` / `ls` が互いを待ち、ツール側のタイムアウトに
当たっただけだった。デバイス側の不具合ではない。

**バックグラウンドに落ちたコマンドの出力を確認する前に結論を出したのが原因。**

---

# 意図しない電源断（Mac の電源が落ちた）の検証

Mac の電源が落ちたため、USB給電の基板も同時に電源を失った。
**計画していた「書き込み中の電源断試験」ではない**ので、
何が言えて何が言えないかを分けて記録する。

## 電源断時に基板が何をしていたか（証拠にもとづく）

1. 基板に対して最後に実行したのは `M` → `D` → `M`。**その全出力を受け取っている**
   （2回のマウント、8件の削除、空き1815クラスタ）
2. その後、基板に触れるコマンドは一切実行していない
3. したがって基板は**アイドル状態**（ファイルオープンなし、書き込み進行なし）だった

## 電源断後の状態

```
---- mount ------------------------------------
  f_mount       : FR_OK
  fat type      : FAT12
  cluster size  : 1024 bytes
  total clusters: 1992
  free clusters : 1815
  free bytes    : 1858560
  files:
    REC_001.ADP  60000
    REC_002.ADP  60000
    REC_003.ADP  60000
  io errors     : 0

---- verify files on volume -------------------
    REC_001.ADP  60000 B  crc32=0xC0BC0466  pattern=ok
    REC_002.ADP  60000 B  crc32=0xB6474878  pattern=ok
    REC_003.ADP  60000 B  crc32=0x12000169  pattern=ok
  files=3  bad=0  io_err=0

---- integrity check (direct from media) ------
  BPB signature : 0x55 AA
  nfats=2  fatsz=8  FAT1 LBA=64  FAT2 LBA=72
  FAT1 vs FAT2  : IDENTICAL (2面が一致)
```

CRC32 は書き込み時の値と一致。空きクラスタも電源断前と同一。
`D` で行った削除もすべて媒体に残っていた。

## この事象が証明すること

- **アイドル中の電源断ではファイルシステムが壊れない。**
  BPBシグネチャ、FAT2面の一致、全ファイルのCRC32とパターンが保たれた
- `f_unlink` によるFAT・ディレクトリの更新が媒体まで到達していた

## この事象が証明しないこと

- **書き込み中の電源断については何も言えない。** これが本来やりたかった試験。
  ファイルオープン中・クラスタ確保途中・FAT更新途中で切れた場合の挙動は未検証

## この事象から気づいた設計上の問題（未対処）

`diskio_flash.cpp` のブロック層は、書き込みを4KiBバッファに溜め、
次の別ブロックへのアクセスか `CTRL_SYNC` まで**媒体へ書かない**。

今回 `D`（削除）の直後に `M`（マウント＝読み出し）を実行しており、
`disk_read` が先頭で `flushBlock()` を呼ぶ設計になっているため、
結果的に削除が媒体へ確定していた。

**もし `D` の直後に電源が切れていたら、削除は失われていた可能性が高い。**

Phase 3 では、録音の区切りやメタデータ更新のあとに
明示的な `f_sync`（＝`CTRL_SYNC`）を入れる必要がある。
「読み出しのついでに書き出される」ことに依存してはいけない。
