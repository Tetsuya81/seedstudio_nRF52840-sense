# pebble_ring — XIAO nRF52840 Sense 指輪型録音デバイス

Seeed Studio XIAO nRF52840 **Sense** を使った、Pebble Index 1 のような
指輪型録音デバイスの開発リポジトリ。

まずUSB接続で各機能を検証し、最終的にバッテリ駆動の指輪型に仕上げる。
XIAO 本体は 21×17.5mm で指輪には入らないため、**指輪に載せる中身を決めるための
実験装置**として使い、最終形は自作基板に落とす。

XIAO 本体は 21×17.5mm。**v1 は幅の太い TPU リングに XIAO をそのまま収める**
ので自作基板は不要で、今あるハードだけで最後まで組める。
小型化（自作基板）は v1 で仕様が固まってからの v2 とする。

- 素材は Bambu Lab **TPU 95A HF**、操作は**物理ボタン**、状態表示は**RGB LED**
- 電池は基板脇のくさび空隙に約 **40mAh** → 録音 約5時間
- **USB-C を残す**ので書き込み・充電・録音の取り出しは従来どおり
- ただし 2MB フラッシュは ADPCM でも **4.4分**。v1 は電池より**容量が先に尽きる**

物理設計は [docs/ring-design.md](docs/ring-design.md)、
全体計画は [docs/roadmap.md](docs/roadmap.md) を参照。

## 現在のフェーズ

**Phase 1: 模擬ボタン (完了)** — 実ボタン未配線のため、USBシリアル経由で
ボタンの押下波形を注入して録音デバイスの状態機械とLED表示を動かす。

ロードマップ全体は [docs/roadmap.md](docs/roadmap.md) を参照。

## セットアップ

```bash
arduino-cli config set board_manager.additional_urls \
  https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json
arduino-cli core update-index
arduino-cli core install Seeeduino:nrf52
```

ボード: `Seeeduino:nrf52:xiaonRF52840Sense`（非mbed / Adafruit Bluefruit ベース）。
BLE・低消費電力・TinyUSBマスストレージが揃うため、mbed版ではなくこちらを使う。

> `adafruit-nrfutil` は ASCII ロケールだと起動に失敗する。
> `scripts/common.sh` が `LC_ALL=en_US.UTF-8` を設定するので、
> 必ずスクリプト経由でビルド/書き込みすること。

## 使い方

```bash
./scripts/build.sh      # ビルドのみ
./scripts/upload.sh     # ビルド + 書き込み（ポート自動検出）
./scripts/monitor.sh    # シリアルモニタ
```

書き込みに失敗する場合は、基板の RESET を素早く2回押してブートローダ
モードに入れてから再実行する。

### 模擬ボタンの操作

シリアル(115200)に1文字送るだけ。

| キー | 動作 |
|---|---|
| `p` | 短押し（80ms の押下波形を注入） |
| `l` | 長押し（1200ms） |
| `d` | ダブル押し（80ms×2、間隔150ms） |
| `[` / `]` | 押しっぱなし開始 / 離す（任意の長さを手で試す） |
| `s` | 状態表示 |
| `v` | 詳細ログ（生の Down/Up も表示） |
| `i` | LED極性を反転 |
| `x` | LEDセルフテスト（赤→緑→青→白） |
| `g` | ボタン入力を virtual ⇄ gpio 切替 |
| `m` | 基板の自己診断（Sense版かどうかを判定） |
| `h` | ヘルプ |

pyserial 不要のシリアルツールも同梱：

```bash
./scripts/serial_console.py                          # 垂れ流し表示
./scripts/serial_console.py -s "p,1.0,d,1.0,p,0.5,s" # コマンドと待ち秒数を順に実行
```

### 状態機械

```
SLEEP  --長押し-->  IDLE  --短押し-->  RECORDING
IDLE   --長押し-->  SLEEP
RECORDING --短押し--> IDLE       (停止して保存)
RECORDING --ダブル--> RECORDING  (マーカー挿入)
RECORDING --長押し--> SLEEP      (停止して保存 + スリープ)
```

LED: 消灯=SLEEP / 緑がちらつく=IDLE / 赤点灯=RECORDING / 青フラッシュ=マーカー

## 設計方針：ボタン入力の抽象化

指輪の実ボタンに移行するときにアプリ側を書き換えずに済むよう、
入力を3層に分けている。

```
ButtonSource (抽象)             ← 「今押されているか」だけを返す
  ├─ VirtualButtonSource        Phase 1: シリアルから押下波形を注入
  └─ GpioButtonSource           Phase 6: 実タクトスイッチ / タッチ電極
        ↓ 生の Down/Up
PressDetector                   デバウンス・長押し・ダブル押しの判定（共通）
        ↓ ShortPress / DoublePress / LongPress
RecorderApp                     状態機械・LED・ログ
```

模擬ボタンは「押した/離した」を時間軸上にスケジュールして**実際のスイッチと
同じ電気的な波形**を再現するので、`PressDetector` のデバウンスや長押し判定は
実機化したときとまったく同じ経路で検証される。実ボタン配線後は
`pebble_ring.ino` の `g_button` の指す先を変えるだけでよい（`g` コマンドで
実行時にも切替可能）。

## ハードウェア メモ

| 項目 | 値 |
|---|---|
| MCU | nRF52840 (Cortex-M4F, 1MB Flash / 256KB RAM) |
| USB | VID 0x2886 / PID 0x8045 |
| RGB LED | 赤=11 / 青=12 / 緑=13（コモンアノード = LOWで点灯） |
| PDMマイク | MSM261D3526H1CPM — PWR=19 / CLK=20 / DIN=21 |
| IMU | LSM6DS3TR-C (6軸) |
| 外部フラッシュ | 2MB QSPI (P25Q16H) |
| バッテリ | VBAT読み取り=32 / 読み取り有効化=14 (LOWで有効) |
| 充電IC | BQ25101（充電電流はP0.13で50/100mA切替） |

## ディレクトリ

```
firmware/pebble_ring/   Arduinoスケッチ（.ino + ヘッダオンリークラス群）
scripts/                ビルド・書き込み・シリアルツール
docs/                   ロードマップ・設計メモ
```
