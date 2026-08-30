#include "pebble_format.h"

#include <string.h>

namespace pebble_format {
namespace {

void put16(uint8_t* p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
}

void put32(uint8_t* p, uint32_t value) {
  for (uint8_t i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (i * 8));
}

void put64(uint8_t* p, uint64_t value) {
  for (uint8_t i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(value >> (i * 8));
}

uint16_t get16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t get32(const uint8_t* p) {
  uint32_t value = 0;
  for (uint8_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(p[i]) << (i * 8);
  return value;
}

uint64_t get64(const uint8_t* p) {
  uint64_t value = 0;
  for (uint8_t i = 0; i < 8; ++i) value |= static_cast<uint64_t>(p[i]) << (i * 8);
  return value;
}

bool hasMagic(const uint8_t* p, const char text[5]) {
  return memcmp(p, text, 4) == 0;
}

bool validCommonRecordFields(const IndexRecord& record) {
  const uint8_t type = static_cast<uint8_t>(record.type);
  if (record.version != 1 || type < static_cast<uint8_t>(RecordType::Commit) ||
      type > static_cast<uint8_t>(RecordType::Ready))
    return false;
  if (record.type == RecordType::Ready) {
    return record.recId == UINT32_MAX && record.byteLen == UINT32_MAX &&
           record.bodyCrc32 == UINT32_MAX && record.blockCount == UINT32_MAX &&
           record.firstCluster == UINT32_MAX && record.nextRecIdHW != UINT32_MAX &&
           record.nextSeqHW != UINT32_MAX;
  }
  if (record.recId == UINT32_MAX || record.nextRecIdHW != UINT32_MAX ||
      record.nextSeqHW != UINT32_MAX)
    return false;
  if (record.type == RecordType::Delete)
    return record.byteLen == UINT32_MAX && record.bodyCrc32 == UINT32_MAX &&
           record.blockCount == UINT32_MAX && record.firstCluster == UINT32_MAX;
  return true;
}

}  // namespace

uint32_t crc32(const uint8_t* data, size_t size) {
  if (!data && size != 0) return 0;
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
  }
  return crc ^ 0xFFFFFFFFU;
}

bool isErased(const uint8_t* data, size_t size) {
  if (!data && size != 0) return false;
  for (size_t i = 0; i < size; ++i)
    if (data[i] != 0xFF) return false;
  return true;
}

void initDataHeader(DataHeader* header) {
  if (!header) return;
  header->version = 1;
  header->flags = 0;
  header->recId = 0;
  header->blockIndex = 0;
  header->startTime = UINT64_MAX;
}

void initBankHeader(BankHeader* header) {
  if (!header) return;
  header->version = 1;
  header->bankId = 0;
  header->generation = 0;
  header->firstSeq = 0;
  header->nextRecIdHW = 1;
  header->nextSeqHW = 1;
  header->createdTime = 0;
}

void initIndexRecord(IndexRecord* record, RecordType type) {
  if (!record) return;
  record->type = type;
  record->version = 1;
  record->seq = 0;
  record->recId = UINT32_MAX;
  record->byteLen = UINT32_MAX;
  record->bodyCrc32 = UINT32_MAX;
  record->blockCount = UINT32_MAX;
  record->firstCluster = UINT32_MAX;
  record->time = 0;
  record->nextRecIdHW = UINT32_MAX;
  record->nextSeqHW = UINT32_MAX;
}

bool encodeDataHeader(const DataHeader& header, uint8_t out[kDataHeaderBytes]) {
  if (!out || header.version != 1 || header.flags != 0) return false;
  memset(out, 0xFF, kDataHeaderBytes);
  memcpy(out, "PRB1", 4);
  put16(out + 4, header.version);
  put16(out + 6, header.flags);
  put32(out + 8, header.recId);
  put32(out + 12, header.blockIndex);
  put64(out + 16, header.startTime);
  put32(out + 24, UINT32_MAX);
  put32(out + 28, crc32(out, 28));
  return true;
}

bool decodeDataHeader(const uint8_t data[kDataHeaderBytes], DataHeader* header) {
  if (!data || !header || !hasMagic(data, "PRB1") || get16(data + 4) != 1 ||
      get16(data + 6) != 0 || get32(data + 24) != UINT32_MAX ||
      get32(data + 28) != crc32(data, 28)) return false;
  header->version = get16(data + 4);
  header->flags = get16(data + 6);
  header->recId = get32(data + 8);
  header->blockIndex = get32(data + 12);
  header->startTime = get64(data + 16);
  return true;
}

bool encodeBankHeader(const BankHeader& header, uint8_t out[kPageBytes]) {
  if (!out || header.version != 1 || header.bankId > 1) return false;
  memset(out, 0xFF, kPageBytes);
  memcpy(out, "PRBH", 4);
  put16(out + 4, header.version);
  put16(out + 6, header.bankId);
  put32(out + 8, header.generation);
  put32(out + 12, header.firstSeq);
  put32(out + 16, header.nextRecIdHW);
  put32(out + 20, header.nextSeqHW);
  put64(out + 24, header.createdTime);
  put32(out + 252, crc32(out, 252));
  return true;
}

bool decodeBankHeader(const uint8_t data[kPageBytes], uint16_t physicalBank,
                      BankHeader* header) {
  if (!data || !header || physicalBank > 1 || !hasMagic(data, "PRBH") ||
      get16(data + 4) != 1 || get16(data + 6) != physicalBank ||
      !isErased(data + 32, 220) || get32(data + 252) != crc32(data, 252)) return false;
  header->version = get16(data + 4);
  header->bankId = get16(data + 6);
  header->generation = get32(data + 8);
  header->firstSeq = get32(data + 12);
  header->nextRecIdHW = get32(data + 16);
  header->nextSeqHW = get32(data + 20);
  header->createdTime = get64(data + 24);
  return true;
}

bool encodeIndexRecord(const IndexRecord& record, uint8_t out[kPageBytes]) {
  if (!out || !validCommonRecordFields(record)) return false;
  memset(out, 0xFF, kPageBytes);
  memcpy(out, "PRR1", 4);
  out[4] = static_cast<uint8_t>(record.type);
  out[5] = record.version;
  put32(out + 6, record.seq);
  put32(out + 10, record.recId);
  put32(out + 14, record.byteLen);
  put32(out + 18, record.bodyCrc32);
  put32(out + 22, record.blockCount);
  put32(out + 26, record.firstCluster);
  put64(out + 30, record.time);
  put32(out + 38, record.nextRecIdHW);
  put32(out + 42, record.nextSeqHW);
  put32(out + 252, crc32(out, 252));
  return true;
}

bool decodeIndexRecord(const uint8_t data[kPageBytes], IndexRecord* record) {
  if (!data || !record || !hasMagic(data, "PRR1") || data[5] != 1 ||
      data[4] < static_cast<uint8_t>(RecordType::Commit) ||
      data[4] > static_cast<uint8_t>(RecordType::Ready) ||
      !isErased(data + 46, 206) || get32(data + 252) != crc32(data, 252)) return false;
  record->type = static_cast<RecordType>(data[4]);
  record->version = data[5];
  record->seq = get32(data + 6);
  record->recId = get32(data + 10);
  record->byteLen = get32(data + 14);
  record->bodyCrc32 = get32(data + 18);
  record->blockCount = get32(data + 22);
  record->firstCluster = get32(data + 26);
  record->time = get64(data + 30);
  record->nextRecIdHW = get32(data + 38);
  record->nextSeqHW = get32(data + 42);
  return validCommonRecordFields(*record);
}

void beginBankScan(BankScanState* state) {
  if (!state) return;
  state->deviceSafe = true;
  state->ready = false;
  state->hasValidRecord = false;
  state->hasOccupiedPage = false;
  state->lastOccupied = 0;
  state->nextWritePage = 0xFF;
  state->lastValidSeq = 0;
}

void scanBankPage(BankScanState* state, uint8_t pageIndex,
                  const uint8_t page[kPageBytes]) {
  if (!state || !page || pageIndex == 0 || pageIndex >= kPagesPerBank) {
    if (state) state->deviceSafe = false;
    return;
  }
  if (!isErased(page, kPageBytes)) {
    state->hasOccupiedPage = true;
    if (pageIndex > state->lastOccupied) state->lastOccupied = pageIndex;
  }
  IndexRecord record;
  if (!decodeIndexRecord(page, &record)) return;
  if (state->hasValidRecord && record.seq <= state->lastValidSeq)
    state->deviceSafe = false;
  state->hasValidRecord = true;
  state->lastValidSeq = record.seq;
  if (record.type == RecordType::Ready) state->ready = true;
}

void finishBankScan(BankScanState* state) {
  if (!state) return;
  if (!state->hasOccupiedPage) {
    state->nextWritePage = 1;  // only used for diagnostics; a valid bank has READY
    return;
  }
  const uint16_t next = static_cast<uint16_t>(state->lastOccupied) + 2U;
  state->nextWritePage = next < kPagesPerBank ? static_cast<uint8_t>(next) : 0xFF;
}

BankChoice chooseActiveBank(bool bank0Valid, uint32_t generation0,
                            bool bank1Valid, uint32_t generation1) {
  BankChoice choice = {true, -1};
  if (bank0Valid && bank1Valid) {
    if (generation0 == generation1) {
      choice.deviceSafe = false;
      return choice;
    }
    choice.activeBank = generation0 > generation1 ? 0 : 1;
  } else if (bank0Valid) {
    choice.activeBank = 0;
  } else if (bank1Valid) {
    choice.activeBank = 1;
  } else {
    choice.deviceSafe = false;
  }
  return choice;
}

uint32_t expectedBlockCount(uint32_t byteLen) {
  return static_cast<uint32_t>((static_cast<uint64_t>(byteLen) + kPayloadBytes - 1U) /
                               kPayloadBytes);
}

uint16_t rawFreePages(const PageReservation& r) {
  return r.usedPages >= kPagesPerBank ? 0 : static_cast<uint16_t>(kPagesPerBank - r.usedPages);
}

uint16_t usableFreePages(const PageReservation& r) {
  uint16_t free = rawFreePages(r);
  const uint32_t held = static_cast<uint32_t>(r.reservedCommits) +
                        (r.bootFenceNeeded ? 1U : 0U);
  return held >= free ? 0 : static_cast<uint16_t>(free - held);
}

bool canAppendRecord(const PageReservation& r) {
  return usableFreePages(r) >= 1;
}

bool canCreateRecording(const PageReservation& r) {
  return usableFreePages(r) >= 1 &&
         static_cast<uint32_t>(r.carryRecords) + r.reservedCommits + 1U <= 126U;
}

bool canDeleteRecording(const PageReservation& r) {
  return usableFreePages(r) >= 1;
}

bool canCompact(const PageReservation& r) {
  return r.carryRecords <= 126;
}

}  // namespace pebble_format
