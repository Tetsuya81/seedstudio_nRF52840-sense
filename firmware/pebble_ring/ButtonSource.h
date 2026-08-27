#pragma once
#include <Arduino.h>

// ボタン入力の抽象化レイヤ。
//
// Phase 1 では VirtualButtonSource (USBシリアル経由の模擬ボタン) を使い、
// Phase 6 で GpioButtonSource (実際のタクトスイッチ/タッチ電極) に
// 差し替える。アプリ側のコードは一切変更しない。
//
// 実装は「今ボタンが押されているか」という電気的な状態だけを返す。
// デバウンスや長押し判定は PressDetector が共通で担当する。
class ButtonSource {
public:
  virtual ~ButtonSource() {}
  virtual void begin() {}
  virtual void poll(uint32_t now) { (void)now; }
  virtual bool isDown() const = 0;
  virtual const char* name() const = 0;
};
