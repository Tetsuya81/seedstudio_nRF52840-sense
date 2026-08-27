#pragma once
#include "ButtonSource.h"

// 実ボタン (Phase 6 で使用)
//
// まだ配線していないので既定では使わない。配線後は .ino の
// g_button を差し替えるだけでよい。タッチ電極に変える場合も
// このクラスと同じインターフェースを実装すれば済む。
class GpioButtonSource : public ButtonSource {
public:
  GpioButtonSource(uint8_t pin, bool activeLow = true)
      : pin_(pin), activeLow_(activeLow) {}

  void begin() override {
    pinMode(pin_, activeLow_ ? INPUT_PULLUP : INPUT_PULLDOWN);
  }

  bool isDown() const override {
    return digitalRead(pin_) == (activeLow_ ? LOW : HIGH);
  }

  const char* name() const override { return "gpio (physical button)"; }

private:
  uint8_t pin_;
  bool    activeLow_;
};
