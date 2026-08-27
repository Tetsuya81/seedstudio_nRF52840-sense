#pragma once
#include <Arduino.h>

// =============================================================
//  pebble_ring - 共通設定
//  Phase 1: USB接続 + 模擬ボタン
// =============================================================

// ---- ボタンのタイミング定義 -----------------------------------
// 模擬ボタン / 実ボタン のどちらでも同じ値が使われる。
// ここを調整すれば指輪の実機の押し心地に合わせられる。
static const uint32_t kDebounceMs  = 25;    // チャタリング除去
static const uint32_t kLongPressMs = 800;   // 長押し判定しきい値
static const uint32_t kDoubleGapMs = 350;   // ダブル押しの最大間隔

// ---- 模擬ボタンが注入する押下時間 ------------------------------
static const uint32_t kSimShortMs  = 80;    // 'p' が生成する押下時間
static const uint32_t kSimLongMs   = 1200;  // 'l' が生成する押下時間
static const uint32_t kSimDoubleGapMs = 150;// 'd' の2回押しの間隔

// ---- LED ------------------------------------------------------
// XIAO nRF52840 のオンボードRGB LEDはコモンアノード = LOWで点灯。
// もし実機で点灯/消灯が逆に見えたら 'i' コマンドで実行時に反転できる。
static const bool kLedActiveLowDefault = true;

// ---- 将来の実ボタン (Phase 6) ---------------------------------
// まだ配線していない。GPIOボタンに切り替えるときはここを実際の
// パッドに合わせる。GND側に落とす前提 (INPUT_PULLUP)。
#define PIN_USER_BUTTON   D1

// ---- 動作ログ --------------------------------------------------
static const uint32_t kRecordingTickMs = 5000;  // 録音中の経過表示間隔
