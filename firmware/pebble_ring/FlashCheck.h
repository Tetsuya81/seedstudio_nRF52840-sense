#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_SPIFlash.h>

// オンボードQSPIフラッシュの実測。
//
// 容量は「2MB」と思い込んでいたが、コアの ExternalFileSystem.cpp:19-22 は
// P25Q32H (total_size = 1UL << 22 = 4MiB) として宣言している。
// 録音できる長さの計算がまるごと変わるので、JEDEC ID を読んで実機で確定させる。
//
// 保存できる秒数は、この実測容量から逆算する。
namespace FlashCheck {

// XIAO nRF52840 のオンボードフラッシュ候補。
// begin() に渡して認識できなかった場合に備え、JEDEC ID も生で表示する。
static const SPIFlash_Device_t kCandidates[] = {
  {  // P25Q16H  2MiB
    .total_size = (1UL << 21), .start_up_time_us = 5000,
    .manufacturer_id = 0x85, .memory_type = 0x60, .capacity = 0x15,
    .max_clock_speed_mhz = 104, .quad_enable_bit_mask = 0x02,
    .has_sector_protection = 1, .supports_fast_read = 1,
    .supports_qspi = 1, .supports_qspi_writes = 1,
    .write_status_register_split = 1, .single_status_byte = 0, .is_fram = 0,
  },
  {  // P25Q32H  4MiB
    .total_size = (1UL << 22), .start_up_time_us = 5000,
    .manufacturer_id = 0x85, .memory_type = 0x60, .capacity = 0x16,
    .max_clock_speed_mhz = 104, .quad_enable_bit_mask = 0x02,
    .has_sector_protection = 1, .supports_fast_read = 1,
    .supports_qspi = 1, .supports_qspi_writes = 1,
    .write_status_register_split = 1, .single_status_byte = 0, .is_fram = 0,
  },
};

// 16kHz/16bit モノラルを基準にした保存レート (bytes/sec)
static const uint32_t kRatePcm16k    = 32000;
static const uint32_t kRateAdpcm16k  = 8000;
static const uint32_t kRateAdpcm8k   = 4000;
static const uint32_t kRateOpus16kbps = 2000;

inline void printDuration(Stream& out, const char* label, uint32_t bytes, uint32_t rate) {
  uint32_t sec = bytes / rate;
  out.print(F("    "));
  out.print(label);
  out.print(F(" : "));
  out.print(sec / 60);
  out.print(F("m "));
  out.print(sec % 60);
  out.print(F("s   ("));
  out.print(rate);
  out.println(F(" B/s)"));
}

static Adafruit_FlashTransport_QSPI g_transport;
static Adafruit_SPIFlash g_flash(&g_transport);

inline void report(Stream& out, bool rc) {
  out.print(F("  begin() rc   : "));
  out.println(rc ? F("true") : F("FALSE"));
  out.print(F("  jedec id     : 0x"));
  out.println(g_flash.getJEDECID(), HEX);
  uint32_t bytes = g_flash.size();
  out.print(F("  size()       : "));
  out.print(bytes);
  out.println(F(" bytes"));
  if (!rc || bytes == 0) {
    out.println(F("  -> _flash_dev == NULL (Adafruit_SPIFlashBase.cpp:277-279)"));
    return;
  }
  out.print(F("  page size    : "));
  out.print(g_flash.pageSize());
  out.print(F("   pages: "));
  out.println(g_flash.numPages());
  out.println(F("  recording time at 16kHz mono (raw capacity, no filesystem):"));
  printDuration(out, "pcm 16bit  ", bytes, kRatePcm16k);
  printDuration(out, "adpcm 16kHz", bytes, kRateAdpcm16k);
  printDuration(out, "adpcm  8kHz", bytes, kRateAdpcm8k);
  printDuration(out, "opus 16kbps", bytes, kRateOpus16kbps);
  out.println(F("  (filesystem overhead is NOT subtracted yet)"));
}

// 'f' : 正しい候補リスト (P25Q16H + P25Q32H) を渡す
inline void run(Stream& out) {
  out.println(F("---- flash check : correct device list --------"));
  bool rc = g_flash.begin(kCandidates, sizeof(kCandidates) / sizeof(kCandidates[0]));
  report(out, rc);
  out.println(F("----------------------------------------------"));
}

// 'F' : ExternalFileSystem.cpp:141 と同じく P25Q32H(4MiB) だけを渡す。
//
// 起動直後にこれを最初に呼べば、end() の影響も
// トランスポート2重生成の影響も無い条件で F-10 を再現できる。
//
// Adafruit_SPIFlashBase.cpp:110-163 の begin() は
//   1. 渡された候補と JEDEC を照合
//   2. 外れたら possible_devices[] と照合
//   3. どちらも外れたら _flash_dev = NULL で false
// で、possible_devices[] (:91-101) に P25Q16H は含まれていない。
inline void runP25Q32HOnly(Stream& out) {
  out.println(F("---- flash check : P25Q32H only (F-10 repro) --"));
  bool rc = g_flash.begin(&kCandidates[1], 1);
  report(out, rc);
  out.println(F("  -> ExternalFileSystem.cpp:141 はこの rc を見ていない"));
  out.println(F("----------------------------------------------"));
}

}  // namespace FlashCheck
