#include "storage_model.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <sstream>

namespace storage_v2 {
namespace {

void put16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t* p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t bankAddress(unsigned bank) {
  return kIndexBase + bank * kIndexBankBytes;
}
uint32_t plusOneSaturated(uint32_t value) {
  return value >= 0xFFFFFFFEU ? 0xFFFFFFFEU : value + 1U;
}

struct BankScan {
  bool headerValid = false;
  bool ready = false;
  bool safe = true;
  uint8_t lastOccupied = 0;
  uint8_t nextWritePage = 0xFF;
  BankHeader header;
  std::vector<IndexRecord> records;
  std::vector<std::string> issues;
};

BankScan scanBank(const NorMedium& medium, unsigned bank) {
  BankScan result;
  std::array<uint8_t, kPageBytes> page{};
  medium.read(bankAddress(bank), page.data(), page.size());
  result.headerValid = decodeBankHeaderPage(page.data(), bank, &result.header);
  if (!result.headerValid) return result;

  pebble_format::BankScanState state;
  pebble_format::beginBankScan(&state);
  for (uint32_t p = 1; p < kPagesPerBank; ++p) {
    medium.read(bankAddress(bank) + p * kPageBytes, page.data(), page.size());
    pebble_format::scanBankPage(&state, static_cast<uint8_t>(p), page.data());
    IndexRecord record;
    if (!decodeIndexRecordPage(page.data(), &record)) continue;  // occupied torn page
    result.records.push_back(record);
  }
  pebble_format::finishBankScan(&state);
  result.safe = state.deviceSafe;
  result.ready = state.ready;
  result.lastOccupied = state.lastOccupied;
  result.nextWritePage = state.nextWritePage;
  if (!result.safe)
    result.issues.push_back("valid index seq is not strictly increasing in physical order");
  return result;
}

struct BlockScan {
  std::map<uint32_t, std::map<uint32_t, std::vector<uint32_t>>> byRecording;
  size_t quarantined = 0;
  std::vector<uint32_t> quarantinedPhysical;
  uint32_t occupied = 0;
};

BlockScan scanBlocks(const NorMedium& medium) {
  BlockScan result;
  std::array<uint8_t, 32> headerBytes{};
  for (uint32_t block = 0; block < kDataBlocks; ++block) {
    const uint32_t address = block * kEraseBytes;
    if (medium.isAllFF(address, kEraseBytes)) continue;
    ++result.occupied;
    medium.read(address, headerBytes.data(), headerBytes.size());
    DataHeader header;
    if (!decodeDataHeaderPage(headerBytes.data(), &header)) {
      ++result.quarantined;
      result.quarantinedPhysical.push_back(block);
      continue;
    }
    result.byRecording[header.recId][header.blockIndex].push_back(block);
  }
  return result;
}

bool readPayload(const NorMedium& medium,
                 const std::vector<uint32_t>& blocks,
                 uint32_t byteLen,
                 std::vector<uint8_t>* payload) {
  if (byteLen > blocks.size() * static_cast<uint64_t>(kPayloadBytes)) return false;
  payload->clear();
  payload->reserve(byteLen);
  uint32_t remaining = byteLen;
  for (uint32_t block : blocks) {
    const uint32_t take = std::min<uint32_t>(remaining, kPayloadBytes);
    const size_t old = payload->size();
    payload->resize(old + take);
    if (take && !medium.read(block * kEraseBytes + 32, payload->data() + old, take)) return false;
    remaining -= take;
    if (!remaining) break;
  }
  return remaining == 0;
}

void setFat12(std::array<uint8_t, 1024>* fat, uint16_t cluster, uint16_t value) {
  const size_t offset = cluster + cluster / 2;
  value &= 0x0FFF;
  if ((cluster & 1U) == 0) {
    (*fat)[offset] = static_cast<uint8_t>(value);
    (*fat)[offset + 1] = static_cast<uint8_t>(((*fat)[offset + 1] & 0xF0) | (value >> 8));
  } else {
    (*fat)[offset] = static_cast<uint8_t>(((*fat)[offset] & 0x0F) | (value << 4));
    (*fat)[offset + 1] = static_cast<uint8_t>(value >> 4);
  }
}

uint16_t fatDate(uint64_t time) {
  (void)time;
  return static_cast<uint16_t>(((2026 - 1980) << 9) | (8 << 5) | 29);
}
uint16_t fatTime(uint64_t time) {
  (void)time;
  return 0;
}

}  // namespace

uint32_t crc32(const uint8_t* data, size_t size) {
  return pebble_format::crc32(data, size);
}

NorMedium::NorMedium()
    : bytes_(kMediumBytes, 0xFF), consumedPages_(kMediumBytes / kPageBytes, 0) {}

uint8_t NorMedium::at(uint32_t address) const {
  return address < bytes_.size() ? bytes_[address] : 0;
}

bool NorMedium::read(uint32_t address, uint8_t* out, size_t size) const {
  if (!out || address > bytes_.size() || size > bytes_.size() - address) return false;
  std::memcpy(out, bytes_.data() + address, size);
  return true;
}

bool NorMedium::isAllFF(uint32_t address, size_t size) const {
  return address <= bytes_.size() && size <= bytes_.size() - address &&
         pebble_format::isErased(bytes_.data() + address, size);
}

bool NorMedium::programPage(uint32_t address, const uint8_t page[kPageBytes], size_t cutBits) {
  ++programAttempts_;
  if (!page || address % kPageBytes != 0 || address > kMediumBytes - kPageBytes) return false;
  const uint32_t pageNumber = address / kPageBytes;
  if (consumedPages_[pageNumber]) return false;
  consumedPages_[pageNumber] = 1;
  const size_t bits = std::min<size_t>(cutBits, kPageBytes * 8U);
  for (size_t bit = 0; bit < bits; ++bit) {
    const size_t byte = bit / 8;
    const uint8_t mask = static_cast<uint8_t>(1U << (bit % 8));
    if ((page[byte] & mask) == 0) bytes_[address + byte] &= static_cast<uint8_t>(~mask);
  }
  return bits == kPageBytes * 8U;
}

bool NorMedium::eraseBlock(uint32_t address, size_t cutBits) {
  ++eraseAttempts_;
  if (address % kEraseBytes != 0 || address > kMediumBytes - kEraseBytes) return false;
  const size_t bits = std::min<size_t>(cutBits, kEraseBytes * 8U);
  for (size_t bit = 0; bit < bits; ++bit)
    bytes_[address + bit / 8] |= static_cast<uint8_t>(1U << (bit % 8));
  if (bits != kEraseBytes * 8U) return false;
  std::fill(consumedPages_.begin() + address / kPageBytes,
            consumedPages_.begin() + (address + kEraseBytes) / kPageBytes, 0);
  return true;
}

bool NorMedium::pageConsumed(uint32_t page) const {
  return page < consumedPages_.size() && consumedPages_[page] != 0;
}

void NorMedium::corruptToZero(uint32_t address, uint8_t mask) {
  if (address < bytes_.size()) bytes_[address] &= static_cast<uint8_t>(~mask);
}

std::array<uint8_t, 32> encodeDataHeaderPage(const DataHeader& h) {
  std::array<uint8_t, 32> out{};
  if (!pebble_format::encodeDataHeader(h, out.data())) out.fill(0);
  return out;
}

bool decodeDataHeaderPage(const uint8_t* p, DataHeader* h) {
  return pebble_format::decodeDataHeader(p, h);
}

std::array<uint8_t, kPageBytes> encodeBankHeaderPage(const BankHeader& h) {
  std::array<uint8_t, kPageBytes> out{};
  if (!pebble_format::encodeBankHeader(h, out.data())) out.fill(0);
  return out;
}

bool decodeBankHeaderPage(const uint8_t* p, uint16_t physicalBank, BankHeader* h) {
  return pebble_format::decodeBankHeader(p, physicalBank, h);
}

std::array<uint8_t, kPageBytes> encodeIndexRecordPage(const IndexRecord& r) {
  std::array<uint8_t, kPageBytes> out{};
  if (!pebble_format::encodeIndexRecord(r, out.data())) out.fill(0);
  return out;
}

bool decodeIndexRecordPage(const uint8_t* p, IndexRecord* r) {
  return pebble_format::decodeIndexRecord(p, r);
}

StorageModel::StorageModel() = default;

void StorageModel::restart() {
  ++bootEpoch_;
  indexCursorValid_ = false;
  indexMutationFaulted_ = false;
  indexCursorBank_ = -1;
  indexWritePage_ = 0;
  const ScanResult boot = scan();
  if (boot.deviceSafe && boot.activeBank >= 0) {
    indexCursorValid_ = true;
    indexCursorBank_ = boot.activeBank;
    indexWritePage_ = boot.nextWritePage;
  }
}

bool StorageModel::program(uint32_t address, const uint8_t page[kPageBytes], size_t bits) {
  const bool ok = medium_.programPage(address, page, bits);
  ++mediaGen_;  // attempted program changes the snapshot generation, including torn writes
  return ok;
}

bool StorageModel::erase(uint32_t address, size_t bits) {
  const bool ok = medium_.eraseBlock(address, bits);
  ++mediaGen_;
  return ok;
}

bool StorageModel::format(uint32_t nextRecId, uint32_t nextSeq) {
  for (uint32_t off = 0; off < kIndexBankBytes; off += kEraseBytes) {
    if (!erase(bankAddress(0) + off) ||
        !medium_.isAllFF(bankAddress(0) + off, kEraseBytes)) return false;
  }
  BankHeader header;
  header.bankId = 0;
  header.generation = 1;
  header.firstSeq = nextSeq;
  header.nextRecIdHW = nextRecId;
  header.nextSeqHW = plusOneSaturated(nextSeq);
  const auto headerPage = encodeBankHeaderPage(header);
  if (!program(bankAddress(0), headerPage.data(), kPageBytes * 8U)) return false;

  IndexRecord ready;
  ready.type = RecordType::Ready;
  ready.seq = nextSeq;
  ready.nextRecIdHW = nextRecId;
  ready.nextSeqHW = plusOneSaturated(nextSeq);
  const auto readyPage = encodeIndexRecordPage(ready);
  if (!program(bankAddress(0) + kPageBytes, readyPage.data(), kPageBytes * 8U)) return false;
  indexCursorValid_ = true;
  indexMutationFaulted_ = false;
  indexCursorBank_ = 0;
  indexWritePage_ = 2;
  return true;
}

bool StorageModel::writeRecording(uint32_t recId, const std::vector<uint8_t>& audio,
                                  const std::vector<uint32_t>& placement,
                                  size_t tornBlock, size_t tornPage, size_t tornBits) {
  const size_t needed = (audio.size() + kPayloadBytes - 1) / kPayloadBytes;
  if (needed != placement.size()) return false;
  size_t source = 0;
  for (size_t i = 0; i < placement.size(); ++i) {
    if (placement[i] >= kDataBlocks) return false;
    const uint32_t blockAddress = placement[i] * kEraseBytes;
    if (!erase(blockAddress) || !medium_.isAllFF(blockAddress, kEraseBytes)) return false;
    std::array<uint8_t, kEraseBytes> block;
    block.fill(0xFF);
    DataHeader header;
    header.recId = recId;
    header.blockIndex = static_cast<uint32_t>(i);
    header.startTime = i == 0 ? 1 : UINT64_MAX;
    const auto encoded = encodeDataHeaderPage(header);
    std::copy(encoded.begin(), encoded.end(), block.begin());
    const size_t take = std::min<size_t>(kPayloadBytes, audio.size() - source);
    std::copy(audio.begin() + source, audio.begin() + source + take, block.begin() + 32);
    source += take;
    const size_t bytesInBlock = 32 + take;
    const size_t pages = std::max<size_t>(1, (bytesInBlock + kPageBytes - 1) / kPageBytes);
    for (size_t page = 0; page < pages; ++page) {
      const bool tear = i == tornBlock && page == tornPage;
      if (!program(placement[i] * kEraseBytes + page * kPageBytes,
                   block.data() + page * kPageBytes,
                   tear ? tornBits : kPageBytes * 8U)) return false;
    }
  }
  return true;
}

bool StorageModel::appendRecord(const IndexRecord& record, size_t bits) {
  const ScanResult current = scan();
  if (!current.deviceSafe || current.activeBank < 0 || indexMutationFaulted_) return false;
  if (!indexCursorValid_ || indexCursorBank_ != current.activeBank) {
    indexCursorValid_ = true;
    indexCursorBank_ = current.activeBank;
    indexWritePage_ = current.nextWritePage;
  }
  if (indexWritePage_ >= kPagesPerBank) return false;
  const uint32_t address = bankAddress(static_cast<unsigned>(indexCursorBank_)) +
                           indexWritePage_ * kPageBytes;
  if (!medium_.isAllFF(address, kPageBytes) || medium_.pageConsumed(address / kPageBytes)) {
    indexMutationFaulted_ = true;
    return false;
  }
  const auto encoded = encodeIndexRecordPage(record);
  ++indexWritePage_;  // a page-program target is never reused in this boot
  if (!program(address, encoded.data(), bits)) {
    indexMutationFaulted_ = true;
    return false;
  }
  return true;
}

bool StorageModel::commitRecording(uint32_t recId, const std::vector<uint8_t>& audio,
                                   const std::vector<uint32_t>& placement, size_t tornBits) {
  const ScanResult before = scan();
  if (!before.safe || before.activeBank < 0 || before.nextSeq >= 0xFFFFFFFEU) return false;
  IndexRecord record;
  record.type = RecordType::Commit;
  record.seq = before.nextSeq;
  record.recId = recId;
  record.byteLen = static_cast<uint32_t>(audio.size());
  record.bodyCrc32 = crc32(audio.data(), audio.size());
  record.blockCount = static_cast<uint32_t>(placement.size());
  record.firstCluster = placement.empty() ? UINT32_MAX : placement.front();
  record.time = 1;
  return appendRecord(record, tornBits);
}

bool StorageModel::deleteRecording(uint32_t recId, size_t tornBits) {
  const ScanResult before = scan();
  if (!before.safe || before.activeBank < 0 || before.nextSeq >= 0xFFFFFFFEU) return false;
  IndexRecord record;
  record.type = RecordType::Delete;
  record.seq = before.nextSeq;
  record.recId = recId;
  record.time = 1;
  return appendRecord(record, tornBits);
}

bool StorageModel::reclaimRecording(uint32_t recId, size_t tornBlock, size_t tornBits) {
  const ScanResult result = scan();
  if (std::find(result.deleted.begin(), result.deleted.end(), recId) == result.deleted.end()) return false;
  BlockScan blocks = scanBlocks(medium_);
  auto found = blocks.byRecording.find(recId);
  if (found == blocks.byRecording.end()) return true;
  size_t ordinal = 0;
  for (const auto& item : found->second) {
    for (uint32_t block : item.second) {
      const bool tear = ordinal++ == tornBlock;
      if (!erase(block * kEraseBytes, tear ? tornBits : kEraseBytes * 8U)) return false;
    }
  }
  return true;
}

ScanResult StorageModel::scan(bool verifyBodies) const {
  ScanResult result;
  result.mediaGeneration = mediaGen_;
  const BankScan a = scanBank(medium_, 0);
  const BankScan b = scanBank(medium_, 1);
  const BankScan* bank = nullptr;
  const pebble_format::BankChoice choice = pebble_format::chooseActiveBank(
      a.headerValid && a.ready, a.header.generation,
      b.headerValid && b.ready, b.header.generation);
  result.deviceSafe = choice.deviceSafe;
  result.safe = result.deviceSafe;
  result.activeBank = choice.activeBank;
  if (choice.activeBank == 0) bank = &a;
  if (choice.activeBank == 1) bank = &b;
  if (!bank) {
    result.issues.push_back((a.headerValid && a.ready && b.headerValid && b.ready)
                                ? "both banks have the same generation"
                                : "no ready index bank");
    const BlockScan unowned = scanBlocks(medium_);
    result.quarantinedBlocks = unowned.quarantined;
    result.freeDataBlocks = kDataBlocks - unowned.occupied;
    result.capacityPressure = CapacityPressure::Index;
    result.quarantinedPhysicalBlocks = unowned.quarantinedPhysical;
    for (const auto& item : unowned.byRecording) {
      result.tierB.push_back(item.first);
      result.incomplete.push_back(item.first);
      result.nextRecId = std::max(result.nextRecId, plusOneSaturated(item.first));
    }
    return result;
  }
  result.deviceSafe = result.deviceSafe && bank->safe;
  if (indexMutationFaulted_) {
    result.deviceSafe = false;
    result.issues.push_back("index target was not erased or a page program failed in this boot");
  }
  result.safe = result.deviceSafe;
  result.issues.insert(result.issues.end(), bank->issues.begin(), bank->issues.end());
  result.generation = bank->header.generation;
  result.lastOccupiedPage = bank->lastOccupied;
  result.nextWritePage = bank->nextWritePage;
  result.freeIndexPages = bank->nextWritePage == 0xFF ? 0 : kPagesPerBank - bank->nextWritePage;
  result.nextRecId = bank->header.nextRecIdHW;
  result.nextSeq = bank->header.nextSeqHW;

  std::map<uint32_t, IndexRecord> commits;
  std::map<uint32_t, uint32_t> deletes;
  for (const IndexRecord& r : bank->records) {
    result.nextSeq = std::max(result.nextSeq, plusOneSaturated(r.seq));
    if (r.type == RecordType::Ready) {
      result.nextRecId = std::max(result.nextRecId, r.nextRecIdHW);
      result.nextSeq = std::max(result.nextSeq, r.nextSeqHW);
    } else {
      result.nextRecId = std::max(result.nextRecId, plusOneSaturated(r.recId));
      if (r.type == RecordType::Commit) commits[r.recId] = r;
      if (r.type == RecordType::Delete) deletes[r.recId] = r.seq;
    }
  }

  BlockScan blocks = scanBlocks(medium_);
  result.quarantinedBlocks = blocks.quarantined;
  result.quarantinedPhysicalBlocks = blocks.quarantinedPhysical;
  result.freeDataBlocks = kDataBlocks - blocks.occupied;
  if (result.freeDataBlocks == 0) result.capacityPressure = CapacityPressure::Data;
  else if (result.freeIndexPages == 0) result.capacityPressure = CapacityPressure::Index;
  else result.capacityPressure =
      static_cast<uint64_t>(result.freeDataBlocks) * 127U <=
              static_cast<uint64_t>(result.freeIndexPages) * kDataBlocks
          ? CapacityPressure::Data : CapacityPressure::Index;
  if (blocks.quarantined) {
    result.issues.push_back("data block quarantined: non-blank without valid PRB1");
  }
  for (const auto& recording : blocks.byRecording)
    result.nextRecId = std::max(result.nextRecId, plusOneSaturated(recording.first));

  for (const auto& item : commits) {
    const IndexRecord& commit = item.second;
    auto deleted = deletes.find(item.first);
    if (deleted != deletes.end() && deleted->second > commit.seq) {
      result.deleted.push_back(item.first);
      continue;
    }
    CatalogEntry entry;
    entry.recId = item.first;
    entry.byteLen = commit.byteLen;
    entry.bodyCrc32 = commit.bodyCrc32;
    entry.time = commit.time;
    bool structurallyValid = true;
    auto recBlocks = blocks.byRecording.find(item.first);
    const uint32_t expectedBlocks =
        static_cast<uint32_t>((static_cast<uint64_t>(commit.byteLen) + kPayloadBytes - 1) /
                              kPayloadBytes);
    if (commit.blockCount != expectedBlocks) structurallyValid = false;
    if (commit.blockCount == 0) {
      structurallyValid = structurallyValid && commit.byteLen == 0 &&
                          commit.firstCluster == UINT32_MAX;
    } else if (recBlocks == blocks.byRecording.end() ||
               recBlocks->second.size() != commit.blockCount) {
      structurallyValid = false;
    } else {
      for (uint32_t i = 0; i < commit.blockCount; ++i) {
        auto bi = recBlocks->second.find(i);
        if (bi == recBlocks->second.end() || bi->second.size() != 1) {
          structurallyValid = false;
          break;
        }
        entry.physicalBlocks.push_back(bi->second.front());
      }
      if (!entry.physicalBlocks.empty() && commit.firstCluster != entry.physicalBlocks.front())
        structurallyValid = false;
    }
    std::vector<uint8_t> payload;
    if (!structurallyValid) {
      result.issues.push_back("recording structure conflicts with COMMIT");
      result.isolated.push_back(item.first);
    }
    if (!structurallyValid) {
      result.tierB.push_back(item.first);
    } else if (!verifyBodies) {
      result.committedUnverified.push_back(item.first);
    } else {
      const bool bodyValid = readPayload(medium_, entry.physicalBlocks, entry.byteLen, &payload) &&
          crc32(payload.data(), payload.size()) == entry.bodyCrc32;
      if (bodyValid) result.tierA.push_back(entry);
      else {
        result.tierB.push_back(item.first);
        result.bodyMismatch.push_back(item.first);
      }
    }
  }
  for (const auto& item : deletes)
    if (commits.find(item.first) == commits.end()) result.deleted.push_back(item.first);
  for (const auto& item : blocks.byRecording) {
    if (commits.find(item.first) == commits.end() &&
        deletes.find(item.first) == deletes.end() &&
        std::find(result.tierB.begin(), result.tierB.end(), item.first) == result.tierB.end())
      {
        result.tierB.push_back(item.first);  // valid data header but no COMMIT
        result.incomplete.push_back(item.first);
      }
  }
  std::sort(result.tierB.begin(), result.tierB.end());
  std::sort(result.deleted.begin(), result.deleted.end());
  std::sort(result.incomplete.begin(), result.incomplete.end());
  std::sort(result.bodyMismatch.begin(), result.bodyMismatch.end());
  return result;
}

bool StorageModel::compact(CompactFault fault) {
  const ScanResult before = scan();
  if (!before.safe || before.activeBank < 0) return false;
  const unsigned oldBank = static_cast<unsigned>(before.activeBank);
  const unsigned newBank = 1U - oldBank;

  // Build carry set directly from the active bank. Live COMMIT records stay;
  // DELETE stays until no physical block with that recId remains.
  const BankScan active = scanBank(medium_, oldBank);
  const BlockScan blocks = scanBlocks(medium_);
  std::map<uint32_t, IndexRecord> commits;
  std::map<uint32_t, IndexRecord> deletes;
  for (const auto& r : active.records) {
    if (r.type == RecordType::Commit) commits[r.recId] = r;
    if (r.type == RecordType::Delete) deletes[r.recId] = r;
  }
  std::vector<IndexRecord> carry;
  for (const auto& item : commits) {
    auto d = deletes.find(item.first);
    if (d == deletes.end() || d->second.seq < item.second.seq) carry.push_back(item.second);
  }
  for (const auto& item : deletes)
    if (blocks.byRecording.find(item.first) != blocks.byRecording.end()) carry.push_back(item.second);
  std::sort(carry.begin(), carry.end(),
            [](const IndexRecord& x, const IndexRecord& y) { return x.seq < y.seq; });
  if (carry.size() > 126) return false;

  for (uint32_t off = 0; off < kIndexBankBytes; off += kEraseBytes)
    if (!erase(bankAddress(newBank) + off) ||
        !medium_.isAllFF(bankAddress(newBank) + off, kEraseBytes)) return false;

  const uint32_t readySeq = before.nextSeq;
  const uint32_t nextSeq = plusOneSaturated(readySeq);
  BankHeader header;
  header.bankId = static_cast<uint16_t>(newBank);
  header.generation = before.generation + 1;
  header.firstSeq = carry.empty() ? readySeq : carry.front().seq;
  header.nextRecIdHW = before.nextRecId;
  header.nextSeqHW = nextSeq;
  auto page = encodeBankHeaderPage(header);
  if (!program(bankAddress(newBank), page.data(),
               fault == CompactFault::TornHeader ? 713 : kPageBytes * 8U)) return false;

  uint32_t pageNumber = 1;
  for (const auto& r : carry) {
    page = encodeIndexRecordPage(r);
    const bool tear = fault == CompactFault::TornCarry && pageNumber == 1;
    if (!program(bankAddress(newBank) + pageNumber * kPageBytes, page.data(),
                 tear ? 811 : kPageBytes * 8U)) return false;
    ++pageNumber;
  }
  IndexRecord ready;
  ready.type = RecordType::Ready;
  ready.seq = readySeq;
  ready.nextRecIdHW = before.nextRecId;
  ready.nextSeqHW = nextSeq;
  page = encodeIndexRecordPage(ready);
  if (!program(bankAddress(newBank) + pageNumber * kPageBytes, page.data(),
               fault == CompactFault::TornReady ? 997 : kPageBytes * 8U)) return false;
  indexCursorValid_ = true;
  indexMutationFaulted_ = false;
  indexCursorBank_ = static_cast<int>(newBank);
  indexWritePage_ = static_cast<uint16_t>(pageNumber + 1U);
  if (fault == CompactFault::StopAfterReady) return true;

  for (uint32_t off = 0; off < kIndexBankBytes; off += kEraseBytes) {
    const bool tear = fault == CompactFault::TornOldErase && off == 0;
    if (!erase(bankAddress(oldBank) + off, tear ? 1733 : kEraseBytes * 8U)) return false;
  }
  return true;
}

bool VirtualFat::build(const NorMedium& medium, const std::vector<CatalogEntry>& entries) {
  medium_ = &medium;
  files_.clear();
  boot_.fill(0);
  fat_.fill(0);
  root_.fill(0);
  status_.clear();
  statusFirstCluster_ = 0;
  statusClusterCount_ = 0;

  boot_[0] = 0xEB; boot_[1] = 0x3C; boot_[2] = 0x90;
  std::memcpy(boot_.data() + 3, "PBLRING1", 8);
  put16(boot_.data() + 0x0B, 512);
  boot_[0x0D] = 8;
  put16(boot_.data() + 0x0E, 1);
  boot_[0x10] = 1;
  put16(boot_.data() + 0x11, 512);
  put16(boot_.data() + 0x13, kVirtualSectors);
  boot_[0x15] = 0xF8;
  put16(boot_.data() + 0x16, 2);
  put16(boot_.data() + 0x18, 32);
  put16(boot_.data() + 0x1A, 8);
  put32(boot_.data() + 0x1C, 0);
  put32(boot_.data() + 0x20, 0);
  boot_[0x24] = 0x80;
  boot_[0x26] = 0x29;
  put32(boot_.data() + 0x27, 0x20260829U);
  std::memcpy(boot_.data() + 0x2B, "PEBBLERING ", 11);
  std::memcpy(boot_.data() + 0x36, "FAT12   ", 8);
  boot_[510] = 0x55; boot_[511] = 0xAA;

  setFat12(&fat_, 0, 0xFF8);
  setFat12(&fat_, 1, 0xFFF);
  std::memcpy(root_.data(), "PEBBLERING ", 11);
  root_[11] = 0x08;

  uint16_t nextCluster = 2;
  size_t rootEntry = 1;
  std::vector<CatalogEntry> sorted = entries;
  std::sort(sorted.begin(), sorted.end(),
            [](const CatalogEntry& a, const CatalogEntry& b) { return a.recId < b.recId; });
  for (const CatalogEntry& entry : sorted) {
    const uint32_t neededBlocks = (entry.byteLen + kPayloadBytes - 1) / kPayloadBytes;
    if (entry.physicalBlocks.size() != neededBlocks || rootEntry >= 512) return false;
    File file;
    file.entry = entry;
    file.clusterCount = static_cast<uint16_t>((entry.byteLen + 4095U) / 4096U);
    file.firstCluster = file.clusterCount ? nextCluster : 0;
    if (nextCluster + file.clusterCount > 498) return false;
    for (uint16_t i = 0; i < file.clusterCount; ++i)
      setFat12(&fat_, nextCluster + i,
               i + 1 == file.clusterCount ? 0xFFF : static_cast<uint16_t>(nextCluster + i + 1));

    uint8_t* dir = root_.data() + rootEntry * 32;
    char name[9];
    std::snprintf(name, sizeof(name), "%08X", entry.recId);
    std::memcpy(dir, name, 8);
    std::memcpy(dir + 8, "ADP", 3);
    dir[11] = 0x01;
    put16(dir + 22, fatTime(entry.time));
    put16(dir + 24, fatDate(entry.time));
    put16(dir + 26, file.firstCluster);
    put32(dir + 28, entry.byteLen);
    ++rootEntry;
    nextCluster = static_cast<uint16_t>(nextCluster + file.clusterCount);
    files_.push_back(file);
  }
  return true;
}

bool VirtualFat::build(const NorMedium& medium, const ScanResult& snapshot) {
  if (!build(medium, snapshot.tierA)) return false;
  const bool capacityImminent =
      static_cast<uint64_t>(snapshot.freeDataBlocks) * 10U <= kDataBlocks ||
      static_cast<uint64_t>(snapshot.freeIndexPages) * 10U <= 127U;
  const char* pressure = snapshot.capacityPressure == CapacityPressure::Data ? "DATA" :
      snapshot.capacityPressure == CapacityPressure::Index ? "INDEX" : "NONE";
  std::ostringstream text;
  text << "PEBBLERING STORAGE STATUS\r\n"
       << "FORMAT_VERSION=1\r\n"
       << "MEDIA_GEN=" << snapshot.mediaGeneration << "\r\n"
       << "DEVICE_SAFE=" << (snapshot.deviceSafe ? "YES" : "NO") << "\r\n"
       << "TIER_A=" << snapshot.tierA.size() << "\r\n"
       << "TIER_B=" << snapshot.tierB.size() << "\r\n"
       << "INCOMPLETE=" << snapshot.incomplete.size() << "\r\n"
       << "BODY_MISMATCH=" << snapshot.bodyMismatch.size() << "\r\n"
       << "COMMITTED_UNVERIFIED=" << snapshot.committedUnverified.size() << "\r\n"
       << "RECORD_ISOLATED=" << snapshot.isolated.size() << "\r\n"
       << "BLOCK_QUARANTINED=" << snapshot.quarantinedBlocks << "\r\n"
       << "FREE_DATA_BLOCKS=" << snapshot.freeDataBlocks << "/" << kDataBlocks << "\r\n"
       << "FREE_INDEX_PAGES=" << snapshot.freeIndexPages << "/127\r\n"
       << "CAPACITY_PRESSURE=" << pressure << "\r\n"
       << "CAPACITY_IMMINENT=" << (capacityImminent ? "YES" : "NO") << "\r\n"
       << "RAW_BACKUP_RECOMMENDED="
       << ((!snapshot.deviceSafe || !snapshot.tierB.empty() || snapshot.quarantinedBlocks ||
            !snapshot.bodyMismatch.empty() || capacityImminent) ? "YES" : "NO") << "\r\n";
  status_ = text.str();
  uint16_t nextCluster = 2;
  for (const File& file : files_)
    nextCluster = std::max<uint16_t>(nextCluster,
        static_cast<uint16_t>(file.firstCluster + file.clusterCount));
  statusClusterCount_ = static_cast<uint16_t>((status_.size() + 4095U) / 4096U);
  statusFirstCluster_ = statusClusterCount_ ? nextCluster : 0;
  if (nextCluster + statusClusterCount_ > 498 || files_.size() + 1 >= 512) return false;
  for (uint16_t i = 0; i < statusClusterCount_; ++i)
    setFat12(&fat_, nextCluster + i,
             i + 1 == statusClusterCount_ ? 0xFFF : static_cast<uint16_t>(nextCluster + i + 1));
  uint8_t* dir = root_.data() + (files_.size() + 1U) * 32U;
  std::memcpy(dir, "STATUS  TXT", 11);
  dir[11] = 0x01;
  put16(dir + 26, statusFirstCluster_);
  put32(dir + 28, static_cast<uint32_t>(status_.size()));
  return true;
}

bool VirtualFat::readFileByte(const File& file, uint32_t offset, uint8_t* value) const {
  if (!medium_ || !value || offset >= file.entry.byteLen) return false;
  const uint32_t blockIndex = offset / kPayloadBytes;
  const uint32_t inBlock = offset % kPayloadBytes;
  if (blockIndex >= file.entry.physicalBlocks.size()) return false;
  return medium_->read(file.entry.physicalBlocks[blockIndex] * kEraseBytes + 32 + inBlock,
                       value, 1);
}

bool VirtualFat::read(uint64_t offset, uint8_t* out, size_t size) const {
  if (!out || offset > this->size() || size > this->size() - offset) return false;
  for (size_t i = 0; i < size; ++i) {
    const uint64_t at = offset + i;
    if (at < 512) out[i] = boot_[at];
    else if (at < 3ULL * 512) out[i] = fat_[at - 512];
    else if (at < 35ULL * 512) out[i] = root_[at - 3ULL * 512];
    else {
      const uint64_t dataOffset = at - 35ULL * 512;
      const uint32_t logicalCluster = static_cast<uint32_t>(dataOffset / 4096);
      const uint32_t inCluster = static_cast<uint32_t>(dataOffset % 4096);
      out[i] = 0;
      if (statusClusterCount_ && logicalCluster + 2 >= statusFirstCluster_ &&
          logicalCluster + 2 < statusFirstCluster_ + statusClusterCount_) {
        const uint32_t statusOffset =
            (logicalCluster + 2 - statusFirstCluster_) * 4096U + inCluster;
        if (statusOffset < status_.size()) out[i] = static_cast<uint8_t>(status_[statusOffset]);
        continue;
      }
      for (const File& file : files_) {
        if (!file.clusterCount || logicalCluster + 2 < file.firstCluster ||
            logicalCluster + 2 >= file.firstCluster + file.clusterCount) continue;
        const uint32_t fileOffset = (logicalCluster + 2 - file.firstCluster) * 4096U + inCluster;
        if (fileOffset < file.entry.byteLen && !readFileByte(file, fileOffset, &out[i])) return false;
        break;
      }
    }
  }
  return true;
}

std::vector<uint8_t> VirtualFat::image() const {
  std::vector<uint8_t> out(size());
  if (!read(0, out.data(), out.size())) out.clear();
  return out;
}

bool ExportGate::requestAudio(bool operationsStopped, bool ioComplete, bool noError,
                              uint64_t before, uint64_t after, bool catalogValid) {
  if (state_ != ExportState::IdleLocal) return false;
  state_ = ExportState::PendingAudio;
  if (!operationsStopped || !ioComplete || !noError || before != after || !catalogValid) {
    state_ = ExportState::Fault;
    return false;
  }
  state_ = ExportState::ExportAudio;
  return true;
}

bool ExportGate::requestRaw(bool operationsStopped, bool readPathWorks) {
  if (state_ != ExportState::IdleLocal && state_ != ExportState::Safe &&
      state_ != ExportState::Fault) return false;
  state_ = ExportState::PendingRaw;
  if (!operationsStopped || !readPathWorks) {
    state_ = ExportState::Fault;
    return false;
  }
  state_ = ExportState::ExportRaw;
  return true;
}

void ExportGate::release() {
  if (state_ == ExportState::ExportAudio || state_ == ExportState::ExportRaw) {
    state_ = ExportState::Releasing;
    state_ = ExportState::IdleLocal;
  }
}

bool LowBatteryStop::begin(bool commitPageReserved) {
  if (state_ != StopState::Recording) return false;
  if (!commitPageReserved) {
    state_ = StopState::SafeFault;
    return false;
  }
  state_ = StopState::CommitReserved;
  return true;
}

bool LowBatteryStop::finishPartialDataPage(bool ok) {
  if (state_ != StopState::CommitReserved || pagePrograms_ != 0) return false;
  ++pagePrograms_;
  if (!ok) state_ = StopState::SafeFault;
  return ok;
}

bool LowBatteryStop::programCommit(bool ok) {
  if (state_ != StopState::CommitReserved || pagePrograms_ > 1) return false;
  ++pagePrograms_;
  state_ = ok ? StopState::StoppedUnverified : StopState::SafeFault;
  return ok;
}

bool PageReservation::canAppend() const {
  const uint32_t fence = bootFenceNeeded ? 1U : 0U;
  return freePages() > fence;  // an already-reserved COMMIT may consume its own page
}
bool PageReservation::canCreate() const {
  pebble_format::PageReservation r = {
      static_cast<uint16_t>(std::min<uint32_t>(used, UINT16_MAX)),
      static_cast<uint16_t>(std::min<uint32_t>(reservedCommits, UINT16_MAX)),
      static_cast<uint16_t>(std::min<uint32_t>(carry, UINT16_MAX)), bootFenceNeeded};
  return pebble_format::canCreateRecording(r);
}
bool PageReservation::canDelete() const {
  pebble_format::PageReservation r = {
      static_cast<uint16_t>(std::min<uint32_t>(used, UINT16_MAX)),
      static_cast<uint16_t>(std::min<uint32_t>(reservedCommits, UINT16_MAX)),
      static_cast<uint16_t>(std::min<uint32_t>(carry, UINT16_MAX)), bootFenceNeeded};
  return pebble_format::canDeleteRecording(r);
}
bool PageReservation::canCompact() const {
  pebble_format::PageReservation r = {0, 0,
      static_cast<uint16_t>(std::min<uint32_t>(carry, UINT16_MAX)), false};
  return pebble_format::canCompact(r);
}

}  // namespace storage_v2
