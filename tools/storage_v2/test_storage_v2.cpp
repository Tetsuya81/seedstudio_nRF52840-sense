#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "storage_model.hpp"

extern "C" {
#include "../fatfs_host/fatfs/ff.h"
#include "../fatfs_host/fatfs/diskio.h"
}

using namespace storage_v2;

static const VirtualFat* g_volume = nullptr;

extern "C" DSTATUS disk_status(BYTE pdrv) {
  return pdrv == 0 && g_volume ? 0 : STA_NOINIT;
}
extern "C" DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }
extern "C" DRESULT disk_read(BYTE pdrv, BYTE* buff, DWORD sector, UINT count) {
  if (pdrv != 0 || !g_volume || !buff || count == 0 ||
      sector > kVirtualSectors || count > kVirtualSectors - sector) return RES_PARERR;
  return g_volume->read(static_cast<uint64_t>(sector) * 512, buff,
                        static_cast<size_t>(count) * 512) ? RES_OK : RES_ERROR;
}
extern "C" DRESULT disk_write(BYTE, const BYTE*, DWORD, UINT) { return RES_WRPRT; }
extern "C" DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
  if (pdrv != 0 || !g_volume) return RES_NOTRDY;
  if (cmd == CTRL_SYNC) return RES_OK;
  if (!buff) return RES_PARERR;
  if (cmd == GET_SECTOR_COUNT) { *static_cast<DWORD*>(buff) = kVirtualSectors; return RES_OK; }
  if (cmd == GET_SECTOR_SIZE) { *static_cast<WORD*>(buff) = 512; return RES_OK; }
  if (cmd == GET_BLOCK_SIZE) { *static_cast<DWORD*>(buff) = 8; return RES_OK; }
  return RES_PARERR;
}
extern "C" DWORD get_fattime(void) { return 0; }

static std::vector<uint8_t> pattern(size_t size, uint32_t salt) {
  std::vector<uint8_t> out(size);
  uint32_t state = 0x9E3779B9U ^ salt;
  for (size_t i = 0; i < size; ++i) {
    state = state * 1664525U + 1013904223U;
    out[i] = static_cast<uint8_t>(state >> 24);
  }
  return out;
}

static void writeAndCommit(StorageModel* storage, uint32_t recId,
                           const std::vector<uint8_t>& audio,
                           const std::vector<uint32_t>& blocks) {
  assert(storage->writeRecording(recId, audio, blocks));
  assert(storage->commitRecording(recId, audio, blocks));
  const ScanResult scan = storage->scan();
  assert(scan.safe);
  assert(std::any_of(scan.tierA.begin(), scan.tierA.end(),
                     [recId](const CatalogEntry& e) { return e.recId == recId; }));
}

static void checkCrcAndFormats() {
  static const uint8_t check[] = "123456789";
  assert(crc32(check, 9) == 0xCBF43926U);

  DataHeader data;
  data.recId = 0x12345678;
  data.blockIndex = 7;
  auto dp = encodeDataHeaderPage(data);
  DataHeader decoded;
  assert(decodeDataHeaderPage(dp.data(), &decoded));
  assert(decoded.recId == data.recId && decoded.blockIndex == 7);
  dp[6] = 0x01;  // unknown/non-v1 flags are rejected even with stale CRC
  assert(!decodeDataHeaderPage(dp.data(), &decoded));

  IndexRecord ready;
  ready.type = RecordType::Ready;
  ready.seq = 10;
  ready.nextRecIdHW = 22;
  ready.nextSeqHW = 11;
  auto rp = encodeIndexRecordPage(ready);
  IndexRecord rd;
  assert(decodeIndexRecordPage(rp.data(), &rd));
  assert(rd.nextRecIdHW == 22 && rd.nextSeqHW == 11);
  puts("PASS v2 formats: CRC-32/ISO-HDLC and strict PRB1/PRR1 decoding");
}

