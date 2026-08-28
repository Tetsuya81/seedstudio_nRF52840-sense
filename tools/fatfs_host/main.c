// f_mkfs が 2MiB 媒体に実際に作るジオメトリを、実機に触れずに測る。
//
// MSG-009 の予測表と突き合わせるのが目的。
// 出力する物理LBA: MBR / BPB / FAT1 / FAT2 / root / data
// あわせて 4KiB (8セクタ) 消去境界に各領域の先頭が乗っているかを判定する。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fatfs/ff.h"

int  disk_attach(const char *path, DWORD sectors);
void disk_detach(void);

#define IMG_BYTES   2097152UL
#define IMG_SECTORS (IMG_BYTES / 512)

static BYTE g_work[4096];

static WORD  rd16(const BYTE *p) { return (WORD)(p[0] | (p[1] << 8)); }
static DWORD rd32(const BYTE *p) {
  return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

static int make_blank(const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  BYTE buf[512];
  memset(buf, 0xFF, sizeof(buf));          // NOR の消去状態を模す
  for (DWORD i = 0; i < IMG_SECTORS; i++) {
    if (fwrite(buf, 1, 512, f) != 512) { fclose(f); return -1; }
  }
  fclose(f);
  return 0;
}

static void sec(const char *label, DWORD lba) {
  printf("    %-12s LBA %5lu  %s\n", label, (unsigned long)lba,
         (lba % 8 == 0) ? "(4KiB境界に整列)" : "");
}

static void dump(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { printf("  cannot reopen image\n"); return; }
  BYTE s0[512];
  if (fread(s0, 1, 512, f) != 512) { fclose(f); return; }

  DWORD b_vol = 0;
  int has_mbr = 0;
  // LBA0 が BPB でなければ MBR とみなす
  if (!((s0[0] == 0xEB || s0[0] == 0xE9) && rd16(s0 + 11) == 512)) {
    has_mbr = 1;
    b_vol = rd32(s0 + 446 + 8);
  }

  BYTE bpb[512];
  fseek(f, (long)b_vol * 512, SEEK_SET);
  if (fread(bpb, 1, 512, f) != 512) { fclose(f); return; }
  fclose(f);

  WORD  byts   = rd16(bpb + 11);
  BYTE  spc    = bpb[13];
  WORD  rsvd   = rd16(bpb + 14);
  BYTE  nfats  = bpb[16];
  WORD  rootent= rd16(bpb + 17);
  WORD  tot16  = rd16(bpb + 19);
  WORD  fatsz  = rd16(bpb + 22);
  DWORD tot32  = rd32(bpb + 32);
  DWORD totsec = tot16 ? tot16 : tot32;

  DWORD rootsec  = ((DWORD)rootent * 32 + byts - 1) / byts;
  DWORD firstdat = rsvd + (DWORD)nfats * fatsz + rootsec;
  DWORD datasec  = totsec - firstdat;
  DWORD clusters = spc ? datasec / spc : 0;
  const char *fattype = clusters < 4085 ? "FAT12" : (clusters < 65525 ? "FAT16" : "FAT32");

  printf("  BPB: byts/sec=%u sec/clus=%u rsvd=%u nfats=%u rootent=%u fatsz=%u totsec=%lu\n",
         byts, spc, rsvd, nfats, rootent, fatsz, (unsigned long)totsec);
  printf("  物理LBA配置:\n");
  if (has_mbr) sec("MBR", 0);
  sec("BPB", b_vol);
  sec("FAT1", b_vol + rsvd);
  if (nfats >= 2) sec("FAT2", b_vol + rsvd + fatsz);
  sec("root", b_vol + rsvd + (DWORD)nfats * fatsz);
  sec("data", b_vol + firstdat);

  DWORD databytes = clusters * spc * byts;
  printf("  クラスタ数 = %lu (%s)   データ領域 = %lu bytes\n",
         (unsigned long)clusters, fattype, (unsigned long)databytes);
  printf("  8000 B/s 換算 = %lu.%03lu 秒\n",
         (unsigned long)(databytes / 8000),
         (unsigned long)((databytes % 8000) * 1000 / 8000));
  printf("  FAT12境界(4085)までの余裕 = %ld クラスタ\n", 4085L - (long)clusters);
}

static void trial(const char *label, BYTE opt, DWORD au, const char *path) {
  printf("\n== %s ==\n", label);
  if (make_blank(path) != 0)      { printf("  cannot create image\n"); return; }
  if (disk_attach(path, IMG_SECTORS) != 0) { printf("  cannot attach\n"); return; }
  FRESULT r = f_mkfs("", opt, au, g_work, sizeof(g_work));
  disk_detach();
  printf("  f_mkfs -> %s (%d)\n", r == FR_OK ? "FR_OK" : "ERROR", (int)r);
  if (r == FR_OK) dump(path);
}

static char g_dir[1024] = ".";

static const char *imgpath(const char *name) {
  static char buf[1200];
  snprintf(buf, sizeof(buf), "%s/%s", g_dir, name);
  return buf;
}

int main(int argc, char **argv) {
  if (argc > 1) snprintf(g_dir, sizeof(g_dir), "%s", argv[1]);
  printf("FatFs R0.13c host harness   MKFS_N_FATS=%d   media=%lu bytes (%lu sectors)\n",
         MKFS_N_FATS, IMG_BYTES, (unsigned long)IMG_SECTORS);
  trial("FM_FAT | FM_SFD, au=1024", FM_FAT | FM_SFD, 1024, imgpath("pr_sfd.img"));
  trial("FM_FAT (MBR),    au=1024", FM_FAT,          1024, imgpath("pr_mbr.img"));
  trial("FM_FAT (MBR),    au=512 ", FM_FAT,          512,  imgpath("pr_mbr512.img"));
  return 0;
}
