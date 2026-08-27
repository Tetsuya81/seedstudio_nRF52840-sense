#pragma once
#include <Arduino.h>
#include <Wire.h>

// 基板の自己診断。
//
// XIAO nRF52840 には Sense版 (IMU + PDMマイク搭載) と 非Sense版 (どちらも無し)
// があり、シルク印刷では見分けづらい。Sense版だけに載っている
// LSM6DS3TR-C の WHO_AM_I を読んで確実に判別する。
// マイクがあるかどうかはこのプロジェクトの前提そのものなので、
// 早い段階で確定させておく。
namespace BoardCheck {

static const uint8_t kLsm6ds3Addr1   = 0x6A;
static const uint8_t kLsm6ds3Addr2   = 0x6B;
static const uint8_t kRegWhoAmI      = 0x0F;
static const uint8_t kLsm6ds3trcWhoAmI = 0x6A;

inline bool readReg(uint8_t addr, uint8_t reg, uint8_t& out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, (uint8_t)1) != 1) return false;
  out = (uint8_t)Wire.read();
  return true;
}

inline void run(Stream& out) {
  out.println(F("---- board check ------------------------------"));

  // Sense版はIMUとマイクの電源が P1.08 (PIN_PDM_PWR=19) で制御される
  pinMode(PIN_PDM_PWR, OUTPUT);
  digitalWrite(PIN_PDM_PWR, HIGH);
  delay(50);

  Wire.begin();
  delay(10);

  out.print(F("  i2c devices  :"));
  uint8_t found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      out.print(F(" 0x"));
      if (a < 16) out.print('0');
      out.print(a, HEX);
      found++;
    }
  }
  if (!found) out.print(F(" (none)"));
  out.println();

  uint8_t who = 0;
  bool imu = false;
  if (readReg(kLsm6ds3Addr1, kRegWhoAmI, who) || readReg(kLsm6ds3Addr2, kRegWhoAmI, who)) {
    imu = (who == kLsm6ds3trcWhoAmI);
  }

  out.print(F("  IMU WHO_AM_I : 0x"));
  if (who < 16) out.print('0');
  out.println(who, HEX);
  out.print(F("  variant      : "));
  if (imu) {
    out.println(F("Sense  (LSM6DS3TR-C detected -> PDM mic present)"));
  } else {
    out.println(F("NOT Sense?  (no IMU -> no on-board microphone)"));
  }
  out.println(F("----------------------------------------------"));
}

}  // namespace BoardCheck