static void checkVirtualFatAndBoundaries() {
  StorageModel storage;
  assert(storage.format());
  const std::array<size_t, 7> sizes = {0, 1, 4063, 4064, 4065, 8128, 12001};
  std::map<uint32_t, std::vector<uint8_t>> expected;
  uint32_t nextPhysical = 0;
  for (size_t i = 0; i < sizes.size(); ++i) {
    const uint32_t recId = static_cast<uint32_t>(i + 1);
    auto audio = pattern(sizes[i], recId);
    const size_t blocksNeeded = (audio.size() + kPayloadBytes - 1) / kPayloadBytes;
    std::vector<uint32_t> blocks;
    // Deliberately fragment every multi-block recording.
    for (size_t b = 0; b < blocksNeeded; ++b) blocks.push_back(nextPhysical + (blocksNeeded - 1 - b));
    nextPhysical += static_cast<uint32_t>(blocksNeeded + 2);
    writeAndCommit(&storage, recId, audio, blocks);
    expected[recId] = audio;
  }

  const ScanResult scan = storage.scan();
  assert(scan.tierA.size() == sizes.size());
  VirtualFat volume;
  assert(volume.build(storage.medium(), scan.tierA));
  const auto image = volume.image();
  assert(image.size() == static_cast<size_t>(kVirtualSectors) * 512);
  assert(image[0] == 0xEB && image[510] == 0x55 && image[511] == 0xAA);
  assert(image[0x13] == (kVirtualSectors & 0xFF));
  assert(image[0x14] == (kVirtualSectors >> 8));
  assert(std::all_of(image.begin() + 0x20, image.begin() + 0x24,
                     [](uint8_t v) { return v == 0; }));
  assert(image[512] == 0xF8 && image[513] == 0xFF && image[514] == 0xFF);

  g_volume = &volume;
  FATFS fs;
  assert(f_mount(&fs, "", 1) == FR_OK);
  assert(fs.fs_type == FS_FAT12 && fs.csize == 8 && fs.n_fats == 1);
  for (const auto& item : expected) {
    char filename[16];
    std::snprintf(filename, sizeof(filename), "%08X.ADP", item.first);
    FIL file;
    assert(f_open(&file, filename, FA_READ) == FR_OK);
    std::vector<uint8_t> actual(item.second.size() + 17, 0xCC);
    UINT got = 0;
    assert(f_read(&file, actual.data(), static_cast<UINT>(actual.size()), &got) == FR_OK);
    assert(got == item.second.size());
    actual.resize(got);
    assert(actual == item.second);
    assert(f_close(&file) == FR_OK);
  }
  f_mount(nullptr, "", 0);
  g_volume = nullptr;

  // Use a separate single-file image so its logical start is known. Exercise
  // arbitrary reads around both the 4064-byte physical seam and 4096-byte
  // logical seam.
  StorageModel boundaryStorage;
  assert(boundaryStorage.format());
  const auto boundaryAudio = pattern(8128, 0xB0);
  writeAndCommit(&boundaryStorage, 1, boundaryAudio, {9, 2});
  const ScanResult boundaryScan = boundaryStorage.scan();
  VirtualFat boundaryVolume;
  assert(boundaryVolume.build(boundaryStorage.medium(), boundaryScan.tierA));
  const uint64_t dataBase = 35ULL * 512;
  for (const auto& range : std::vector<std::pair<uint32_t, size_t>>{
           {0, 1}, {4000, 200}, {4063, 3}, {4064, 777}, {4095, 19}}) {
    std::vector<uint8_t> got(range.second);
    assert(boundaryVolume.read(dataBase + range.first, got.data(), got.size()));
    for (size_t i = 0; i < got.size(); ++i) {
      const uint32_t pos = range.first + static_cast<uint32_t>(i);
      const uint8_t want = pos < boundaryAudio.size() ? boundaryAudio[pos] : 0;
      assert(got[i] == want);
    }
  }
  puts("PASS virtual FAT12: FatFs mount/read, 8.3 names, fragmented payload and boundary sizes");
}

static void checkFullCapacity() {
  StorageModel storage;
  assert(storage.format());
  auto audio = pattern(static_cast<size_t>(kDataBlocks) * kPayloadBytes, 99);
  std::vector<uint32_t> blocks(kDataBlocks);
  for (uint32_t i = 0; i < kDataBlocks; ++i) blocks[i] = kDataBlocks - 1 - i;
  writeAndCommit(&storage, 0xABCDEF01U, audio, blocks);
  ScanResult scan = storage.scan();
  VirtualFat volume;
  assert(volume.build(storage.medium(), scan.tierA));
  std::array<uint8_t, 1027> got{};
  const uint32_t start = static_cast<uint32_t>(audio.size() - got.size());
  assert(volume.read(35ULL * 512 + start, got.data(), got.size()));
  assert(std::equal(got.begin(), got.end(), audio.begin() + start));
  puts("PASS virtual FAT12: full 496-block payload and reverse physical placement");
}

