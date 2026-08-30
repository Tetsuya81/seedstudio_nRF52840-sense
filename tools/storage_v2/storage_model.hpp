#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "../../pebble_format/pebble_format.h"

namespace storage_v2 {

constexpr uint32_t kMediumBytes = 2U * 1024U * 1024U;
constexpr uint32_t kEraseBytes = 4096;
constexpr uint32_t kPageBytes = 256;
constexpr uint32_t kIndexBytes = 64U * 1024U;
constexpr uint32_t kIndexBankBytes = 32U * 1024U;
constexpr uint32_t kDataBlocks = (kMediumBytes - kIndexBytes) / kEraseBytes;
constexpr uint32_t kPayloadBytes = kEraseBytes - 32;
constexpr uint32_t kIndexBase = kDataBlocks * kEraseBytes;
constexpr uint32_t kPagesPerBank = kIndexBankBytes / kPageBytes;
constexpr uint32_t kVirtualSectorBytes = 512;
constexpr uint32_t kVirtualSectors = 4003;

static_assert(kPageBytes == pebble_format::kPageBytes, "shared page size mismatch");
static_assert(kEraseBytes == pebble_format::kEraseBytes, "shared erase size mismatch");
static_assert(kDataBlocks == pebble_format::kDataBlocks, "shared data geometry mismatch");

uint32_t crc32(const uint8_t* data, size_t size);

class NorMedium {
 public:
  NorMedium();

  const std::vector<uint8_t>& bytes() const { return bytes_; }
  uint8_t at(uint32_t address) const;
  bool read(uint32_t address, uint8_t* out, size_t size) const;
  bool isAllFF(uint32_t address, size_t size) const;

  // cutBits is the number of low-to-high source bits which reach the medium.
  // A value >= 2048 completes the page program. A torn program still consumes
  // the page for the current erase epoch.
  bool programPage(uint32_t address, const uint8_t page[kPageBytes],
                   size_t cutBits = kPageBytes * 8U);

  // A torn erase changes only a prefix of bits to 1 and does not start a new
  // usable epoch. Only a complete erase makes pages programmable again.
  bool eraseBlock(uint32_t address, size_t cutBits = kEraseBytes * 8U);

  bool pageConsumed(uint32_t page) const;
  void corruptToZero(uint32_t address, uint8_t mask);
  uint64_t programAttempts() const { return programAttempts_; }
  uint64_t eraseAttempts() const { return eraseAttempts_; }

 private:
  std::vector<uint8_t> bytes_;
  std::vector<uint8_t> consumedPages_;
  uint64_t programAttempts_ = 0;
  uint64_t eraseAttempts_ = 0;
};

using DataHeader = pebble_format::DataHeader;

std::array<uint8_t, 32> encodeDataHeaderPage(const DataHeader& header);
bool decodeDataHeaderPage(const uint8_t* data, DataHeader* header);

using BankHeader = pebble_format::BankHeader;
using RecordType = pebble_format::RecordType;
using IndexRecord = pebble_format::IndexRecord;

std::array<uint8_t, kPageBytes> encodeBankHeaderPage(const BankHeader& header);
bool decodeBankHeaderPage(const uint8_t* data, uint16_t physicalBank,
                      BankHeader* header);
std::array<uint8_t, kPageBytes> encodeIndexRecordPage(const IndexRecord& record);
bool decodeIndexRecordPage(const uint8_t* data, IndexRecord* record);

struct CatalogEntry {
  uint32_t recId = 0;
  uint32_t byteLen = 0;
  uint32_t bodyCrc32 = 0;
  uint64_t time = 0;
  std::vector<uint32_t> physicalBlocks;
};

struct ScanResult {
  bool safe = true;
  bool deviceSafe = true;
  int activeBank = -1;
  uint32_t generation = 0;
  uint32_t nextRecId = 1;
  uint32_t nextSeq = 1;
  size_t quarantinedBlocks = 0;
  uint8_t lastOccupiedPage = 0;
  uint8_t nextWritePage = 0xFF;
  std::vector<CatalogEntry> tierA;
  std::vector<uint32_t> tierB;
  std::vector<uint32_t> committedUnverified;
  std::vector<uint32_t> isolated;
  std::vector<uint32_t> quarantinedPhysicalBlocks;
  std::vector<uint32_t> deleted;
  std::vector<std::string> issues;
};

enum class CompactFault {
  None,
  TornHeader,
  TornCarry,
  TornReady,
  StopAfterReady,
  TornOldErase,
};

class StorageModel {
 public:
  StorageModel();

