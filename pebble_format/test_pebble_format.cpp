#include "pebble_format.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace pebble_format;

static void formats() {
  static const uint8_t check[] = "123456789";
  assert(crc32(check, 9) == 0xCBF43926U);

  DataHeader data;
  initDataHeader(&data);
  data.recId = 7;
  data.blockIndex = 3;
  uint8_t page[kPageBytes];
  assert(encodeDataHeader(data, page));
  DataHeader decodedData;
  assert(decodeDataHeader(page, &decodedData));
  assert(decodedData.recId == 7 && decodedData.blockIndex == 3);

  BankHeader bank;
  initBankHeader(&bank);
  bank.bankId = 1;
  bank.generation = 9;
  assert(encodeBankHeader(bank, page));
  BankHeader decodedBank;
  assert(decodeBankHeader(page, 1, &decodedBank));
  assert(decodedBank.generation == 9);

  IndexRecord ready;
  initIndexRecord(&ready, RecordType::Ready);
  ready.seq = 10;
  ready.nextRecIdHW = 20;
  ready.nextSeqHW = 11;
  assert(encodeIndexRecord(ready, page));
  IndexRecord decodedRecord;
  assert(decodeIndexRecord(page, &decodedRecord));
  assert(decodedRecord.type == RecordType::Ready && decodedRecord.nextRecIdHW == 20);
}

static void bankScanAndFence() {
  uint8_t blank[kPageBytes];
  memset(blank, 0xFF, sizeof(blank));
  BankScanState scan;
  beginBankScan(&scan);

  IndexRecord record;
  initIndexRecord(&record, RecordType::Ready);
  record.seq = 1;
  record.nextRecIdHW = 2;
  record.nextSeqHW = 2;
  uint8_t page[kPageBytes];
  assert(encodeIndexRecord(record, page));
  scanBankPage(&scan, 1, page);
  scanBankPage(&scan, 2, blank);  // boot fence
  record.type = RecordType::Commit;
  record.seq = 2;
  record.recId = 1;
  record.byteLen = 0;
  record.bodyCrc32 = 0;
  record.blockCount = 0;
  record.firstCluster = UINT32_MAX;
  record.nextRecIdHW = UINT32_MAX;
  record.nextSeqHW = UINT32_MAX;
  assert(encodeIndexRecord(record, page));
  scanBankPage(&scan, 3, page);
  for (uint8_t p = 4; p < kPagesPerBank; ++p) scanBankPage(&scan, p, blank);
  finishBankScan(&scan);
  assert(scan.deviceSafe && scan.ready && scan.lastOccupied == 3 && scan.nextWritePage == 5);

  // Physical order, not adjacency, controls seq monotonicity.
  beginBankScan(&scan);
  record.seq = 5;
  assert(encodeIndexRecord(record, page));
  scanBankPage(&scan, 5, page);
  record.seq = 4;
  assert(encodeIndexRecord(record, page));
  scanBankPage(&scan, 9, page);
  finishBankScan(&scan);
  assert(!scan.deviceSafe);
}

static void reservations() {
  PageReservation r = {125, 1, 20, true};
  assert(rawFreePages(r) == 3);
  assert(usableFreePages(r) == 1);
  assert(canAppendRecord(r) && canCreateRecording(r) && canDeleteRecording(r));
  r.usedPages = 126;
  assert(usableFreePages(r) == 0);
  assert(!canAppendRecord(r) && !canCreateRecording(r) && !canDeleteRecording(r));
  r.carryRecords = 127;
  assert(!canCompact(r));
}

int main() {
  formats();
  bankScanAndFence();
  reservations();
  puts("ALL PEBBLE FORMAT TESTS PASSED (no STL, heap, device, or filesystem)");
}
