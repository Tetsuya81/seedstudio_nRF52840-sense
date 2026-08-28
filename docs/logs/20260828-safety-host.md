# 2026-08-28 安全対策：ホスト検証ログ

実行者：ChatGPT。**実機操作・upload・mount・USB再列挙・電源断なし。**
NORモデル試験の成功は、電気的安全性や実機電源断時の耐性を証明しない。

## メモリNOR＋実際のFatFs/diskio（ASan/UBSan）

```sh
bash tools/storage_safety/run.sh
```

```text
PASS ranges: overflow/zero/null/drive/end-of-medium
PASS attach: pending data preserved, replacement refused
PASS faults: erase/write/read/verify latch and no destructive retry
PASS unlink: deletion persisted before any subsequent read/remount
PASS unlink: lower-layer failure reaches FatFs
PASS model counterexample: later erase can destroy earlier synced data
ALL HOST STORAGE TESTS PASSED (no physical device accessed)
```

終了値0、AddressSanitizer/UndefinedBehaviorSanitizerの指摘なし。
最後のPASSは「安全性が証明された」ではなく、**以前の同期済みデータを失う反例を再現できた**という意味。
FatFsはfirmware側R0.13c、diskioもfirmware側をそのままコンパイル。媒体APIだけを模擬化した。

## 実機アクセス停止ガード

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tools/storage_safety/test_guards.py
```

```text
....
Ran 4 tests in 0.124s
OK
```

- serial：os.openより前に拒否。
- ポート自動検出：globより前に拒否。
- upload/monitor：PORT指定があってもArduino CLIより前に拒否。
- build：upload/portオプションを拒否。

Arduino CLIはシェル関数スタブへ差し替え、シリアルopenはモック化している。
テスト失敗時にも実機へアクセスしない構成。

## ビルド（実機へ書き込まない）

```sh
trial_build=$(mktemp -d /private/tmp/pebble-safe-build.XXXXXX)
bash scripts/build.sh --jobs 1 --build-path "$trial_build"
```

```text
Sketch uses 83232 bytes (10%) of program storage space. Maximum is 811008 bytes.
Global variables use 14128 bytes (5%) of dynamic memory, leaving 223440 bytes for local variables. Maximum is 237568 bytes.
```

終了値0。FQBN：Seeeduino:nrf52:xiaonRF52840Sense、インストール済みcore 1.1.13。
安全隔離で未使用機能が最適化除去されるため、将来MSCを再有効化した際のサイズとは異なる。

## その他

`bash -n scripts/build.sh scripts/common.sh scripts/upload.sh scripts/monitor.sh tools/storage_safety/run.sh` は終了値0。
`git diff --check` はエラーなし。
テストの一時ビルド成果物は新規一時ディレクトリに保持。既存データの削除はしていない。

## 未検証

- Mac停止の直接原因、USB電気特性、OSドライバ安定性。
- 修正ファームの実機動作、書込み中の実電源断、充電中録音。
- TestDataの全エラー分岐の動的試験（コード修正とArduinoコンパイルは実施）。
- MSC再開時の状態遷移・復旧設計。現在はHOLDで実行禁止。

これらは [事故対応・再開条件](../safety/20260828-incident.md) に従って別途扱う。
