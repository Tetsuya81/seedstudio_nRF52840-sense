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

inline void run(Stream& out) {
  out.println(F("---- flash check ------------------------------"));

  static Adafruit_FlashTransport_QSPI transport;
  static Adafruit_SPIFlash flash(&transport);

  if (!flash.begin(kCandidates, sizeof(kCandidates) / sizeof(kCandidates[0]))) {
    out.println(F("  flash.begin() FAILED"));
    // 認識できなくても JEDEC ID だけは出す
    out.print(F("  jedec id     : 0x"));
    out.println(flash.getJEDECID(), HEX);
    out.println(F("----------------------------------------------"));
    return;
  }

  uint32_t jedec = flash.getJEDECID();
  uint32_t bytes = flash.size();

  out.print(F("  jedec id     : 0x"));
  out.println(jedec, HEX);
  out.print(F("  capacity     : "));
  out.print(bytes);
  out.print(F(" bytes  = "));
  out.print(bytes / 1024UL / 1024UL);
  out.println(F(" MiB"));
  out.print(F("  page size    : "));
  out.print(flash.pageSize());
  out.print(F("   pages: "));
  out.println(flash.numPages());

  out.println(F("  recording time at 16kHz mono (raw capacity, no filesystem):"));
  printDuration(out, "pcm 16bit  ", bytes, kRatePcm16k);
  printDuration(out, "adpcm 16kHz", bytes, kRateAdpcm16k);
  printDuration(out, "adpcm  8kHz", bytes, kRateAdpcm8k);
  printDuration(out, "opus 16kbps", bytes, kRateOpus16kbps);
  out.println(F("  (filesystem overhead is NOT subtracted yet)"));
  out.println(F("----------------------------------------------"));
}

}  // namespace FlashCheck
