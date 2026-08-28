#include <array>
#include <cassert>
#include <cstdio>
#include <limits>
#include "../../firmware/pebble_ring/src/fatfs/diskio_flash.cpp"

static Adafruit_SPIFlash medium;
static std::array<BYTE, 512> data;

static void resetModel() {
  f_mount(nullptr, "", 0);
  medium = Adafruit_SPIFlash();
  // Simulated MCU restart, available only inside this host test translation unit.
  g_fl = nullptr; g_blkAddr = INVALID; g_dirty = false; g_faulted = false;
  g_verify = true; flashio_clear_errors();
  assert(flashio_attach(&medium));
  data.fill(0x5a);
}

static void checkRanges() {
  resetModel();
  assert(!SafetyPolicy::sectorRange(UINT32_MAX, 2, 4096));
  assert(!SafetyPolicy::byteRange(UINT32_MAX, 512, 2097152));
  assert(!SafetyPolicy::byteRange(4095, 513, 2097152));
  assert(SafetyPolicy::byteRange(4095, 512, 2097152));
  assert(disk_write(0, data.data(), UINT32_MAX, 2) == RES_PARERR);
  assert(disk_read(0, data.data(), UINT32_MAX, 2) == RES_PARERR);
  assert(disk_write(0, data.data(), 0, 0) == RES_PARERR);
  assert(disk_read(0, nullptr, 0, 1) == RES_PARERR);
  assert(disk_write(1, data.data(), 0, 1) == RES_PARERR);
  assert(disk_ioctl(0, GET_SECTOR_COUNT, nullptr) == RES_PARERR);
  assert(medium.reads == 0 && medium.writes == 0 && medium.erases == 0);
  assert(disk_write(0, data.data(), 4095, 1) == RES_OK);
  assert(disk_ioctl(0, CTRL_SYNC, nullptr) == RES_OK);
  assert(medium.bytes.back() == 0x5a);
  puts("PASS ranges: overflow/zero/null/drive/end-of-medium");
}

static void checkDirtyAttach() {
  resetModel();
  assert(disk_write(0, data.data(), 0, 1) == RES_OK);
  assert(g_dirty && medium.bytes[0] == 0xff);
  assert(flashio_attach(&medium) && g_dirty);
  Adafruit_SPIFlash other;
  assert(!flashio_attach(&other) && g_dirty);
  assert(flashio_flush() && !g_dirty && medium.bytes[0] == 0x5a);
  puts("PASS attach: pending data preserved, replacement refused");
}

static void checkFaults() {
  for (int kind = 0; kind < 4; kind++) {
    resetModel();
    assert(disk_write(0, data.data(), 0, 1) == RES_OK);
    if (kind == 0) medium.failErase = true;
    if (kind == 1) medium.failWrite = true;
    if (kind == 2) medium.corruptWrite = true;
    if (kind == 3) medium.shortRead = true;
    assert(disk_ioctl(0, CTRL_SYNC, nullptr) == RES_ERROR);
    assert(g_dirty && g_faulted && flashio_error_count() == 1);
    unsigned erases = medium.erases;
    flashio_clear_errors();
    assert(!flashio_attach(&medium));
    assert(disk_write(0, data.data(), 8, 1) == RES_ERROR);
    assert(!flashio_flush() && medium.erases == erases);
  }
  resetModel();
  assert(disk_write(0, data.data(), 0, 1) == RES_OK && flashio_flush());
  medium.shortRead = true;
  assert(disk_write(0, data.data(), 8, 1) == RES_ERROR);
  assert(g_blkAddr == INVALID && g_faulted);
  puts("PASS faults: erase/write/read/verify latch and no destructive retry");
}

static void format(FATFS& fs) {
  static BYTE work[4096];
  assert(f_mkfs("", FM_FAT, 1024, work, sizeof(work)) == FR_OK);
  assert(f_mount(&fs, "", 1) == FR_OK);
  assert(fs.n_fatent - 2 == 1992 && fs.n_fats == 2 && fs.database == 112);
}

static void createFile() {
  FIL file; UINT written = 0;
  assert(f_open(&file, "REC_010.ADP", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK);
  assert(f_write(&file, data.data(), data.size(), &written) == FR_OK);
  assert(written == data.size());
  assert(f_close(&file) == FR_OK);
}

static void checkUnlinkSync() {
  resetModel(); FATFS fs;
  format(fs); createFile();
  assert(f_unlink("REC_010.ADP") == FR_OK);
  // Immediately after unlink: no M, disk_read, f_getfree or extra sync.
  assert(!g_dirty && !g_faulted);
  auto persisted = medium.bytes;
  f_mount(nullptr, "", 0);
  g_fl = nullptr; g_blkAddr = INVALID; g_dirty = false;
  assert(flashio_attach(&medium));
  assert(f_mount(&fs, "", 1) == FR_OK);
  FILINFO info;
  assert(f_stat("REC_010.ADP", &info) == FR_NO_FILE);
  assert(persisted == medium.bytes);
  f_mount(nullptr, "", 0);
  puts("PASS unlink: deletion persisted before any subsequent read/remount");

  resetModel(); format(fs); createFile(); medium.failErase = true;
  assert(f_unlink("REC_010.ADP") == FR_DISK_ERR);
  assert(g_faulted);
  f_mount(nullptr, "", 0);
  puts("PASS unlink: lower-layer failure reaches FatFs");
}

static void checkCheckpointLimit() {
  resetModel();
  assert(disk_write(0, data.data(), 0, 1) == RES_OK && flashio_flush());
  assert(medium.bytes[0] == 0x5a);  // Previously synced data.
  assert(disk_write(0, data.data(), 1, 1) == RES_OK);
  medium.cutAfterErase = true;
  assert(!flashio_flush());
  assert(medium.bytes[0] == 0xff);  // Same erase block: old data also lost.
  puts("PASS model counterexample: later erase can destroy earlier synced data");
}

int main() {
  static_assert(SafetyPolicy::kHardwareQuarantine, "Hardware hold must remain enabled");
  for (char c : {'M','Z','Y','W','X','R','E','V','D','L','A','G','f','F','S','m'})
    assert(!SafetyPolicy::localOnlyCommand(c));
  checkRanges(); checkDirtyAttach(); checkFaults(); checkUnlinkSync(); checkCheckpointLimit();
  puts("ALL HOST STORAGE TESTS PASSED (no physical device accessed)");
}
