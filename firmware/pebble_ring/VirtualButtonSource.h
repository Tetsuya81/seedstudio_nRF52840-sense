#pragma once
#include "ButtonSource.h"
#include "config.h"

// 模擬ボタン (Phase 1)
//
// USBシリアルのコマンドから「押した/離した」を時間軸上にスケジュールし、
// 実際のタクトスイッチと同じ電気的な波形を再現する。
// そのため PressDetector のデバウンス・長押し・ダブル押し判定は
// 実機のボタンに置き換えたときとまったく同じ経路で検証される。
class VirtualButtonSource : public ButtonSource {
public:
  void begin() override { down_ = false; count_ = 0; idx_ = 0; }

  void poll(uint32_t now) override {
    while (idx_ < count_ && (int32_t)(now - steps_[idx_].at) >= 0) {
      down_ = steps_[idx_].down;
      idx_++;
    }
    if (idx_ >= count_) { count_ = 0; idx_ = 0; }
  }

  bool isDown() const override { return down_; }
  const char* name() const override { return "virtual (USB serial)"; }

  // --- コマンドから呼ばれる注入API ---

  // 指定時間だけ押して離す
  void pressFor(uint32_t ms) {
    uint32_t t = millis();
    clear();
    push(t, true);
    push(t + ms, false);
  }

  // 短押しを2回続けて注入する
  void pressTwice(uint32_t ms, uint32_t gapMs) {
    uint32_t t = millis();
    clear();
    push(t, true);
    push(t + ms, false);
    push(t + ms + gapMs, true);
    push(t + ms + gapMs + ms, false);
  }

  // 手動で押しっぱなし / 離す (任意の時間を自分で測りたいとき)
  void hold()    { clear(); down_ = true; }
  void release() { clear(); down_ = false; }

  bool busy() const { return idx_ < count_; }

private:
  struct Step { uint32_t at; bool down; };

  void clear() { count_ = 0; idx_ = 0; }
  void push(uint32_t at, bool down) {
    if (count_ < kMaxSteps) { steps_[count_].at = at; steps_[count_].down = down; count_++; }
  }

  static const uint8_t kMaxSteps = 8;
  Step    steps_[kMaxSteps];
  uint8_t count_ = 0;
  uint8_t idx_   = 0;
  bool    down_  = false;
};
