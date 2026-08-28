#pragma once
#include <Arduino.h>
#include "FlashCheck.h"
#include "src/fatfs/ff.h"

extern "C" {
void        flashio_attach(Adafruit_SPIFlash *fl);
void        flashio_set_verify(int on);
uint32_t    flashio_error_count(void);
const char *flashio_error_what(void);
uint32_t    flashio_error_addr(void);
void        flashio_clear_errors(void);
}

// 実機のフラッシュを FAT12 で初期化し、結果を検証する。
//
// 期待するジオメトリ（tools/fatfs_host での実測。chat.md F-21）:
//   MBR 0 / BPB 63 / FAT1 64 / FAT2 72 / root 80 / data 112
//   1992 クラスタ x 1KiB = 2,039,808 bytes (FAT12)
namespace FormatCheck {

static const uint32_t kExpectJedec    = 0x856015;
static const uint32_t kExpectSize     = 2097152;
static const uint32_t kExpectClusters = 1992;
static const uint32_t kExpectDataLba  = 112;

static bool     g_armed = false;
static uint32_t g_armedAt = 0;
static const uint32_t kArmWindowMs = 30000;

static FATFS g_fs;
static BYTE  g_work[4096];

inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 初期化ゲート。MSG-009 の複合条件。
// raw は begin() 後に同じトランスポートから読み直したもの（取得時点を明示する）。
inline bool gate(Stream& out) {
  bool begin_ok = FlashCheck::g_flash.begin(
      FlashCheck::kCandidates,
      sizeof(FlashCheck::kCandidates) / sizeof(FlashCheck::kCandidates[0]));

  uint8_t raw[4] = {0, 0, 0, 0};
  bool raw_ok = FlashCheck::g_transport.readCommand(SFLASH_CMD_READ_JEDEC_ID, raw, 4);
  uint32_t raw_id = ((uint32_t)raw[0] << 16) | ((uint32_t)raw[1] << 8) | raw[2];
  uint32_t desc_id = FlashCheck::g_flash.getJEDECID();
  uint32_t size = FlashCheck::g_flash.size();

  out.println(F("  -- gate (raw is re-read after begin) --"));
  out.print(F("    raw_read_ok   : ")); out.println(raw_ok ? F("true") : F("false"));
  out.print(F("    raw_jedec     : 0x")); out.println(raw_id, HEX);
  out.print(F("    begin_ok      : ")); out.println(begin_ok ? F("true") : F("false"));
  out.print(F("    descriptor_id : 0x")); out.println(desc_id, HEX);
  out.print(F("    size          : ")); out.println(size);

  bool ok = raw_ok && begin_ok && (raw_id == kExpectJedec) &&
            (desc_id == raw_id) && (size == kExpectSize);
  out.print(F("    GATE          : "));
  out.println(ok ? F("PASS") : F("FAIL -> abort"));
  return ok;
}

// FatFs もブロック層も経由せず、媒体から直接読んで配置を確認する。
inline void verifyLayout(Stream& out) {
  uint8_t s0[512], bpb[512];
  if (FlashCheck::g_flash.readBuffer(0, s0, 512) != 512) {
    out.println(F("  verify: readBuffer(LBA0) failed"));
    return;
  }
  uint32_t b_vol = 0;
  bool has_mbr = !((s0[0] == 0xEB || s0[0] == 0xE9) && rd16(s0 + 11) == 512);
  if (has_mbr) b_vol = rd32(s0 + 446 + 8);

  if (FlashCheck::g_flash.readBuffer(b_vol * 512, bpb, 512) != 512) {
    out.println(F("  verify: readBuffer(BPB) failed"));
    return;
  }

  uint16_t byts = rd16(bpb + 11);
  uint8_t  spc  = bpb[13];
  uint16_t rsvd = rd16(bpb + 14);
  uint8_t  nfat = bpb[16];
  uint16_t rent = rd16(bpb + 17);
  uint16_t t16  = rd16(bpb + 19);
  uint16_t fsz  = rd16(bpb + 22);
  uint32_t tot  = t16 ? t16 : rd32(bpb + 32);

  uint32_t rootsec = ((uint32_t)rent * 32 + byts - 1) / byts;
  uint32_t firstdat = rsvd + (uint32_t)nfat * fsz + rootsec;
  uint32_t clusters = spc ? (tot - firstdat) / spc : 0;

  out.println(F("  -- layout read back from media (no cache, no FatFs) --"));
  out.print(F("    BPB: byts/sec=")); out.print(byts);
  out.print(F(" sec/clus="));         out.print(spc);
  out.print(F(" rsvd="));             out.print(rsvd);
  out.print(F(" nfats="));            out.print(nfat);
  out.print(F(" rootent="));          out.print(rent);
  out.print(F(" fatsz="));            out.print(fsz);
  out.print(F(" totsec="));           out.println(tot);

  uint32_t lbaMbr = 0, lbaFat1 = b_vol + rsvd, lbaFat2 = b_vol + rsvd + fsz;
  uint32_t lbaRoot = b_vol + rsvd + (uint32_t)nfat * fsz, lbaData = b_vol + firstdat;
  out.print(F("    physical LBA: "));
  if (has_mbr) { out.print(F("MBR ")); out.print(lbaMbr); out.print(F(" / ")); }
  out.print(F("BPB "));  out.print(b_vol);
  out.print(F(" / FAT1 ")); out.print(lbaFat1);
  if (nfat >= 2) { out.print(F(" / FAT2 ")); out.print(lbaFat2); }
  out.print(F(" / root ")); out.print(lbaRoot);
  out.print(F(" / data ")); out.println(lbaData);

  out.print(F("    4KiB alignment: FAT1="));
  out.print(lbaFat1 % 8 == 0 ? F("ok") : F("NG"));
  if (nfat >= 2) { out.print(F(" FAT2=")); out.print(lbaFat2 % 8 == 0 ? F("ok") : F("NG")); }
  out.print(F(" root=")); out.print(lbaRoot % 8 == 0 ? F("ok") : F("NG"));
  out.print(F(" data=")); out.println(lbaData % 8 == 0 ? F("ok") : F("NG"));

  out.print(F("    clusters      : ")); out.print(clusters);
  out.print(F("  (expect ")); out.print(kExpectClusters); out.println(F(")"));
  out.print(F("    data bytes    : ")); out.println(clusters * spc * byts);
  out.print(F("    RESULT        : "));
  out.println((clusters == kExpectClusters && lbaData == kExpectDataLba && nfat == 2)
              ? F("MATCHES host measurement") : F("DIFFERS from host measurement"));
}

// フラッシュが初期化済みであることを保証する。
// begin() を呼ばずに使うと disk_status() が STA_NOINIT を返し、
// f_mount が FR_NOT_READY(3) になる。戻り値は必ず確認する。
inline bool ensureFlash(Stream& out) {
  if (FlashCheck::g_flash.size() == FlashCheck::kExpectedSize) return true;
  bool ok = FlashCheck::g_flash.begin(
      FlashCheck::kCandidates,
      sizeof(FlashCheck::kCandidates) / sizeof(FlashCheck::kCandidates[0]));
  if (!ok || FlashCheck::g_flash.size() == 0) {
    out.print(F("  flash.begin() FAILED (size="));
    out.print(FlashCheck::g_flash.size());
    out.println(F(") -> abort"));
    return false;
  }
  return true;
}

inline void mountReport(Stream& out) {
  out.println(F("---- mount ------------------------------------"));
  if (!ensureFlash(out)) { out.println(F("----------------------------------------------")); return; }
  flashio_attach(&FlashCheck::g_flash);
  FRESULT r = f_mount(&g_fs, "", 1);
  out.print(F("  f_mount       : "));
  out.println(r == FR_OK ? F("FR_OK") : F("ERROR"));
  if (r != FR_OK) { out.print(F("  FRESULT=")); out.println((int)r); }
  else {
    DWORD nclst = 0; FATFS *fsp = NULL;
    if (f_getfree("", &nclst, &fsp) == FR_OK) {
      out.print(F("  fat type      : FAT")); out.println(fsp->fs_type == FS_FAT12 ? 12 : (fsp->fs_type == FS_FAT16 ? 16 : 32));
      out.print(F("  cluster size  : ")); out.print((uint32_t)fsp->csize * 512); out.println(F(" bytes"));
      out.print(F("  total clusters: ")); out.println((uint32_t)fsp->n_fatent - 2);
      out.print(F("  free clusters : ")); out.println((uint32_t)nclst);
      out.print(F("  free bytes    : ")); out.println((uint32_t)nclst * fsp->csize * 512);
    }
    // 中身の一覧
    DIR dir; FILINFO fno; uint16_t n = 0;
    if (f_opendir(&dir, "/") == FR_OK) {
      out.println(F("  files:"));
      while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        out.print(F("    ")); out.print(fno.fname);
        out.print(F("  ")); out.println((uint32_t)fno.fsize);
        n++;
      }
      f_closedir(&dir);
      if (!n) out.println(F("    (empty)"));
    }
  }
  out.print(F("  io errors     : ")); out.print(flashio_error_count());
  out.print(F("  last=")); out.print(flashio_error_what());
  out.print(F(" @0x")); out.println(flashio_error_addr(), HEX);
  out.println(F("----------------------------------------------"));
}