  NorMedium& medium() { return medium_; }
  const NorMedium& medium() const { return medium_; }
  uint64_t mediaGen() const { return mediaGen_; }
  void restart();

  bool format(uint32_t nextRecId = 1, uint32_t nextSeq = 1);
  bool writeRecording(uint32_t recId, const std::vector<uint8_t>& audio,
                      const std::vector<uint32_t>& placement,
                      size_t tornBlock = SIZE_MAX,
                      size_t tornPage = SIZE_MAX,
                      size_t tornBits = kPageBytes * 8U);
  bool commitRecording(uint32_t recId, const std::vector<uint8_t>& audio,
                       const std::vector<uint32_t>& placement,
                       size_t tornBits = kPageBytes * 8U);
  bool deleteRecording(uint32_t recId,
                       size_t tornBits = kPageBytes * 8U);
  bool reclaimRecording(uint32_t recId, size_t tornBlock = SIZE_MAX,
                        size_t tornBits = kEraseBytes * 8U);
  bool compact(CompactFault fault = CompactFault::None);

  ScanResult scan(bool verifyBodies = true) const;

 private:
  NorMedium medium_;
  uint64_t mediaGen_ = 0;
  uint64_t bootEpoch_ = 1;
  bool indexCursorValid_ = false;
  bool indexMutationFaulted_ = false;
  int indexCursorBank_ = -1;
  uint16_t indexWritePage_ = 0;

  bool program(uint32_t address, const uint8_t page[kPageBytes], size_t bits);
  bool erase(uint32_t address, size_t bits = kEraseBytes * 8U);
  bool appendRecord(const IndexRecord& record, size_t bits);
};

class VirtualFat {
 public:
  bool build(const NorMedium& medium, const std::vector<CatalogEntry>& entries);
  bool build(const NorMedium& medium, const ScanResult& snapshot);
  uint64_t size() const { return static_cast<uint64_t>(kVirtualSectors) * 512U; }
  bool read(uint64_t offset, uint8_t* out, size_t size) const;
  std::vector<uint8_t> image() const;

 private:
  struct File {
    CatalogEntry entry;
    uint16_t firstCluster = 0;
    uint16_t clusterCount = 0;
  };

  const NorMedium* medium_ = nullptr;
  std::vector<File> files_;
  std::array<uint8_t, 512> boot_{};
  std::array<uint8_t, 1024> fat_{};
  std::array<uint8_t, 16384> root_{};
  std::string status_;
  uint16_t statusFirstCluster_ = 0;
  uint16_t statusClusterCount_ = 0;

  bool readFileByte(const File& file, uint32_t offset, uint8_t* value) const;
};

enum class StopState { Recording, CommitReserved, StoppedUnverified, SafeFault };

class LowBatteryStop {
 public:
  StopState state() const { return state_; }
  uint8_t pagePrograms() const { return pagePrograms_; }
  bool begin(bool commitPageReserved);
  bool finishPartialDataPage(bool programAndReadbackOk);
  bool programCommit(bool programAndReadbackOk);
 private:
  StopState state_ = StopState::Recording;
  uint8_t pagePrograms_ = 0;
};

enum class ExportState {
  IdleLocal,
  PendingAudio,
  ExportAudio,
  PendingRaw,
  ExportRaw,
  Releasing,
  Safe,
  Fault,
};

class ExportGate {
 public:
  ExportState state() const { return state_; }
  bool audioReady() const { return state_ == ExportState::ExportAudio; }
  bool rawReady() const { return state_ == ExportState::ExportRaw; }

  bool requestAudio(bool operationsStopped, bool ioComplete, bool noError,
                    uint64_t generationBefore, uint64_t generationAfter,
                    bool catalogValid);
  bool requestRaw(bool operationsStopped, bool readPathWorks);
  void enterSafe() { state_ = ExportState::Safe; }
  void enterFault() { state_ = ExportState::Fault; }
  void release();

 private:
  ExportState state_ = ExportState::IdleLocal;
};

struct PageReservation {
  uint32_t used = 0;
  uint32_t reservedCommits = 0;
  uint32_t carry = 0;
  bool bootFenceNeeded = false;

  uint32_t freePages() const { return used >= kPagesPerBank ? 0 : kPagesPerBank - used; }
  bool canAppend() const;
  bool canCreate() const;
  bool canDelete() const;
  bool canCompact() const;
};

}  // namespace storage_v2
