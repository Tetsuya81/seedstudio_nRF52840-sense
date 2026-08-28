// FatFs の diskio を Adafruit_SPIFlash に接続する。
//
// コア同梱の実装 (SdFat_format.ino:187-189) は flash.syncBlocks() の結果を捨て、
// さらに下層の Adafruit_FlashCache.cpp:52-62 は eraseSector()/writeBuffer() の
// 戻り値を見ずに無条件で true を返す（chat.md F-18）。
// このため FR_OK だけでは媒体に正しく書けたことを証明できない。
//
// ここでは Adafruit_FlashCache を使わず、4KiB ブロック層を自前で持ち、
//   ・eraseSector() の bool
//   ・writeBuffer() / readBuffer() の転送バイト数
//   ・書き込み後の読み戻し照合
// をすべて確認して、失敗を RES_ERROR として上位へ返す。
//
// なお Adafruit_SPIFlash を useCache=false で使う手は取れない。
// NOR は書き込み前に消去が必要で、その read-modify-erase-write を
// キャッシュ層が担っているため、素通しにすると内容が壊れる。

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_SPIFlash.h>

extern "C" {
#include "ff.h"
#include "diskio.h"
}

#define BLK_SIZE   4096u
#define SEC_SIZE   512u
#define INVALID    0xFFFFFFFFu

static Adafruit_SPIFlash *g_fl      = NULL;
static uint8_t            g_blk[BLK_SIZE];
static uint32_t           g_blkAddr = INVALID;
static bool               g_dirty   = false;
static bool               g_verify  = true;

// 直近の失敗の記録（報告用）
static const char *g_errWhat  = NULL;
static uint32_t    g_errAddr  = 0;
static uint32_t    g_errCount = 0;

static void fail(const char *what, uint32_t addr) {
  g_errWhat = what;
  g_errAddr = addr;
  g_errCount++;
}

extern "C" void flashio_attach(Adafruit_SPIFlash *fl) {
  g_fl = fl;
  g_blkAddr = INVALID;
  g_dirty = false;
}
extern "C" void flashio_set_verify(int on) { g_verify = on ? true : false; }
extern "C" uint32_t flashio_error_count(void) { return g_errCount; }
extern "C" const char *flashio_error_what(void) { return g_errWhat ? g_errWhat : "none"; }
extern "C" uint32_t flashio_error_addr(void) { return g_errAddr; }
extern "C" void flashio_clear_errors(void) { g_errWhat = NULL; g_errAddr = 0; g_errCount = 0; }

// 未書き出しのブロックを媒体へ確定させる。
// ホストへ見せる前に必ず呼ぶ（見せている間はこちらから書かない）。
extern "C" int flashio_flush(void);

// 書き込み後に媒体から読み直して一致を確認する。
// キャッシュを経由しない readBuffer を使う。
static bool verifyBlock(uint32_t addr) {
  uint8_t tmp[256];
  for (uint32_t off = 0; off < BLK_SIZE; off += sizeof(tmp)) {
    if (g_fl->readBuffer(addr + off, tmp, sizeof(tmp)) != sizeof(tmp)) {
      fail("verify:readBuffer", addr + off);
      return false;
    }
    if (memcmp(tmp, g_blk + off, sizeof(tmp)) != 0) {
      fail("verify:mismatch", addr + off);
      return false;
    }
  }
  return true;
}

static bool flushBlock(void) {
  if (!g_dirty || g_blkAddr == INVALID) return true;

  if (!g_fl->eraseSector(g_blkAddr / BLK_SIZE)) {
    fail("eraseSector", g_blkAddr);
    return false;
  }
  if (g_fl->writeBuffer(g_blkAddr, g_blk, BLK_SIZE) != BLK_SIZE) {
    fail("writeBuffer", g_blkAddr);
    return false;
  }
  if (g_verify && !verifyBlock(g_blkAddr)) return false;

  g_dirty = false;
  return true;
}

static bool loadBlock(uint32_t addr) {
  if (g_blkAddr == addr) return true;
  if (!flushBlock()) return false;
  if (g_fl->readBuffer(addr, g_blk, BLK_SIZE) != BLK_SIZE) {
    fail("readBuffer:load", addr);
    return false;
  }
  g_blkAddr = addr;
  return true;
}

extern "C" int flashio_flush(void) { return flushBlock() ? 1 : 0; }

extern "C" DSTATUS disk_status(BYTE pdrv) {
  (void)pdrv;
  return (g_fl && g_fl->size()) ? 0 : STA_NOINIT;
}

extern "C" DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }

extern "C" DRESULT disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count) {
  (void)pdrv;
  if (!g_fl || !g_fl->size()) return RES_NOTRDY;
  if ((uint64_t)(sector + count) * SEC_SIZE > g_fl->size()) {
    fail("read:out of range", sector);
    return RES_PARERR;
  }
  if (!flushBlock()) return RES_ERROR;      // 未書き出しを残したまま読まない
  uint32_t bytes = count * SEC_SIZE;
  if (g_fl->readBuffer(sector * SEC_SIZE, buff, bytes) != bytes) {
    fail("readBuffer", sector * SEC_SIZE);
    return RES_ERROR;
  }
  return RES_OK;
}

extern "C" DRESULT disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count) {
  (void)pdrv;
  if (!g_fl || !g_fl->size()) return RES_NOTRDY;
  if ((uint64_t)(sector + count) * SEC_SIZE > g_fl->size()) {
    fail("write:out of range", sector);
    return RES_PARERR;
  }
  for (UINT i = 0; i < count; i++) {
    uint32_t addr = (sector + i) * SEC_SIZE;
    uint32_t base = addr & ~(BLK_SIZE - 1);
    if (!loadBlock(base)) return RES_ERROR;
    memcpy(g_blk + (addr - base), buff + i * SEC_SIZE, SEC_SIZE);
    g_dirty = true;
  }
  return RES_OK;
}

extern "C" DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
  (void)pdrv;
  if (!g_fl || !g_fl->size()) return RES_NOTRDY;
  switch (cmd) {
    case CTRL_SYNC:
      return flushBlock() ? RES_OK : RES_ERROR;   // 結果を捨てない
    case GET_SECTOR_COUNT:
      *(DWORD *)buff = g_fl->size() / SEC_SIZE;
      return RES_OK;
    case GET_SECTOR_SIZE:
      *(WORD *)buff = SEC_SIZE;
      return RES_OK;
    case GET_BLOCK_SIZE:
      *(DWORD *)buff = BLK_SIZE / SEC_SIZE;       // = 8。消去境界への整列に使われる
      return RES_OK;
    default:
      return RES_PARERR;
  }
}

extern "C" DWORD get_fattime(void) {
  // RTC を積んでいないので固定値。Phase 3 で録音時刻を持つときに差し替える。
  return ((DWORD)(2026 - 1980) << 25) | ((DWORD)8 << 21) | ((DWORD)28 << 16);
}