static void checkTornDataAndRecords() {
  const auto audio = pattern(700, 5);

  StorageModel createTorn;
  assert(createTorn.format());
  assert(!createTorn.writeRecording(10, audio, {0}, 0, 0, 111));
  assert(createTorn.scan().quarantinedBlocks == 1 && createTorn.scan().safe);
  assert(!createTorn.medium().programPage(0, encodeDataHeaderPage(DataHeader{}).data()));

  StorageModel appendTorn;
  assert(appendTorn.format());
  auto longAudio = pattern(700, 6);
  assert(!appendTorn.writeRecording(11, longAudio, {0}, 0, 1, 733));
  assert(appendTorn.commitRecording(11, longAudio, {0}));
  ScanResult scan = appendTorn.scan();
  assert(scan.tierA.empty() && scan.tierB == std::vector<uint32_t>{11});
  assert(scan.bodyMismatch == std::vector<uint32_t>{11});

  StorageModel commitTorn;
  assert(commitTorn.format());
  assert(commitTorn.writeRecording(12, audio, {0}));
  assert(!commitTorn.commitRecording(12, audio, {0}, 1001));
  scan = commitTorn.scan();
  assert(scan.tierA.empty());
  assert(std::find(scan.tierB.begin(), scan.tierB.end(), 12) != scan.tierB.end());
  assert(scan.incomplete == std::vector<uint32_t>{12});

  StorageModel deleteTorn;
  assert(deleteTorn.format());
  writeAndCommit(&deleteTorn, 13, audio, {0});
  assert(!deleteTorn.deleteRecording(13, 877));
  scan = deleteTorn.scan();
  assert(scan.tierA.size() == 1 && scan.deleted.empty());

  StorageModel reclaimTorn;
  assert(reclaimTorn.format());
  writeAndCommit(&reclaimTorn, 14, audio, {0});
  assert(reclaimTorn.deleteRecording(14));
  assert(!reclaimTorn.reclaimRecording(14, 0, 32U * 8U + 3));
  scan = reclaimTorn.scan();
  assert(scan.quarantinedBlocks == 1 && scan.safe);
  assert(std::find(scan.deleted.begin(), scan.deleted.end(), 14) != scan.deleted.end());
  puts("PASS torn writes: CREATE/APPEND/COMMIT/DELETE/RECLAIM fail closed");
}

