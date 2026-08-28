// ファイルを媒体に見立てた diskio。
// ioctl の戻り値は基板側 (SdFat_format.ino:191-201) と同じにしてある:
//   GET_SECTOR_COUNT = 容量/512, GET_SECTOR_SIZE = 512, GET_BLOCK_SIZE = 8
// GET_BLOCK_SIZE=8 (=4KiB) が f_mkfs のデータ開始位置の整列に使われる (ff.c:5820-5830)。
#include <stdio.h>
#include <string.h>
#include "fatfs/ff.h"
#include "fatfs/diskio.h"

static FILE  *g_fp = NULL;
static DWORD  g_sectors = 0;

int disk_attach(const char *path, DWORD sectors) {
  g_fp = fopen(path, "r+b");
  if (!g_fp) return -1;
  g_sectors = sectors;
  return 0;
}
void disk_detach(void) { if (g_fp) { fflush(g_fp); fclose(g_fp); g_fp = NULL; } }

DSTATUS disk_status(BYTE pdrv)     { (void)pdrv; return g_fp ? 0 : STA_NOINIT; }
DSTATUS disk_initialize(BYTE pdrv) { (void)pdrv; return g_fp ? 0 : STA_NOINIT; }

DRESULT disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count) {
  (void)pdrv;
  if (!g_fp) return RES_NOTRDY;
  if (sector + count > g_sectors) return RES_PARERR;   // 範囲外LBAは弾く
  if (fseek(g_fp, (long)sector * 512, SEEK_SET) != 0) return RES_ERROR;
  return fread(buff, 512, count, g_fp) == count ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count) {
  (void)pdrv;
  if (!g_fp) return RES_NOTRDY;
  if (sector + count > g_sectors) return RES_PARERR;
  if (fseek(g_fp, (long)sector * 512, SEEK_SET) != 0) return RES_ERROR;
  return fwrite(buff, 512, count, g_fp) == count ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
  (void)pdrv;
  if (!g_fp) return RES_NOTRDY;
  switch (cmd) {
    case CTRL_SYNC:        return fflush(g_fp) == 0 ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT: *(DWORD *)buff = g_sectors;   return RES_OK;
    case GET_SECTOR_SIZE:  *(WORD  *)buff = 512;         return RES_OK;
    case GET_BLOCK_SIZE:   *(DWORD *)buff = 8;           return RES_OK;
    default:                                             return RES_PARERR;
  }
}

DWORD get_fattime(void) { return ((DWORD)(2026 - 1980) << 25) | (8 << 21) | (28 << 16); }
