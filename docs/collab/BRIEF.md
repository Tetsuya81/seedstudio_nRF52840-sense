# プロジェクト概要（ChatGPT に最初に貼るもの）

これは Claude Code と ChatGPT が協業するための前提共有ファイルです。
まず `RULES.md` と、このファイルを読んでください。

**2026-08-28更新：Mac突然停止の調査中につき実機アクセス停止。**
ChatGPTが実装・検証、Claudeが仕様・設計レビューを担当する。
`HARDWARE_HOLD` と [再開条件](../safety/20260828-incident.md) を優先し、
過去の実機試験手順をそのまま再開しない。

---

## 何を作っているか

**Seeed Studio XIAO nRF52840 Sense** を使った、**指輪型の録音デバイス**。
Pebble Index 1 のようなもの。ボタンを押して録音、LEDで状態表示。

リポジトリ: https://github.com/Tetsuya81/seedstudio_nRF52840-sense （プライベート）

**ユーザーは電子工作の初心者**です。断定するときは根拠を、
手順を示すときは前提から書いてください。

## v1 の構成（確定済み）

| 項目 | 内容 |
|---|---|
| 基板 | XIAO nRF52840 Sense を**そのまま**使う（自作基板なし） |
| 筐体 | Bambu Lab **TPU 95A HF** で幅の太いリングを3Dプリント |
| 配置 | 基板をリング頂部に収納、基板脇のくさび空隙にバッテリー |
| 操作 | **物理ボタン**（タクトスイッチ、D1–GND、INPUT_PULLUP） |
| 表示 | オンボードRGB LED のみ（ディスプレイなし＝消費電力のため） |
| USB-C | **残す**。書き込み・充電・録音の取り出しに使う |

小型化（ISP1807 での自作基板）は v1 完成後の v2 とする。

## 開発環境

- macOS / `arduino-cli 1.5.1`
- コア: `Seeeduino:nrf52 1.1.13`（非mbed、Adafruit Bluefruit ベース）
- FQBN: `Seeeduino:nrf52:xiaonRF52840Sense`
- `adafruit-nrfutil` が ASCII ロケールで落ちるため `LC_ALL=en_US.UTF-8` が必要

## ピンマップ（[一次資料] core の variant.cpp）

`~/Library/Arduino15/packages/Seeeduino/hardware/nrf52/1.1.13/variants/Seeed_XIAO_nRF52840_Sense/variant.cpp`

| 機能 | Arduinoピン | nRFポート |
|---|---|---|
| LED 赤 / 青 / 緑 | 11 / 12 / 13 | P0.26 / P0.06 / P0.30 |
| バッテリ読取 有効 | 14 | P0.14 |
| IMU 電源 | 15 | P1.08 |
| IMU I2C SCL / SDA (**Wire1**) | 16 / 17 | P0.27 / P0.07 |
| IMU INT1 | 18 | P0.11 |
| マイク 電源 / CLK / DATA | 19 / 20 / 21 | P1.10 / P1.00 / P0.16 |
| 充電電流 HICHG | 22 | P0.13 |
| 充電状態 ~CHG | 23 | P0.17 |
| QSPIフラッシュ | 24〜29 | P0.21/25/20/24/22/23 |
| VBAT | 32 | P0.31 |

> **注意**: 公式Wikiには「IMUはD4/D5」と読める記述があるが**誤り**。
> IMU は専用の `Wire1`（P0.27/P0.07）にいる。

## 進捗

- **Phase 0 環境構築** ✅
- **Phase 1 模擬ボタン** ✅ — USBシリアルから押下波形を注入して
  状態機械（SLEEP/IDLE/RECORDING）とLED表示を検証。
  入力は `ButtonSource` で抽象化済み（virtual ⇄ gpio を差し替え可能）
- **Phase 2 マイク性能確認** ← いまここ
- Phase 3 録音パイプライン → Phase 4 圧縮 → Phase 5 低消費電力 → Phase 6 組み立て

## v1 最大の制約

電池は基板脇に約40mAh入り録音5時間分あるが、**XIAOのフラッシュは2MB**しかない。

| 保存方式 | 2MB で録れる長さ |
|---|---|
| 無圧縮 PCM 16kHz/16bit | 65秒 |
| IMA ADPCM (4:1) | 4.4分 |
| Opus 16kbps | 17.5分 |

**電池より保存容量が先に尽きる。** ここが v1 の設計上いちばん効く。

## 詳しい資料

- `docs/roadmap.md` — 全体計画（v1/v2）
- `docs/ring-design.md` — 指輪の物理設計（寸法計算、バッテリー室、音道、アンテナ）