static void checkConflictsAndCorruption() {
  const auto a = pattern(600, 1);
  const auto b = pattern(600, 2);

  StorageModel duplicate;
  assert(duplicate.format());
  assert(duplicate.writeRecording(20, a, {0}));
  assert(duplicate.writeRecording(20, b, {1}));
  assert(duplicate.commitRecording(20, a, {0}));
  ScanResult scan = duplicate.scan();
  assert(scan.safe && scan.tierA.empty() && scan.tierB == std::vector<uint32_t>{20});
  assert(scan.isolated == std::vector<uint32_t>{20});

  StorageModel missing;
  assert(missing.format());
  std::array<uint8_t, kPageBytes> page;
  page.fill(0xFF);
  DataHeader header;
  header.recId = 21;
  header.blockIndex = 1;
  const auto encodedHeader = encodeDataHeaderPage(header);
  std::copy(encodedHeader.begin(), encodedHeader.end(), page.begin());
  std::copy(a.begin(), a.begin() + 224, page.begin() + 32);
  assert(missing.medium().programPage(0, page.data()));
  assert(missing.commitRecording(21, a, {0}));
  scan = missing.scan();
  assert(scan.safe && scan.tierA.empty() && scan.tierB == std::vector<uint32_t>{21});
  assert(scan.isolated == std::vector<uint32_t>{21});

  StorageModel wrongLength;
  assert(wrongLength.format());
  assert(wrongLength.writeRecording(22, a, {0}));
  auto claimed = pattern(kPayloadBytes + 10, 1);
  assert(wrongLength.commitRecording(22, claimed, {0}));
  scan = wrongLength.scan();
  assert(scan.safe && scan.tierA.empty() && scan.tierB == std::vector<uint32_t>{22});
  assert(scan.isolated == std::vector<uint32_t>{22});

  StorageModel bodyDamage;
  assert(bodyDamage.format());
  writeAndCommit(&bodyDamage, 23, a, {0});
  assert(a[0] != 0);
  const uint8_t setBit = static_cast<uint8_t>(a[0] & static_cast<uint8_t>(-a[0]));
  bodyDamage.medium().corruptToZero(32, setBit);
  scan = bodyDamage.scan();
  assert(scan.safe && scan.tierA.empty() && scan.tierB == std::vector<uint32_t>{23});
  assert(scan.bodyMismatch == std::vector<uint32_t>{23});

  StorageModel sameSeq;
  assert(sameSeq.format());
  writeAndCommit(&sameSeq, 24, a, {0});  // COMMIT uses seq 2 at page2
  IndexRecord conflicting;
  conflicting.type = RecordType::Delete;
  conflicting.seq = 2;
  conflicting.recId = 24;
  auto conflictPage = encodeIndexRecordPage(conflicting);
  assert(sameSeq.medium().programPage(kIndexBase + 3 * kPageBytes, conflictPage.data()));
  assert(!sameSeq.scan().safe);
  puts("PASS recovery conflicts: duplicate, gap, length mismatch and body CRC damage");
}

static StorageModel compactFixture(bool deleted) {
  StorageModel storage;
  assert(storage.format());
  auto audio = pattern(900, 77);
  writeAndCommit(&storage, 30, audio, {0});
  if (deleted) assert(storage.deleteRecording(30));
  return storage;
}

static void checkCompactionAndHighWater() {
  for (CompactFault fault : {CompactFault::TornHeader, CompactFault::TornCarry,
                             CompactFault::TornReady}) {
    StorageModel storage = compactFixture(false);
    assert(!storage.compact(fault));
    ScanResult scan = storage.scan();
    assert(scan.safe && scan.activeBank == 0 && scan.tierA.size() == 1);
  }
  {
    StorageModel storage = compactFixture(false);
    assert(storage.compact(CompactFault::StopAfterReady));
    ScanResult scan = storage.scan();
    assert(scan.safe && scan.activeBank == 1 && scan.tierA.size() == 1);
  }
  {
    StorageModel storage = compactFixture(false);
    assert(!storage.compact(CompactFault::TornOldErase));
    ScanResult scan = storage.scan();
    assert(scan.safe && scan.activeBank == 1 && scan.tierA.size() == 1);
  }
  {
    StorageModel storage = compactFixture(true);
    assert(storage.compact());
    ScanResult scan = storage.scan();
    assert(std::find(scan.deleted.begin(), scan.deleted.end(), 30) != scan.deleted.end());
  }
  {
    StorageModel storage = compactFixture(true);
    assert(storage.reclaimRecording(30));
    const uint32_t before = storage.scan().nextRecId;
    assert(storage.compact());
    ScanResult scan = storage.scan();
    assert(scan.deleted.empty());
    assert(scan.nextRecId >= before && scan.nextRecId > 30);
  }

  StorageModel sameGeneration = compactFixture(false);
  BankHeader header;
  header.bankId = 1;
  header.generation = 1;
  header.firstSeq = 100;
  header.nextRecIdHW = 100;
  header.nextSeqHW = 101;
  auto hp = encodeBankHeaderPage(header);
  assert(sameGeneration.medium().programPage(kIndexBase + kIndexBankBytes, hp.data()));
  IndexRecord ready;
  ready.type = RecordType::Ready;
  ready.seq = 100;
  ready.nextRecIdHW = 100;
  ready.nextSeqHW = 101;
  auto rp = encodeIndexRecordPage(ready);
  assert(sameGeneration.medium().programPage(kIndexBase + kIndexBankBytes + 256, rp.data()));
  assert(!sameGeneration.scan().safe);

  StorageModel gap = compactFixture(false);
  IndexRecord extra;
  extra.type = RecordType::Delete;
  extra.seq = 99;
  extra.recId = 30;
  rp = encodeIndexRecordPage(extra);
  // page3 is occupied by no record in this fixture? format page0/1, commit page2;
  // leave page3 blank and place a valid record at page4.
  assert(gap.medium().programPage(kIndexBase + 4 * 256, rp.data()));
  assert(gap.scan().safe);
  puts("PASS index: compaction cuts, READY selection, tombstones, high-water and bank conflicts");
}

