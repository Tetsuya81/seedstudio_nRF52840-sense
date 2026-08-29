#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

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

 private:
  std::vector<uint8_t> bytes_;
  std::vector<uint8_t> consumedPages_;
};

struct DataHeader {
  uint16_t version = 1;
  uint16_t flags = 0;
  uint32_t recId = 0;
  uint32_t blockIndex = 0;
  uint64_t startTime = UINT64_MAX;
};

std::array<uint8_t, 32> encodeDataHeader(const DataHeader& header);
bool decodeDataHeader(const uint8_t* data, DataHeader* header);

struct BankHeader {
  uint16_t version = 1;
  uint16_t bankId = 0;
  uint32_t generation = 0;
  uint32_t firstSeq = 0;
  uint32_t nextRecIdHW = 1;
  uint32_t nextSeqHW = 1;
  uint64_t createdTime = 0;
};

enum class RecordType : uint8_t { Commit = 1, Delete = 2, Ready = 3 };

struct IndexRecord {
  RecordType type = RecordType::Ready;
  uint8_t version = 1;
  uint32_t seq = 0;
  uint32_t recId = UINT32_MAX;
  uint32_t byteLen = UINT32_MAX;
  uint32_t bodyCrc32 = UINT32_MAX;
  uint32_t blockCount = UINT32_MAX;
  uint32_t firstCluster = UINT32_MAX;
  uint64_t time = 0;
  uint32_t nextRecIdHW = UINT32_MAX;
  uint32_t nextSeqHW = UINT32_MAX;
};

std::array<uint8_t, kPageBytes> encodeBankHeader(const BankHeader& header);
bool decodeBankHeader(const uint8_t* data, uint16_t physicalBank,
                      BankHeader* header);
std::array<uint8_t, kPageBytes> encodeIndexRecord(const IndexRecord& record);
bool decodeIndexRecord(const uint8_t* data, IndexRecord* record);

struct CatalogEntry {
  uint32_t recId = 0;
  uint32_t byteLen = 0;
  uint32_t bodyCrc32 = 0;
  uint64_t time = 0;
  std::vector<uint32_t> physicalBlocks;
};

struct ScanResult {
  bool safe = true;
  int activeBank = -1;
  uint32_t generation = 0;
  uint32_t nextRecId = 1;
  uint32_t nextSeq = 1;
  size_t quarantinedBlocks = 0;
  std::vector<CatalogEntry> tierA;
  std::vector<uint32_t> tierB;
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

  ScanResult scan() const;

 private:
  NorMedium medium_;
  uint64_t mediaGen_ = 0;

  bool program(uint32_t address, const uint8_t page[kPageBytes], size_t bits);
  bool erase(uint32_t address, size_t bits = kEraseBytes * 8U);
  bool appendRecord(const IndexRecord& record, size_t bits);
};

class VirtualFat {
 public:
  bool build(const NorMedium& medium, const std::vector<CatalogEntry>& entries);
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

  bool readFileByte(const File& file, uint32_t offset, uint8_t* value) const;
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

  uint32_t freePages() const { return used >= kPagesPerBank ? 0 : kPagesPerBank - used; }
  bool canAppend() const;
  bool canCreate() const;
  bool canDelete() const;
  bool canCompact() const;
};

}  // namespace storage_v2
