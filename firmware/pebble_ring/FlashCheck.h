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

// 観測を4つに分離して記録する。
//
//   raw_read_ok    : トランスポートからの JEDEC 生読み出しが成功したか
//   raw_jedec      : そのとき受信した4バイト（バスが生きている証拠）
//   begin_ok       : Adafruit_SPIFlash::begin() の戻り値
//   descriptor_id  : getJEDECID()。バスではなく**照合に成功した記述子**の値。
//                    _flash_dev == NULL なら固定値 0xFFFFFF を返すだけ
//                    (Adafruit_SPIFlashBase.cpp:287-293)
//   size           : 同じく記述子の total_size。NULL なら 0 (:277-279)
//
// raw_jedec が正しく読めているのに begin_ok が false なら、
// 「バス障害」ではなく「候補リストとの照合失敗」だと分離できる。
//
// トランスポートは1個だけ。begin() が内部で _trans->begin() を呼ぶので、
// 照合に失敗した後でも同じトランスポートから生読み出しできる。
inline void probe(Stream& out, const SPIFlash_Device_t* devs, size_t count,
                  const char* label) {
  out.print(F("---- flash probe : "));
  out.println(label);

  bool begin_ok = g_flash.begin(devs, count);

  uint8_t raw[4] = {0, 0, 0, 0};
  bool raw_ok = g_transport.readCommand(SFLASH_CMD_READ_JEDEC_ID, raw, 4);

  out.print(F("  raw_read_ok   : "));
  out.println(raw_ok ? F("true") : F("false"));
  out.print(F("  raw_jedec     : "));
  for (uint8_t i = 0; i < 4; i++) {
    if (raw[i] < 16) out.print('0');
    out.print(raw[i], HEX);
    out.print(' ');
  }
  out.println();
  out.print(F("  begin_ok      : "));
  out.println(begin_ok ? F("true") : F("FALSE"));
  out.print(F("  descriptor_id : 0x"));
  out.println(g_flash.getJEDECID(), HEX);
  uint32_t bytes = g_flash.size();
  out.print(F("  size          : "));
  out.print(bytes);
  out.println(F(" bytes"));

  if (begin_ok && bytes) {
    out.println(F("  recording time at 16kHz mono (raw capacity, no filesystem):"));
    printDuration(out, "pcm 16bit  ", bytes, kRatePcm16k);
    printDuration(out, "adpcm 16kHz", bytes, kRateAdpcm16k);
    printDuration(out, "adpcm  8kHz", bytes, kRateAdpcm8k);
    printDuration(out, "opus 16kbps", bytes, kRateOpus16kbps);
    out.println(F("  (filesystem overhead is NOT subtracted yet)"));
  }
  out.println(F("----------------------------------------------"));
}

// 'f' : 正しい候補リスト (P25Q16H + P25Q32H)
inline void run(Stream& out) {
  probe(out, kCandidates, sizeof(kCandidates) / sizeof(kCandidates[0]),
        "correct device list");
}

// 'F' : ExternalFileSystem.cpp:141 と同じく P25Q32H(4MiB) のみ
inline void runP25Q32HOnly(Stream& out) {
  probe(out, &kCandidates[1], 1, "P25Q32H only (F-10 repro)");
}

}  // namespace FlashCheck