static void checkReservationsAndExportGate() {
  PageReservation pages;
  pages.used = 127;
  pages.reservedCommits = 1;
  pages.carry = 100;
  assert(!pages.canCreate());
  assert(!pages.canDelete());  // reserved COMMIT page cannot be consumed by DELETE
  assert(pages.canAppend());
  pages.used = 2;
  pages.reservedCommits = 0;
  pages.carry = 126;
  assert(!pages.canCreate() && pages.canCompact());
  pages.carry = 127;
  assert(!pages.canCompact());
  pages.used = 129;
  pages.carry = 0;
  assert(pages.freePages() == 0 && !pages.canCreate() && !pages.canDelete());

  ExportGate gate;
  assert(!gate.audioReady() && !gate.rawReady());
  assert(gate.requestAudio(true, true, true, 9, 9, true));
  assert(gate.audioReady() && !gate.rawReady());
  assert(!gate.requestRaw(true, true));
  gate.release();
  assert(gate.state() == ExportState::IdleLocal);
  gate.enterSafe();
  assert(gate.requestRaw(true, true));
  assert(!gate.audioReady() && gate.rawReady());

  ExportGate changed;
  assert(!changed.requestAudio(true, true, true, 10, 11, true));
  assert(changed.state() == ExportState::Fault);
  assert(changed.requestRaw(true, true));  // pre-existing fault does not block raw recovery
  puts("PASS gates: COMMIT reservation, exclusive audio/raw export and mediaGen faulting");
}

static void checkBootFenceFreshEraseAndSafetyLayers() {
  StorageModel storage;
  assert(storage.format());
  assert(storage.medium().eraseAttempts() == 8);
  const auto audio = pattern(300, 0x51);
  writeAndCommit(&storage, 50, audio, {7});
  assert(storage.medium().eraseAttempts() == 9);

  storage.restart();
  ScanResult scan = storage.scan(false);
  assert(scan.lastOccupiedPage == 2 && scan.nextWritePage == 4);
  assert(scan.tierA.empty() && scan.committedUnverified == std::vector<uint32_t>{50});
  const std::vector<uint8_t> empty;
  const std::vector<uint32_t> noBlocks;
  assert(storage.commitRecording(51, empty, noBlocks));
  assert(storage.medium().isAllFF(kIndexBase + 3 * kPageBytes, kPageBytes));
  IndexRecord fenced;
  std::array<uint8_t, kPageBytes> page{};
  assert(storage.medium().read(kIndexBase + 4 * kPageBytes, page.data(), page.size()));
  assert(decodeIndexRecordPage(page.data(), &fenced) && fenced.recId == 51);

  StorageModel occupiedTarget;
  assert(occupiedTarget.format());
  assert(occupiedTarget.commitRecording(70, empty, noBlocks));
  occupiedTarget.restart();  // cursor is fenced to page4
  occupiedTarget.medium().corruptToZero(kIndexBase + 4 * kPageBytes, 0x01);
  const uint64_t programsBefore = occupiedTarget.medium().programAttempts();
  const uint64_t erasesBefore = occupiedTarget.medium().eraseAttempts();
  assert(!occupiedTarget.commitRecording(71, empty, noBlocks));
  assert(occupiedTarget.medium().programAttempts() == programsBefore);
  assert(occupiedTarget.medium().eraseAttempts() == erasesBefore);
  assert(!occupiedTarget.scan().deviceSafe);

  storage.medium().corruptToZero(8 * kEraseBytes, 0x01);
  scan = storage.scan();
  assert(scan.deviceSafe && scan.safe && scan.quarantinedBlocks == 1);
  assert(scan.quarantinedPhysicalBlocks == std::vector<uint32_t>{8});

  for (uint32_t off = 0; off < kIndexBytes; off += kEraseBytes)
    assert(storage.medium().eraseBlock(kIndexBase + off));
  scan = storage.scan();
  assert(!scan.deviceSafe && !scan.safe && scan.activeBank < 0 && scan.tierA.empty());
  assert(std::find(scan.tierB.begin(), scan.tierB.end(), 50) != scan.tierB.end());

  PageReservation pages;
  pages.used = 127;
  pages.bootFenceNeeded = true;
  assert(!pages.canAppend());
  puts("PASS P1/P2: fresh erase, reboot fence, deferred verification and three-layer safety");
}

