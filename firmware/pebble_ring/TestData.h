#pragma once
#include <Arduino.h>
#include "FormatCheck.h"

// 実機での保存・再起動後の読み出し・満杯・Macへのコピーを検証するための
// テストデータ生成と照合。
//
// 中身は決定的なパターンなので、再起動後でも同じ CRC32 が出るはず。
// Mac へコピーした後も同じ CRC32 になることを確認する。
namespace TestData {

inline uint32_t crc32(uint32_t crc, const uint8_t *d, uint32_t n) {
  crc = ~crc;
  while (n--) {
    crc ^= *d++;
    for (uint8_t k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
  }
  return ~crc;
}

// ファイル番号とオフセットだけで決まるパターン
inline uint8_t pat(uint16_t idx, uint32_t off) {
  return (uint8_t)(((off * 2654435761u) + (uint32_t)idx * 40503u) >> 24);
}

static uint8_t g_buf[512];

// 1本書く。成否・実サイズ・CRC32・所要時間を返す。
inline bool writeOne(Stream& out, uint16_t idx, uint32_t bytes) {
  char name[16];
  snprintf(name, sizeof(name), "rec_%03u.adp", (unsigned)idx);

  FIL f;
  if (f_open(&f, name, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
    out.print(F("    ")); out.print(name); out.println(F("  f_open FAILED"));
    return false;
  }

  uint32_t t0 = millis(), written = 0, crc = 0;
  bool ok = true;
  while (written < bytes) {
    uint32_t chunk = bytes - written;
    if (chunk > sizeof(g_buf)) chunk = sizeof(g_buf);
    for (uint32_t i = 0; i < chunk; i++) g_buf[i] = pat(idx, written + i);
    UINT bw = 0;
    FRESULT r = f_write(&f, g_buf, chunk, &bw);
    if (r != FR_OK || bw != chunk) { ok = false; }
    crc = crc32(crc, g_buf, bw);
    written += bw;
    if (bw < chunk) break;          // 満杯
  }
  FRESULT rs = f_sync(&f);
  f_close(&f);
  uint32_t dt = millis() - t0;

  out.print(F("    ")); out.print(name);
  out.print(F("  ")); out.print(written); out.print(F(" B"));
  out.print(F("  crc32=0x")); out.print(crc, HEX);
  out.print(F("  ")); out.print(dt); out.print(F(" ms"));
  out.print(F("  sync=")); out.print(rs == FR_OK ? F("ok") : F("ERR"));
  out.print(F("  io_err=")); out.print(flashio_error_count());
  if (written < bytes) out.print(F("   <- FULL"));
  out.println();
  return ok && written == bytes;
}

// ボリューム上の全ファイルを読み直して CRC32 とパターン一致を確認する
inline void verifyAll(Stream& out) {
  out.println(F("---- verify files on volume -------------------"));
  DIR dir; FILINFO fno;
  if (f_opendir(&dir, "/") != FR_OK) {
    out.println(F("  f_opendir failed (mounted?)"));
    out.println(F("----------------------------------------------"));
    return;
  }
  uint16_t files = 0, bad = 0;
  while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
    files++;
    uint16_t idx = 0;
    // rec_NNN.adp から番号を取る
    if (strncmp(fno.fname, "REC_", 4) == 0 || strncmp(fno.fname, "rec_", 4) == 0)
      idx = (uint16_t)atoi(fno.fname + 4);

    FIL f;
    if (f_open(&f, fno.fname, FA_READ) != FR_OK) {
      out.print(F("    ")); out.print(fno.fname); out.println(F("  open FAILED"));
      bad++; continue;
    }
    uint32_t crc = 0, off = 0, mismatch = 0;
    for (;;) {
      UINT br = 0;
      if (f_read(&f, g_buf, sizeof(g_buf), &br) != FR_OK || br == 0) break;
      for (UINT i = 0; i < br; i++) if (g_buf[i] != pat(idx, off + i)) mismatch++;
      crc = crc32(crc, g_buf, br);
      off += br;
    }
    f_close(&f);
    out.print(F("    ")); out.print(fno.fname);
    out.print(F("  ")); out.print(off); out.print(F(" B"));
    out.print(F("  crc32=0x")); out.print(crc, HEX);
    out.print(F("  pattern="));
    if (mismatch == 0) out.println(F("ok"));
    else { out.print(mismatch); out.println(F(" BYTES DIFFER")); bad++; }
  }
  f_closedir(&dir);
  out.print(F("  files=")); out.print(files);
  out.print(F("  bad=")); out.print(bad);
  out.print(F("  io_err=")); out.println(flashio_error_count());
  out.println(F("----------------------------------------------"));
}

// 基本の書き込み試験（小さめ3本）
inline void writeBasic(Stream& out) {
  out.println(F("---- write test files -------------------------"));
  flashio_clear_errors();
  for (uint16_t i = 1; i <= 3; i++) writeOne(out, i, 60000);
  out.println(F("----------------------------------------------"));
}

// 満杯試験。30秒ADPCM相当(240,000B)を書けなくなるまで書く。
inline void fillTest(Stream& out) {
  out.println(F("---- fill test (240,000 B each) ---------------"));
  flashio_clear_errors();
  for (uint16_t i = 10; i < 40; i++) {
    if (!writeOne(out, i, 240000)) break;
  }
  DWORD nclst = 0; FATFS *fsp = NULL;
  if (f_getfree("", &nclst, &fsp) == FR_OK) {
    out.print(F("  free clusters after fill: ")); out.print((uint32_t)nclst);
    out.print(F("  = ")); out.print((uint32_t)nclst * fsp->csize * 512); out.println(F(" bytes"));
  }
  out.println(F("----------------------------------------------"));
}

// 大きいファイルを削除して空きを作る（電源断試験の準備）
inline void deleteBig(Stream& out) {
  out.println(F("---- delete REC_010..REC_017 ------------------"));
  for (uint16_t i = 10; i <= 17; i++) {
    char name[16];
    snprintf(name, sizeof(name), "rec_%03u.adp", (unsigned)i);
    FRESULT r = f_unlink(name);
    out.print(F("    ")); out.print(name); out.print(F("  "));
    out.println(r == FR_OK ? F("deleted") : (r == FR_NO_FILE ? F("(none)") : F("ERROR")));
  }
  DWORD nclst = 0; FATFS *fsp = NULL;
  if (f_getfree("", &nclst, &fsp) == FR_OK) {
    out.print(F("  free clusters : ")); out.print((uint32_t)nclst);
    out.print(F("  = ")); out.print((uint32_t)nclst * fsp->csize * 512); out.println(F(" bytes"));
  }
  out.println(F("----------------------------------------------"));
}

// 電源断試験用の長い書き込み。
//
// 100,000 B ごとに f_sync して、その時点のオフセットを出力する。
// 途中でケーブルを抜かれたとき、
//   「最後に synced と表示されたオフセットまでは残っているはず」
// という明確な期待値で判定できるようにするため。
inline void longWrite(Stream& out, uint32_t bytes = 1700000) {
  out.println(F("---- long write (pull the cable during this) --"));
  out.print(F("  target: rec_090.adp  ")); out.print(bytes); out.println(F(" bytes"));
  out.println(F("  100,000 B ごとに f_sync して到達点を報告します。"));
  out.flush();

  flashio_clear_errors();
  const uint16_t idx = 90;
  FIL f;
  if (f_open(&f, "rec_090.adp", FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
    out.println(F("  f_open FAILED"));
    out.println(F("----------------------------------------------"));
    return;
  }

  uint32_t written = 0, crc = 0, nextSync = 100000;
  uint32_t t0 = millis();
  while (written < bytes) {
    uint32_t chunk = bytes - written;
    if (chunk > sizeof(g_buf)) chunk = sizeof(g_buf);
    for (uint32_t i = 0; i < chunk; i++) g_buf[i] = pat(idx, written + i);
    UINT bw = 0;
    if (f_write(&f, g_buf, chunk, &bw) != FR_OK) break;
    crc = crc32(crc, g_buf, bw);
    written += bw;
    if (bw < chunk) break;

    if (written >= nextSync) {
      FRESULT rs = f_sync(&f);
      out.print(F("  synced ")); out.print(written);
      out.print(F(" B  crc32_so_far=0x")); out.print(crc, HEX);
      out.print(F("  ")); out.print(millis() - t0); out.print(F(" ms"));
      out.print(F("  sync=")); out.println(rs == FR_OK ? F("ok") : F("ERR"));
      out.flush();
      nextSync += 100000;
    }
  }
  f_sync(&f);
  f_close(&f);
  out.print(F("  DONE (not interrupted): ")); out.print(written);
  out.print(F(" B  crc32=0x")); out.println(crc, HEX);
  out.println(F("----------------------------------------------"));
}

// 電源断後の判定。rec_090.adp がどこまで正しく残っているかを測る。
inline void assessAfterCut(Stream& out) {
  out.println(F("---- assess after power cut -------------------"));
  FIL f;
  if (f_open(&f, "rec_090.adp", FA_READ) != FR_OK) {
    out.println(F("  rec_090.adp : NOT FOUND (ディレクトリ登録前に切れた)"));
  } else {
    uint32_t off = 0, good = 0, crc = 0; uint32_t firstBad = 0xFFFFFFFF;
    for (;;) {
      UINT br = 0;
      if (f_read(&f, g_buf, sizeof(g_buf), &br) != FR_OK || br == 0) break;
      for (UINT i = 0; i < br; i++) {
        if (g_buf[i] == pat(90, off + i)) { if (firstBad == 0xFFFFFFFF) good++; }
        else if (firstBad == 0xFFFFFFFF) firstBad = off + i;
      }
      crc = crc32(crc, g_buf, br);
      off += br;
    }
    f_close(&f);
    out.print(F("  rec_090.adp size     : ")); out.println(off);
    out.print(F("  先頭からの連続一致長 : ")); out.println(good);
    out.print(F("  最初の不一致位置     : "));
    if (firstBad == 0xFFFFFFFF) out.println(F("なし (全域一致)"));
    else out.println(firstBad);
    out.print(F("  crc32                : 0x")); out.println(crc, HEX);
  }
  out.println(F("  -- 既存ファイルの生存 --"));
  verifyAll(out);
  out.println(F("----------------------------------------------"));
}

}  // namespace TestData
