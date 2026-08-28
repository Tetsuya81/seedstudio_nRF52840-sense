#pragma once
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include "FlashCheck.h"
#include "FormatCheck.h"
#include "SafetyPolicy.h"

// USB マスストレージでフラッシュを Mac に見せる。
//
// v1 は **ホストから read-only** にする。理由は実測にもとづく:
// macOS が FAT 領域に書くと AppleDouble `._<name>` が1ファイルあたり
// 4,096 B 作られ、2MiB しかない媒体では無視できない（chat.md F-24）。
// `.Trashes` や `.Spotlight-V100` も同様。
//
// read-only の実現方法は tud_msc_is_writable_cb() を false にすること。
// これで Mode Sense の write_protected が立ち、macOS は書き込み不可として
// マウントする（msc_device.c:775-785）。書き込みが来ても
// SCSI_SENSE_DATA_PROTECT で失敗させる（:856-869）。
//
// 排他: ホストへ見せている間は基板側から書かない。
// 見せる前に必ずアンマウントとフラッシュを行う。
namespace MscBridge {

extern "C" int flashio_flush(void);

static Adafruit_USBD_MSC g_msc;
static bool     g_exposed      = false;
inline bool isExposed() { return g_exposed; }
static uint32_t g_hostReads    = 0;
static uint32_t g_writeAttempts = 0;      // ホストが書こうとした回数

inline int32_t onRead(uint32_t lba, void *buffer, uint32_t bufsize) {
  if (SafetyPolicy::kHardwareQuarantine || !g_exposed || !buffer ||
      !SafetyPolicy::byteRange(lba, bufsize, FlashCheck::g_flash.size())) return -1;
  if (FlashCheck::g_flash.readBuffer(lba * 512, (uint8_t *)buffer, bufsize) != bufsize)
    return -1;
  g_hostReads++;
  return (int32_t)bufsize;
}

// read-only なので本来ここには来ない。来たら記録して拒否する。
inline int32_t onWrite(uint32_t lba, uint8_t *buffer, uint32_t bufsize) {
  (void)lba; (void)buffer; (void)bufsize;
  g_writeAttempts++;
  return -1;
}

inline void onFlush(void) { /* ホスト書き込みは無いので何もしない */ }

inline bool onReady(void) { return g_exposed; }

// USB列挙前に呼ぶので何も出力しない。
inline void beginQuiet(void) {
  if (SafetyPolicy::kHardwareQuarantine) return;
  g_msc.setID("Seeed", "pebble_ring", "1.0");
  g_msc.setCapacity(FlashCheck::g_flash.size() / 512, 512);
  g_msc.setReadWriteCallback(onRead, onWrite, onFlush);
  g_msc.setReadyCallback(onReady);
  // 起動時から ready にしておく。
  // macOS は列挙時の初回プローブでメディア無しと判定すると、
  // その後 setUnitReady(true) にしても自発的に再プローブしない（実測）。
  // ホストからは read-only なので、常時見せていても書き荒らされることはない。
  g_msc.setUnitReady(true);
  g_exposed = true;
  g_msc.begin();
}

// ホストへ見せる。基板側のFSはアンマウントして書き出しを確定させる。
inline void expose(Stream& out) {
  if (SafetyPolicy::kHardwareQuarantine) {
    out.println(F("HARDWARE HOLD: MSC expose/re-enumeration blocked."));
    return;
  }
  out.println(F("---- expose to host ---------------------------"));
  f_mount(NULL, "", 0);                     // 基板側をアンマウント
  int ok = flashio_flush();                 // 未書き出しを確定
  out.print(F("  flush        : "));
  out.println(ok ? F("ok") : F("FAILED"));
  if (!ok) {
    out.println(F("  -> not exposing. stopped."));
    out.println(F("----------------------------------------------"));
    return;
  }
  g_exposed = true;
  g_msc.setUnitReady(true);
  // macOS にメディアの出現を認識させるには再列挙が要る（実測）。
  // detach/attach は CDC も切るので、シリアル接続は張り直しになる。
  USBDevice.detach();
  delay(300);
  USBDevice.attach();
  out.println(F("  unit ready   : true  (READ-ONLY for host)"));
  out.println(F("  -> USB を再列挙しました。Mac 側でボリュームが見えます。"));
  out.println(F("----------------------------------------------"));
}

inline void hide(Stream& out) {
  if (SafetyPolicy::kHardwareQuarantine) {
    out.println(F("HARDWARE HOLD: USB re-enumeration blocked."));
    return;
  }
  out.println(F("---- hide from host ---------------------------"));
  g_exposed = false;
  g_msc.setUnitReady(false);
  USBDevice.detach();
  delay(300);
  USBDevice.attach();
  out.print(F("  host reads   : "));
  out.println(g_hostReads);
  out.print(F("  write attempts by host : "));
  out.println(g_writeAttempts);
  out.println(F("  -> 'M' で基板側に再マウントできます。"));
  out.println(F("----------------------------------------------"));
}

inline void status(Stream& out) {
  out.print(F("  msc exposed  : "));
  out.println(g_exposed ? F("true (read-only)") : F("false"));
  out.print(F("  host reads   : "));
  out.println(g_hostReads);
  out.print(F("  host writes  : "));
  out.println(g_writeAttempts);
}

}  // namespace MscBridge

// TinyUSB へ「書き込み不可」を伝える。
// これで macOS は read-only でマウントする (msc_device.c:775-785)。
extern "C" bool tud_msc_is_writable_cb(uint8_t lun) {
  (void)lun;
  return false;   // v1 はホストから read-only
}
