# f_mkfs ジオメトリ検証ハーネス（ホスト側・実機に触れない）

2MiB 媒体に `f_mkfs()` が実際に作るFATのジオメトリを、
XIAO のフラッシュを消さずに測るためのもの。

## なぜ必要か

v1 は USB マスストレージで録音を取り出すので FAT が要る。
しかし 2MiB では:

- コア同梱の SdFat `FatFormatter` は 6MB 以下を拒否する（`FatFormatter.cpp:55-57`）
- 一方 FatFs の `f_mkfs()` は使える（`SdFat_format.ino:27-33`）

そこで `f_mkfs` に何を渡すとどんな配置になるかを、
**実機を消す前に**ホスト上で確定させる。

## 出所とライセンス

`fatfs/` は下記からのコピー。

```
~/Library/Arduino15/packages/Seeeduino/hardware/nrf52/1.1.13/
  libraries/Adafruit_SPIFlash/examples/SdFat_format/{ff.c,ff.h,ffconf.h,diskio.h}
```

FatFs **R0.13c** (Copyright (C) 2018, ChaN)。
再配布は著作権表示の保持のみを条件とするライセンスで、`ff.c` 冒頭の表示はそのまま残してある。

### 上流との差分

`ff.c` の1箇所のみ。`f_mkfs()` 内で `const` にハードコードされている
FAT面数をコンパイル時に切り替えられるようにした。

```c
#ifndef MKFS_N_FATS
#define MKFS_N_FATS 1   /* pebble_ring: only local change. upstream default is 1 */
#endif
	const UINT n_fats = MKFS_N_FATS;
```

差分の確認:

```bash
diff ~/Library/Arduino15/packages/Seeeduino/hardware/nrf52/1.1.13/libraries/Adafruit_SPIFlash/examples/SdFat_format/ff.c tools/fatfs_host/fatfs/ff.c
```

## 実機との対応

`diskio_file.c` の ioctl は基板側（`SdFat_format.ino:191-201`）と同じ値を返す。

| ioctl | 値 |
|---|---|
| `GET_SECTOR_COUNT` | 4096（= 2,097,152 / 512） |
| `GET_SECTOR_SIZE` | 512 |
| `GET_BLOCK_SIZE` | **8**（消去ブロック = 4KiB） |

`GET_BLOCK_SIZE` は `f_mkfs` がデータ開始位置を消去境界へ整列させるのに使う
（`ff.c:5820-5830`）。ここを実機と揃えないと配置が変わるので注意。

媒体は 0xFF で埋めて NOR の消去状態を模している。

## 使い方

```bash
cd tools/fatfs_host && make run
```

`make run` は n_fats=1（上流既定）と n_fats=2（採用案）の両方で、
`FM_FAT|FM_SFD` / `FM_FAT` × `au=1024` / `au=512` を生成し、
BPB を読み戻して物理LBA配置・クラスタ数・4KiB境界への整列を表示する。

生成イメージの出力先は `make run OUT=<dir>` で変えられる。

## 実測で採用した構成

**`FM_FAT`（MBR付き）+ `n_fats=2` + `au=1024`**

| 項目 | 値 |
|---|---|
| 物理LBA | MBR 0 / BPB 63 / FAT1 64 / FAT2 72 / root 80 / data 112 |
| クラスタ | 1992 個 × 1KiB（FAT12、境界4085まで2093の余裕） |
| データ領域 | 2,039,808 B |
| 8000 B/s 換算 | 254.976 秒 |

BPB・FAT1・FAT2・root・data が**それぞれ別の4KiB消去ブロック**に入る。
`reserved` は 1 のままなので互換性の心配もない。

macOS で `hdiutil attach` すると `DOS_FAT_12` として認識してマウントできることを確認済み。
