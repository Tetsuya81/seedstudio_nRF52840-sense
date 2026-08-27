#pragma once
#include <Arduino.h>
#include "config.h"

// オンボードRGB LED (LED_RED=11 / LED_GREEN=13 / LED_BLUE=12) の
// パターン表示。指輪では唯一のフィードバック手段になるので、
// 状態が一目で分かる配色にしておく。
class StatusLed {
public:
  enum class Pattern : uint8_t {
    Off,            // スリープ
    HeartbeatGreen, // 待機中: 2秒に1回ちらっと緑
    SolidRed,       // 録音中: 赤点灯
    BlinkAmber,     // 一時停止/警告
  };

  void begin() {
    pinMode(LED_RED, OUTPUT);
    pinMode(LED_GREEN, OUTPUT);
    pinMode(LED_BLUE, OUTPUT);
    activeLow_ = kLedActiveLowDefault;
    write(false, false, false);
  }

  void setPattern(Pattern p) { pattern_ = p; patternSinceMs_ = millis(); }
  Pattern pattern() const { return pattern_; }

  // 一時的に別の色を割り込ませる (マーカー挿入時の青フラッシュなど)
  void flash(bool r, bool g, bool b, uint16_t ms) {
    flashR_ = r; flashG_ = g; flashB_ = b;
    flashUntilMs_ = millis() + ms;
  }

  void update(uint32_t now) {
    if ((int32_t)(flashUntilMs_ - now) > 0) {
      write(flashR_, flashG_, flashB_);
      return;
    }
    switch (pattern_) {
      case Pattern::Off:
        write(false, false, false);
        break;
      case Pattern::HeartbeatGreen: {
        uint32_t phase = (now - patternSinceMs_) % 2000;
        write(false, phase < 40, false);
        break;
      }
      case Pattern::SolidRed:
        write(true, false, false);
        break;
      case Pattern::BlinkAmber: {
        uint32_t phase = (now - patternSinceMs_) % 800;
        bool on = phase < 400;
        write(on, on, false);
        break;
      }
    }
  }

  // 実機で点灯/消灯が逆に見えた場合の実行時反転
  void toggleActiveLow() { activeLow_ = !activeLow_; }
  bool activeLow() const { return activeLow_; }

  // 配線確認用のセルフテスト (ブロッキング)
  void selfTest() {
    const bool seq[4][3] = {{1,0,0},{0,1,0},{0,0,1},{1,1,1}};
    for (uint8_t i = 0; i < 4; i++) {
      write(seq[i][0], seq[i][1], seq[i][2]);
      delay(350);
    }
    write(false, false, false);
  }

private:
  void write(bool r, bool g, bool b) {
    uint8_t on  = activeLow_ ? LOW : HIGH;
    uint8_t off = activeLow_ ? HIGH : LOW;
    digitalWrite(LED_RED,   r ? on : off);
    digitalWrite(LED_GREEN, g ? on : off);
    digitalWrite(LED_BLUE,  b ? on : off);
  }

  Pattern  pattern_        = Pattern::Off;
  uint32_t patternSinceMs_ = 0;
  bool     activeLow_      = true;
  bool     flashR_ = false, flashG_ = false, flashB_ = false;
  uint32_t flashUntilMs_   = 0;
};