inline void arm(Stream& out) {
  out.println(F("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"));
  out.println(F("  この操作は 2MiB の録音領域を初期化します。"));
  out.println(F("  既存の内容は通常の操作では読めなくなります。"));
  out.println(F("  target: XIAO nRF52840 Sense onboard flash"));
  out.println(F("          JEDEC 0x856015 / 2,097,152 bytes"));
  out.println(F("  実行するなら 30 秒以内に 'Y' を送ってください。"));
  out.println(F("  それ以外の入力・時間切れで取り消します。"));
  out.println(F("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"));
  g_armed = true;
  g_armedAt = millis();
}

inline void execute(Stream& out) {
  if (!g_armed) { out.println(F("(not armed. send 'Z' first)")); return; }
  if (millis() - g_armedAt > kArmWindowMs) {
    g_armed = false;
    out.println(F("(arm expired. cancelled)"));
    return;
  }
  g_armed = false;

  out.println(F("---- format -----------------------------------"));
  if (!gate(out)) { out.println(F("----------------------------------------------")); return; }

  flashio_clear_errors();
  flashio_set_verify(1);          // 書き込みごとに読み戻して照合する
  flashio_attach(&FlashCheck::g_flash);

  f_mount(NULL, "", 0);           // 念のためアンマウント
  uint32_t t0 = millis();
  FRESULT r = f_mkfs("", FM_FAT, 1024, g_work, sizeof(g_work));
  uint32_t dt = millis() - t0;

  out.print(F("  f_mkfs        : "));
  out.print(r == FR_OK ? F("FR_OK") : F("ERROR"));
  out.print(F("  FRESULT=")); out.print((int)r);
  out.print(F("  ")); out.print(dt); out.println(F(" ms"));
  out.print(F("  io errors     : ")); out.print(flashio_error_count());
  out.print(F("  last=")); out.print(flashio_error_what());
  out.print(F(" @0x")); out.println(flashio_error_addr(), HEX);

  if (r == FR_OK && flashio_error_count() == 0) {
    verifyLayout(out);
  } else {
    out.println(F("  -> NOT proceeding to verify. stopped."));
  }
  out.println(F("----------------------------------------------"));
}

}  // namespace FormatCheck
