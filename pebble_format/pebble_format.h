#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pebble_format {

static const uint32_t kPageBytes = 256;
static const uint32_t kEraseBytes = 4096;
static const uint32_t kDataHeaderBytes = 32;
static const uint32_t kPayloadBytes = kEraseBytes - kDataHeaderBytes;
static const uint32_t kPagesPerBank = 128;
static const uint32_t kDataBlocks = 496;

enum class RecordType : uint8_t {
  Commit = 1,
  Delete = 2,
  Ready = 3,
};

struct DataHeader {
  uint16_t version = 1;
  uint16_t flags = 0;
  uint32_t recId = 0;
  uint32_t blockIndex = 0;
  uint64_t startTime = UINT64_MAX;
};

struct BankHeader {
  uint16_t version = 1;
  uint16_t bankId = 0;
  uint32_t generation = 0;
  uint32_t firstSeq = 0;
  uint32_t nextRecIdHW = 1;
  uint32_t nextSeqHW = 1;
  uint64_t createdTime = 0;
};

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

struct BankScanState {
  bool deviceSafe;
  bool ready;
  bool hasValidRecord;
  bool hasOccupiedPage;
  uint8_t lastOccupied;
  uint8_t nextWritePage;
  uint32_t lastValidSeq;
  uint8_t pagesScanned;
  uint8_t nextExpectedPage;
};

struct DataScanState {
  bool deviceSafe;
  uint16_t blocksScanned;
  uint16_t nextExpectedBlock;
};


struct BankChoice {
  bool deviceSafe;
  int8_t activeBank;
};

struct PageReservation {
  uint16_t usedPages;
  uint16_t reservedCommits;
  uint16_t carryRecords;
  bool bootFenceNeeded;
};

uint32_t crc32(const uint8_t* data, size_t size);
bool isErased(const uint8_t* data, size_t size);

void initDataHeader(DataHeader* header);
void initBankHeader(BankHeader* header);
void initIndexRecord(IndexRecord* record, RecordType type);

bool encodeDataHeader(const DataHeader& header, uint8_t out[kDataHeaderBytes]);
bool decodeDataHeader(const uint8_t data[kDataHeaderBytes], DataHeader* header);
bool encodeBankHeader(const BankHeader& header, uint8_t out[kPageBytes]);
bool decodeBankHeader(const uint8_t data[kPageBytes], uint16_t physicalBank,
                      BankHeader* header);
bool encodeIndexRecord(const IndexRecord& record, uint8_t out[kPageBytes]);
bool decodeIndexRecord(const uint8_t data[kPageBytes], IndexRecord* record);

void beginBankScan(BankScanState* state);
void scanBankPage(BankScanState* state, uint8_t pageIndex,
                  const uint8_t page[kPageBytes]);
void finishBankScan(BankScanState* state);
void beginDataScan(DataScanState* state);
void scanDataBlock(DataScanState* state, uint16_t blockIndex);
void finishDataScan(DataScanState* state);

BankChoice chooseActiveBank(bool bank0Valid, uint32_t generation0,
                            bool bank1Valid, uint32_t generation1);

uint32_t expectedBlockCount(uint32_t byteLen);
uint16_t rawFreePages(const PageReservation& reservation);
uint16_t usableFreePages(const PageReservation& reservation);
bool canAppendRecord(const PageReservation& reservation);
bool canCreateRecording(const PageReservation& reservation);
bool canDeleteRecording(const PageReservation& reservation);
bool canCompact(const PageReservation& reservation);

}  // namespace pebble_format
