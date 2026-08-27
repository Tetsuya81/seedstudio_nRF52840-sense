#pragma once
#include <Arduino.h>

// ボタンの「意味づけされた」イベント。
// アプリ側 (RecorderApp) はこれだけを見る。
// 生の電気信号 (Down/Up) を見るのは PressDetector まで。
enum class ButtonEvent : uint8_t {
  None = 0,
  Down,         // 物理的に押された (デバウンス後)
  Up,           // 物理的に離された
  ShortPress,   // 短押し確定 (ダブル押し待ち時間の経過後)
  DoublePress,  // 2回連続の短押し
  LongPress,    // 押しっぱなしが kLongPressMs を超えた瞬間
};

inline const char* toString(ButtonEvent e) {
  switch (e) {
    case ButtonEvent::Down:        return "Down";
    case ButtonEvent::Up:          return "Up";
    case ButtonEvent::ShortPress:  return "ShortPress";
    case ButtonEvent::DoublePress: return "DoublePress";
    case ButtonEvent::LongPress:   return "LongPress";
    default:                       return "None";
  }
}
