#pragma once
#include <Arduino.h>
#include "ButtonEvent.h"
#include "StatusLed.h"
#include "config.h"

// 録音デバイスの状態機械。
//
// Phase 1 では録音そのものは行わず、状態遷移とLED/ログだけを扱う。
// Phase 3 で startCapture()/stopCapture() の中身を PDMマイク +
// フラッシュ書き込みに差し替える。状態機械は変えずに済むようにしてある。
//
//   SLEEP  --長押し-->  IDLE
//   IDLE   --短押し-->  RECORDING
//   IDLE   --長押し-->  SLEEP
//   REC    --短押し-->  IDLE          (停止して保存)
//   REC    --ダブル-->  REC           (マーカー挿入)
//   REC    --長押し-->  SLEEP         (停止して保存 + スリープ)
class RecorderApp {
public:
  enum class State : uint8_t { Sleep, Idle, Recording };

  void begin(StatusLed* led, Stream* out) {
    led_ = led;
    out_ = out;
    setState(State::Idle, millis());
  }

  void handle(ButtonEvent e, uint32_t now) {
    if (e == ButtonEvent::None) return;

    // Down/Up は生の信号。既定では冗長なので verbose のときだけ出す。
    if (e == ButtonEvent::Down || e == ButtonEvent::Up) {
      if (verbose_) logLine(now, "raw", toString(e));
      return;
    }

    switch (state_) {
      case State::Sleep:
        if (e == ButtonEvent::LongPress) {
          logLine(now, toString(e), "wake -> IDLE");
          setState(State::Idle, now);
        } else {
          logLine(now, toString(e), "ignored (sleeping)");
        }
        break;

      case State::Idle:
        if (e == ButtonEvent::ShortPress) {
          startCapture(now);
        } else if (e == ButtonEvent::LongPress) {
          logLine(now, toString(e), "-> SLEEP");
          setState(State::Sleep, now);
        } else {
          logLine(now, toString(e), "ignored (idle)");
        }
        break;

      case State::Recording:
        if (e == ButtonEvent::ShortPress) {
          stopCapture(now, "stop -> IDLE");
          setState(State::Idle, now);
        } else if (e == ButtonEvent::LongPress) {
          stopCapture(now, "stop -> SLEEP");
          setState(State::Sleep, now);
        } else if (e == ButtonEvent::DoublePress) {
          markers_++;
          led_->flash(false, false, true, 200);
          char msg[48];
          snprintf(msg, sizeof(msg), "marker #%u at %lus",
                   (unsigned)markers_, (unsigned long)((now - recStartMs_) / 1000));
          logLine(now, toString(e), msg);
        }
        break;
    }
  }

  void update(uint32_t now) {
    if (state_ == State::Recording && (now - lastTickMs_) >= kRecordingTickMs) {
      lastTickMs_ = now;
      char msg[48];
      snprintf(msg, sizeof(msg), "recording... %lus",
               (unsigned long)((now - recStartMs_) / 1000));
      logLine(now, "tick", msg);
    }
  }

  void printStatus(uint32_t now, const char* buttonSourceName) {
    out_->println(F("---- status ----------------------------------"));
    out_->print(F("  state        : ")); out_->println(stateName(state_));
    out_->print(F("  uptime       : ")); out_->print(now / 1000.0f, 1); out_->println(F(" s"));
    out_->print(F("  button source: ")); out_->println(buttonSourceName);
    out_->print(F("  sessions     : ")); out_->println(sessions_);
    out_->print(F("  total rec    : ")); out_->print(totalRecMs_ / 1000.0f, 1); out_->println(F(" s"));
    if (state_ == State::Recording) {
      out_->print(F("  current rec  : "));
      out_->print((now - recStartMs_) / 1000.0f, 1); out_->println(F(" s"));
      out_->print(F("  markers      : ")); out_->println(markers_);
    }
    out_->print(F("  led active   : ")); out_->println(led_->activeLow() ? F("LOW") : F("HIGH"));
    out_->print(F("  verbose      : ")); out_->println(verbose_ ? F("on") : F("off"));
    out_->println(F("----------------------------------------------"));
  }

  void setVerbose(bool v) { verbose_ = v; }
  bool verbose() const { return verbose_; }
  State state() const { return state_; }

  static const char* stateName(State s) {
    switch (s) {
      case State::Sleep:     return "SLEEP";
      case State::Idle:      return "IDLE";
      case State::Recording: return "RECORDING";
    }
    return "?";
  }

private:
  // Phase 3 でここが PDM開始 + フラッシュ書き込みになる
  void startCapture(uint32_t now) {
    sessions_++;
    markers_ = 0;
    recStartMs_ = now;
    lastTickMs_ = now;
    char msg[48];
    snprintf(msg, sizeof(msg), "start -> RECORDING (session #%u)", (unsigned)sessions_);
    logLine(now, "ShortPress", msg);
    setState(State::Recording, now);
  }

  // Phase 3 でここが PDM停止 + WAVクローズになる
  void stopCapture(uint32_t now, const char* what) {
    uint32_t dur = now - recStartMs_;
    totalRecMs_ += dur;
    char msg[80];
    snprintf(msg, sizeof(msg), "%s (%lu.%03lus, %u markers)",
             what, (unsigned long)(dur / 1000), (unsigned long)(dur % 1000),
             (unsigned)markers_);
    logLine(now, "press", msg);
  }

  void setState(State s, uint32_t now) {
    state_ = s;
    switch (s) {
      case State::Sleep:     led_->setPattern(StatusLed::Pattern::Off); break;
      case State::Idle:      led_->setPattern(StatusLed::Pattern::HeartbeatGreen); break;
      case State::Recording: led_->setPattern(StatusLed::Pattern::SolidRed); break;
    }
    (void)now;
  }

  void logLine(uint32_t now, const char* kind, const char* msg) {
    char buf[128];
    // newlib-nano の printf は %f を落とすことがあるので整数で組む
    snprintf(buf, sizeof(buf), "[%5lu.%03lu] %-9s %-12s %s",
             (unsigned long)(now / 1000), (unsigned long)(now % 1000),
             stateName(state_), kind, msg);
    out_->println(buf);
  }

  StatusLed* led_ = nullptr;
  Stream*    out_ = nullptr;
  State      state_ = State::Idle;
  uint32_t   recStartMs_ = 0;
  uint32_t   lastTickMs_ = 0;
  uint32_t   totalRecMs_ = 0;
  uint16_t   sessions_ = 0;
  uint16_t   markers_  = 0;
  bool       verbose_  = false;
};