static void checkStatusSnapshotAndLowBatteryStop() {
  StorageModel storage;
  assert(storage.format());
  const auto audio = pattern(128, 0x61);
  writeAndCommit(&storage, 60, audio, {2});
  const ScanResult frozen = storage.scan(false);
  assert(frozen.tierA.empty() && frozen.committedUnverified.size() == 1);
  assert(frozen.freeDataBlocks == kDataBlocks - 1);
  assert(frozen.freeIndexPages == 124);
  assert(frozen.capacityPressure == CapacityPressure::Index);
  VirtualFat volume;
  assert(volume.build(storage.medium(), frozen));
  g_volume = &volume;
  FATFS fs;
  FIL file;
  assert(f_mount(&fs, "", 1) == FR_OK);
  assert(f_open(&file, "0000003C.ADP", FA_READ) == FR_NO_FILE);
  assert(f_open(&file, "STATUS.TXT", FA_READ) == FR_OK);
  std::array<char, 512> bytes{};
  UINT got = 0;
  assert(f_read(&file, bytes.data(), bytes.size(), &got) == FR_OK);
  const std::string status(bytes.data(), got);
  assert(status.find("TIER_A=0") != std::string::npos);
  assert(status.find("COMMITTED_UNVERIFIED=1") != std::string::npos);
  assert(f_close(&file) == FR_OK);
  assert(status.find("INCOMPLETE=0") != std::string::npos);
  assert(status.find("BODY_MISMATCH=0") != std::string::npos);
  assert(status.find("FREE_DATA_BLOCKS=495/496") != std::string::npos);
  assert(status.find("MUTATION_READY=YES") != std::string::npos);
  assert(status.find("DATA_BLOCKS_SCANNED=496/496") != std::string::npos);
  assert(status.find("FREE_INDEX_PAGES=124/127") != std::string::npos);
  assert(status.find("CAPACITY_PRESSURE=INDEX") != std::string::npos);
  assert(status.find("CAPACITY_IMMINENT=NO") != std::string::npos);
  assert(status.find("RAW_BACKUP_RECOMMENDED=NO") != std::string::npos);
  f_mount(nullptr, "", 0);
  g_volume = nullptr;

  ScanResult alert = frozen;
  alert.committedUnverified.clear();
  alert.tierB.push_back(60);
  alert.bodyMismatch.push_back(60);
  alert.freeDataBlocks = 49;  // below the 10% warning threshold
  alert.capacityPressure = CapacityPressure::Data;
  VirtualFat alertVolume;
  assert(alertVolume.build(storage.medium(), alert));
  const std::vector<uint8_t> alertImage = alertVolume.image();
  const std::string alertText(reinterpret_cast<const char*>(alertImage.data()), alertImage.size());
  assert(alertText.find("BODY_MISMATCH=1") != std::string::npos);
  assert(alertText.find("CAPACITY_IMMINENT=YES") != std::string::npos);
  assert(alertText.find("RAW_BACKUP_RECOMMENDED=YES") != std::string::npos);

  LowBatteryStop normal;
  assert(normal.begin(true));
  assert(normal.finishPartialDataPage(true));
  assert(normal.programCommit(true));
  assert(normal.state() == StopState::StoppedUnverified && normal.pagePrograms() == 2);
  assert(!normal.programCommit(true));  // no retry and no third page program

  LowBatteryStop noReservation;
  assert(!noReservation.begin(false));
  assert(noReservation.state() == StopState::SafeFault && noReservation.pagePrograms() == 0);
  LowBatteryStop tornData;
  assert(tornData.begin(true) && !tornData.finishPartialDataPage(false));
  assert(!tornData.programCommit(true) && tornData.pagePrograms() == 1);
  LowBatteryStop tornCommit;
  assert(tornCommit.begin(true) && tornCommit.finishPartialDataPage(true));
  assert(!tornCommit.programCommit(false));
  assert(tornCommit.state() == StopState::SafeFault && tornCommit.pagePrograms() == 2);
  puts("PASS low battery/status: max-two programs, no retry, unverified hidden, frozen STATUS.TXT");
}

