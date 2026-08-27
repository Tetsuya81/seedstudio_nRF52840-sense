#pragma once
#include "ButtonEvent.h"
#include "config.h"

// 生のボタン状態 (押されている/いない) から、
// デバウンス済みの意味あるイベントを生成する。
//
// 模擬ボタンでも実ボタンでもこのクラスを通るので、
// Phase 1 でここを詰めておけば実機化のときに操作感が変わらない。
//
// 判定の流れ:
//   ・kDebounceMs 安定してから状態変化を確定
//   ・押しっぱなしが kLongPressMs を超えた「瞬間」に LongPress を発火
//     (指輪では押し続けたまま反応が返るほうが分かりやすいため)
//   ・離してから kDoubleGapMs 以内に次の押下があれば DoublePress
//   ・なければ ShortPress (= kDoubleGapMs だけ遅延して確定)
class PressDetector {
public:
  void begin(bool initialDown, uint32_t now) {
    rawDown_ = stableDown_ = initialDown;
    lastChangeMs_ = now;
    head_ = tail_ = 0;
  }

  void update(bool rawDown, uint32_t now) {
    // --- デバウンス ---
    if (rawDown != rawDown_) {
      rawDown_ = rawDown;
      lastChangeMs_ = now;
    }
    if (rawDown_ != stableDown_ && (now - lastChangeMs_) >= kDebounceMs) {
      stableDown_ = rawDown_;
      if (stableDown_) onDown(now);
      else             onUp(now);
    }

    // --- 押しっぱなしの長押し判定 ---
    if (stableDown_ && !longFired_ && (now - pressStartMs_) >= kLongPressMs) {
      longFired_ = true;
      emit(ButtonEvent::LongPress);
    }

    // --- ダブル押し待ちのタイムアウト = 短押し確定 ---
    if (shortPending_ && (now - shortSinceMs_) > kDoubleGapMs) {
      shortPending_ = false;
      emit(ButtonEvent::ShortPress);
    }
  }

  // 溜まっているイベントを1つ取り出す。無ければ None。
  ButtonEvent take() {
    if (head_ == tail_) return ButtonEvent::None;
    ButtonEvent e = queue_[tail_];
    tail_ = (uint8_t)((tail_ + 1) % kQueueSize);
    return e;
  }

  bool isDown() const { return stableDown_; }
  uint32_t heldMs(uint32_t now) const { return stableDown_ ? (now - pressStartMs_) : 0; }

private:
  void onDown(uint32_t now) {
    pressStartMs_ = now;
    longFired_ = false;
    emit(ButtonEvent::Down);
  }

  void onUp(uint32_t now) {
    emit(ButtonEvent::Up);
    if (longFired_) {
      // 長押しは押した瞬間に発火済み。離しただけでは何も起きない。
      shortPending_ = false;
      return;
    }
    if (shortPending_ && (now - shortSinceMs_) <= kDoubleGapMs) {
      shortPending_ = false;
      emit(ButtonEvent::DoublePress);
    } else {
      shortPending_ = true;
      shortSinceMs_ = now;
    }
  }

  void emit(ButtonEvent e) {
    uint8_t next = (uint8_t)((head_ + 1) % kQueueSize);
    if (next == tail_) return;  // あふれたら捨てる (通常起こらない)
    queue_[head_] = e;
    head_ = next;
  }

  static const uint8_t kQueueSize = 8;
  ButtonEvent queue_[kQueueSize];
  uint8_t head_ = 0, tail_ = 0;

  bool     rawDown_      = false;
  bool     stableDown_   = false;
  uint32_t lastChangeMs_ = 0;
  uint32_t pressStartMs_ = 0;
  bool     longFired_    = false;
  bool     shortPending_ = false;
  uint32_t shortSinceMs_ = 0;
};
