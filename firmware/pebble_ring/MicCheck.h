#pragma once
#include <Arduino.h>
#include <PDM.h>

// マイクの実測チェック。
//
// 「Sense版かどうか」を IMU 経由で調べようとして失敗したので、
// 目的そのもの（マイクが鳴るか）を直接測る。
// PDM.begin() が通ってサンプルが流れてくれば Sense版で確定する。
//
// これは Phase 2（マイク性能確認）の土台でもある。
// 出力: サンプル数 / DCオフセット / RMS / ピーク / dBFS
//
// ピンは variant の PIN_PDM_DIN(21) / PIN_PDM_CLK(20) / PIN_PDM_PWR(19) を
// PDM ライブラリが自動で使う。
namespace MicCheck {

static volatile int   g_samplesRead = 0;
static short          g_isrBuf[256];

inline void onData() {
  int bytes = PDM.available();
  if (bytes > (int)sizeof(g_isrBuf)) bytes = sizeof(g_isrBuf);
  PDM.read((void*)g_isrBuf, bytes);
  g_samplesRead = bytes / 2;
}

inline void run(Stream& out, uint32_t durMs = 500, int gain = 20) {
  out.println(F("---- mic check --------------------------------"));

  g_samplesRead = 0;
  PDM.onReceive(onData);
  PDM.setGain(gain);

  if (!PDM.begin(1, 16000)) {
    out.println(F("  PDM.begin() FAILED"));
    out.println(F("  -> microphone not available (non-Sense board?)"));
    out.println(F("----------------------------------------------"));
    return;
  }
  out.print(F("  started      : 1ch / 16000Hz / gain "));
  out.println(gain);

  int32_t  minv = 32767, maxv = -32768;
  int32_t  sum = 0;
  double   sumsq = 0;
  uint32_t n = 0;
  static short local[256];

  uint32_t t0 = millis();
  while ((millis() - t0) < durMs) {
    if (g_samplesRead) {
      noInterrupts();
      int cnt = g_samplesRead;
      if (cnt > 256) cnt = 256;
      memcpy(local, (const void*)g_isrBuf, (size_t)cnt * 2);
      g_samplesRead = 0;
      interrupts();

      for (int i = 0; i < cnt; i++) {
        int32_t v = local[i];
        if (v < minv) minv = v;
        if (v > maxv) maxv = v;
        sum += v;
        sumsq += (double)v * (double)v;
        n++;
      }
    }
    yield();
  }
  PDM.end();

  out.print(F("  samples      : ")); out.print(n);
  out.print(F("  in ")); out.print(durMs); out.println(F(" ms"));

  if (n == 0) {
    out.println(F("  -> PDM started but NO samples arrived. check wiring/power."));
    out.println(F("----------------------------------------------"));
    return;
  }

  int32_t mean = sum / (int32_t)n;
  double  rms  = sqrt(sumsq / (double)n);
  int32_t peak = max(abs(minv), abs(maxv));

  out.print(F("  dc offset    : ")); out.println(mean);
  out.print(F("  rms          : ")); out.println((int32_t)rms);
  out.print(F("  peak         : ")); out.print(peak);
  out.print(F("  (min ")); out.print(minv);
  out.print(F(" / max ")); out.print(maxv); out.println(F(")"));

  if (rms > 0) {
    out.print(F("  noise floor  : "));
    out.print(20.0f * log10f((float)rms / 32768.0f), 1);
    out.println(F(" dBFS"));
  }
  out.print(F("  peak level   : "));
  out.print(20.0f * log10f((float)max(peak, (int32_t)1) / 32768.0f), 1);
  out.println(F(" dBFS"));

  out.println(F("  -> PDM microphone works. This IS a Sense board."));
  out.println(F("----------------------------------------------"));
}

}  // namespace MicCheck