static void checkBootMutationGateAndOwnership() {
  StorageModel storage;
  assert(storage.format());
  const auto committed = pattern(300, 0x70);
  const auto incomplete = pattern(300, 0x71);
  writeAndCommit(&storage, 80, committed, {10});
  assert(storage.writeRecording(81, incomplete, {11}));

  storage.beginRestart();
  assert(!storage.bootScanComplete());
  const uint64_t programsBefore = storage.medium().programAttempts();
  ScanResult pending = storage.scan(false);
  assert(!pending.mutationReady && pending.dataBlocksScanned == kDataBlocks);
  const uint64_t erasesBefore = storage.medium().eraseAttempts();
  const std::vector<uint8_t> mediumBefore = storage.medium().bytes();
  assert(!storage.writeRecording(82, pattern(32, 0x72), {12}));
  assert(!storage.commitRecording(82, std::vector<uint8_t>(), std::vector<uint32_t>()));
  assert(!storage.deleteRecording(80));
  assert(!storage.compact());
  assert(storage.medium().programAttempts() == programsBefore);
  assert(storage.medium().eraseAttempts() == erasesBefore);
  assert(storage.medium().bytes() == mediumBefore);

  assert(storage.completeRestartScan());
  assert(storage.bootScanComplete());
  ScanResult ownership = storage.scan(false);
  assert(ownership.dataBlocksScanned == kDataBlocks);
  assert(ownership.committedUnverified == std::vector<uint32_t>{80});
  assert(ownership.incomplete == std::vector<uint32_t>{81});

  const uint64_t protectedPrograms = storage.medium().programAttempts();
  const uint64_t protectedErases = storage.medium().eraseAttempts();
  assert(!storage.writeRecording(82, pattern(32, 0x73), {10}));
  assert(!storage.writeRecording(82, pattern(32, 0x74), {11}));
  assert(storage.medium().programAttempts() == protectedPrograms);
  assert(storage.medium().eraseAttempts() == protectedErases);
  assert(storage.writeRecording(82, pattern(32, 0x75), {12}));
  puts("PASS F7: boot gate and verified ownership prevent premature erase");
}
static void checkFullIndexBank() {
  StorageModel storage;
  assert(storage.format());
  const std::vector<uint8_t> empty;
  const std::vector<uint32_t> noBlocks;
  for (uint32_t i = 0; i < 126; ++i)
    assert(storage.commitRecording(1000 + i, empty, noBlocks));
  ScanResult scan = storage.scan();
  assert(scan.safe && scan.tierA.size() == 126);
  assert(!storage.commitRecording(2000, empty, noBlocks));
  assert(!storage.deleteRecording(1000));
  assert(storage.compact());  // header + 126 carry + READY exactly fills the other bank
  scan = storage.scan();
  assert(scan.safe && scan.activeBank == 1 && scan.tierA.size() == 126);
  assert(!storage.commitRecording(2000, empty, noBlocks));
  puts("PASS index capacity: 126-record boundary, full-bank refusal and exact-fit compaction");
}

int main() {
  static_assert(kDataBlocks == 496, "64KiB index must leave 496 aligned data blocks");
  checkCrcAndFormats();
  checkVirtualFatAndBoundaries();
  checkFullCapacity();
  checkTornDataAndRecords();
  checkConflictsAndCorruption();
  checkCompactionAndHighWater();
  checkReservationsAndExportGate();
  checkBootFenceFreshEraseAndSafetyLayers();
  checkStatusSnapshotAndLowBatteryStop();
  checkBootMutationGateAndOwnership();
  checkFullIndexBank();
  puts("ALL STORAGE V2 HOST TESTS PASSED (no physical device accessed)");
}
