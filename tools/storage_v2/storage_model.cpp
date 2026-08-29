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
void put64(uint8_t* p, uint64_t v) {
  for (unsigned i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint16_t get16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}
uint32_t get32(const uint8_t* p) {
  uint32_t v = 0;
  for (unsigned i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
  return v;
}
uint64_t get64(const uint8_t* p) {
  uint64_t v = 0;
  for (unsigned i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}
bool magic(const uint8_t* p, const char text[5]) {
  return std::memcmp(p, text, 4) == 0;
}
uint32_t bankAddress(unsigned bank) {
  return kIndexBase + bank * kIndexBankBytes;
}
bool allFF(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) if (p[i] != 0xFF) return false;
  return true;
}
uint32_t plusOneSaturated(uint32_t value) {
  return value >= 0xFFFFFFFEU ? 0xFFFFFFFEU : value + 1U;
}

struct BankScan {
  bool headerValid = false;
  bool ready = false;
  bool safe = true;
  BankHeader header;
  std::vector<IndexRecord> records;
  std::vector<std::string> issues;
};

BankScan scanBank(const NorMedium& medium, unsigned bank) {
  BankScan result;
  std::array<uint8_t, kPageBytes> page{};
  medium.read(bankAddress(bank), page.data(), page.size());
  result.headerValid = decodeBankHeader(page.data(), bank, &result.header);
  if (!result.headerValid) return result;

  bool sawBlank = false;
  std::map<uint32_t, std::array<uint8_t, kPageBytes>> seqPages;
  for (uint32_t p = 1; p < kPagesPerBank; ++p) {
    medium.read(bankAddress(bank) + p * kPageBytes, page.data(), page.size());
    if (allFF(page.data(), page.size())) {
      sawBlank = true;
      continue;
    }
    IndexRecord record;
    if (!decodeIndexRecord(page.data(), &record)) continue;  // occupied torn page
    if (sawBlank) {
      result.safe = false;
      result.issues.push_back("valid record after blank index page");
    }
    auto found = seqPages.find(record.seq);
    if (found != seqPages.end() && found->second != page) {
      result.safe = false;
      result.issues.push_back("same seq has different records");
    } else {
      seqPages[record.seq] = page;
    }
    if (record.type == RecordType::Ready) result.ready = true;
    result.records.push_back(record);
  }
  std::sort(result.records.begin(), result.records.end(),
            [](const IndexRecord& a, const IndexRecord& b) { return a.seq < b.seq; });
  return result;
}

struct BlockScan {
  std::map<uint32_t, std::map<uint32_t, std::vector<uint32_t>>> byRecording;
  size_t quarantined = 0;
};

BlockScan scanBlocks(const NorMedium& medium) {
  BlockScan result;
  std::array<uint8_t, 32> headerBytes{};
  for (uint32_t block = 0; block < kDataBlocks; ++block) {
    const uint32_t address = block * kEraseBytes;
    if (medium.isAllFF(address, kEraseBytes)) continue;
    medium.read(address, headerBytes.data(), headerBytes.size());
    DataHeader header;
    if (!decodeDataHeader(headerBytes.data(), &header)) {
      ++result.quarantined;
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
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
  }
  return crc ^ 0xFFFFFFFFU;
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
         allFF(bytes_.data() + address, size);
}

bool NorMedium::programPage(uint32_t address, const uint8_t page[kPageBytes], size_t cutBits) {
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

std::array<uint8_t, 32> encodeDataHeader(const DataHeader& h) {
  std::array<uint8_t, 32> out;
  out.fill(0xFF);
  std::memcpy(out.data(), "PRB1", 4);
  put16(out.data() + 4, h.version);
  put16(out.data() + 6, h.flags);
  put32(out.data() + 8, h.recId);
  put32(out.data() + 12, h.blockIndex);
  put64(out.data() + 16, h.startTime);
  put32(out.data() + 24, 0xFFFFFFFFU);
  put32(out.data() + 28, crc32(out.data(), 28));
  return out;
}

bool decodeDataHeader(const uint8_t* p, DataHeader* h) {
  if (!p || !h || !magic(p, "PRB1") || get16(p + 4) != 1 || get16(p + 6) != 0 ||
      get32(p + 24) != 0xFFFFFFFFU || get32(p + 28) != crc32(p, 28)) return false;
  h->version = get16(p + 4);
  h->flags = get16(p + 6);
  h->recId = get32(p + 8);
  h->blockIndex = get32(p + 12);
  h->startTime = get64(p + 16);
  return true;
}

std::array<uint8_t, kPageBytes> encodeBankHeader(const BankHeader& h) {
  std::array<uint8_t, kPageBytes> out;
  out.fill(0xFF);
  std::memcpy(out.data(), "PRBH", 4);
  put16(out.data() + 4, h.version);
  put16(out.data() + 6, h.bankId);
  put32(out.data() + 8, h.generation);
  put32(out.data() + 12, h.firstSeq);
  put32(out.data() + 16, h.nextRecIdHW);
  put32(out.data() + 20, h.nextSeqHW);
  put64(out.data() + 24, h.createdTime);
  put32(out.data() + 252, crc32(out.data(), 252));
  return out;
}

bool decodeBankHeader(const uint8_t* p, uint16_t physicalBank, BankHeader* h) {
  if (!p || !h || !magic(p, "PRBH") || get16(p + 4) != 1 ||
      get16(p + 6) != physicalBank || !allFF(p + 32, 220) ||
      get32(p + 252) != crc32(p, 252)) return false;
  h->version = get16(p + 4);
  h->bankId = get16(p + 6);
  h->generation = get32(p + 8);
  h->firstSeq = get32(p + 12);
  h->nextRecIdHW = get32(p + 16);
  h->nextSeqHW = get32(p + 20);
  h->createdTime = get64(p + 24);
  return true;
}

std::array<uint8_t, kPageBytes> encodeIndexRecord(const IndexRecord& r) {
  std::array<uint8_t, kPageBytes> out;
  out.fill(0xFF);
  std::memcpy(out.data(), "PRR1", 4);
  out[4] = static_cast<uint8_t>(r.type);
  out[5] = r.version;
  put32(out.data() + 6, r.seq);
  put32(out.data() + 10, r.recId);
  put32(out.data() + 14, r.byteLen);
  put32(out.data() + 18, r.bodyCrc32);
  put32(out.data() + 22, r.blockCount);
  put32(out.data() + 26, r.firstCluster);
  put64(out.data() + 30, r.time);
  put32(out.data() + 38, r.nextRecIdHW);
  put32(out.data() + 42, r.nextSeqHW);
  put32(out.data() + 252, crc32(out.data(), 252));
  return out;
}

bool decodeIndexRecord(const uint8_t* p, IndexRecord* r) {
  if (!p || !r || !magic(p, "PRR1") || p[5] != 1 ||
      p[4] < 1 || p[4] > 3 || !allFF(p + 46, 206) ||
      get32(p + 252) != crc32(p, 252)) return false;
  r->type = static_cast<RecordType>(p[4]);
  r->version = p[5];
  r->seq = get32(p + 6);
  r->recId = get32(p + 10);
  r->byteLen = get32(p + 14);
  r->bodyCrc32 = get32(p + 18);
  r->blockCount = get32(p + 22);
  r->firstCluster = get32(p + 26);
  r->time = get64(p + 30);
  r->nextRecIdHW = get32(p + 38);
  r->nextSeqHW = get32(p + 42);
  if (r->type == RecordType::Ready) {
    if (r->recId != UINT32_MAX || r->byteLen != UINT32_MAX ||
        r->bodyCrc32 != UINT32_MAX || r->blockCount != UINT32_MAX ||
        r->firstCluster != UINT32_MAX || r->nextRecIdHW == UINT32_MAX ||
        r->nextSeqHW == UINT32_MAX) return false;
  } else if (r->recId == UINT32_MAX || r->nextRecIdHW != UINT32_MAX ||
             r->nextSeqHW != UINT32_MAX) {
    return false;
  } else if (r->type == RecordType::Delete &&
             (r->byteLen != UINT32_MAX || r->bodyCrc32 != UINT32_MAX ||
              r->blockCount != UINT32_MAX || r->firstCluster != UINT32_MAX)) {
    return false;
  }
  return true;
}

StorageModel::StorageModel() = default;

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
  BankHeader header;
  header.bankId = 0;
  header.generation = 1;
  header.firstSeq = nextSeq;
  header.nextRecIdHW = nextRecId;
  header.nextSeqHW = plusOneSaturated(nextSeq);
  const auto headerPage = encodeBankHeader(header);
  if (!program(bankAddress(0), headerPage.data(), kPageBytes * 8U)) return false;

  IndexRecord ready;
  ready.type = RecordType::Ready;
  ready.seq = nextSeq;
  ready.nextRecIdHW = nextRecId;
  ready.nextSeqHW = plusOneSaturated(nextSeq);
  const auto readyPage = encodeIndexRecord(ready);
  return program(bankAddress(0) + kPageBytes, readyPage.data(), kPageBytes * 8U);
}

bool StorageModel::writeRecording(uint32_t recId, const std::vector<uint8_t>& audio,
                                  const std::vector<uint32_t>& placement,
                                  size_t tornBlock, size_t tornPage, size_t tornBits) {
  const size_t needed = (audio.size() + kPayloadBytes - 1) / kPayloadBytes;
  if (needed != placement.size()) return false;
  size_t source = 0;
  for (size_t i = 0; i < placement.size(); ++i) {
    if (placement[i] >= kDataBlocks || !medium_.isAllFF(placement[i] * kEraseBytes, kEraseBytes))
      return false;
    std::array<uint8_t, kEraseBytes> block;
    block.fill(0xFF);
    DataHeader header;
    header.recId = recId;
    header.blockIndex = static_cast<uint32_t>(i);
    header.startTime = i == 0 ? 1 : UINT64_MAX;
    const auto encoded = encodeDataHeader(header);
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
  if (current.activeBank < 0) return false;
  const uint32_t base = bankAddress(static_cast<unsigned>(current.activeBank));
  for (uint32_t page = 1; page < kPagesPerBank; ++page) {
    const uint32_t address = base + page * kPageBytes;
    if (medium_.isAllFF(address, kPageBytes) &&
        !medium_.pageConsumed(address / kPageBytes)) {
      const auto encoded = encodeIndexRecord(record);
      return program(address, encoded.data(), bits);
    }
  }
  return false;
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

ScanResult StorageModel::scan() const {
  ScanResult result;
  const BankScan a = scanBank(medium_, 0);
  const BankScan b = scanBank(medium_, 1);
  const BankScan* bank = nullptr;
  if (a.headerValid && a.ready && b.headerValid && b.ready) {
    if (a.header.generation == b.header.generation) {
      result.safe = false;
      result.issues.push_back("both banks have the same generation");
      return result;
    }
    bank = a.header.generation > b.header.generation ? &a : &b;
    result.activeBank = bank == &a ? 0 : 1;
  } else if (a.headerValid && a.ready) {
    bank = &a;
    result.activeBank = 0;
  } else if (b.headerValid && b.ready) {
    bank = &b;
    result.activeBank = 1;
  } else {
    result.safe = false;
    result.issues.push_back("no ready index bank");
    return result;
  }
  result.safe = bank->safe;
  result.issues.insert(result.issues.end(), bank->issues.begin(), bank->issues.end());
  result.generation = bank->header.generation;
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
  if (blocks.quarantined) {
    result.safe = false;
    result.issues.push_back("non-blank data block has no valid PRB1 header");
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
    const bool bodyValid = structurallyValid &&
        readPayload(medium_, entry.physicalBlocks, entry.byteLen, &payload) &&
        crc32(payload.data(), payload.size()) == entry.bodyCrc32;
    if (!structurallyValid) {
      result.safe = false;
      result.issues.push_back("recording structure conflicts with COMMIT");
    }
    if (bodyValid) result.tierA.push_back(entry);
    else result.tierB.push_back(item.first);
  }
  for (const auto& item : deletes)
    if (commits.find(item.first) == commits.end()) result.deleted.push_back(item.first);
  for (const auto& item : blocks.byRecording) {
    if (commits.find(item.first) == commits.end() &&
        deletes.find(item.first) == deletes.end() &&
        std::find(result.tierB.begin(), result.tierB.end(), item.first) == result.tierB.end())
      result.tierB.push_back(item.first);  // valid data header but no COMMIT
  }
  std::sort(result.tierB.begin(), result.tierB.end());
  std::sort(result.deleted.begin(), result.deleted.end());
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

  if (!medium_.isAllFF(bankAddress(newBank), kIndexBankBytes)) {
    for (uint32_t off = 0; off < kIndexBankBytes; off += kEraseBytes)
      if (!erase(bankAddress(newBank) + off)) return false;
  }

  const uint32_t readySeq = before.nextSeq;
  const uint32_t nextSeq = plusOneSaturated(readySeq);
  BankHeader header;
  header.bankId = static_cast<uint16_t>(newBank);
  header.generation = before.generation + 1;
  header.firstSeq = carry.empty() ? readySeq : carry.front().seq;
  header.nextRecIdHW = before.nextRecId;
  header.nextSeqHW = nextSeq;
  auto page = encodeBankHeader(header);
  if (!program(bankAddress(newBank), page.data(),
               fault == CompactFault::TornHeader ? 713 : kPageBytes * 8U)) return false;

  uint32_t pageNumber = 1;
  for (const auto& r : carry) {
    page = encodeIndexRecord(r);
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
  page = encodeIndexRecord(ready);
  if (!program(bankAddress(newBank) + pageNumber * kPageBytes, page.data(),
               fault == CompactFault::TornReady ? 997 : kPageBytes * 8U)) return false;
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

bool PageReservation::canAppend() const { return freePages() >= 1; }
bool PageReservation::canCreate() const {
  return freePages() > reservedCommits &&
         static_cast<uint64_t>(carry) + reservedCommits + 1U <= 126U;
}
bool PageReservation::canDelete() const { return freePages() > reservedCommits; }
bool PageReservation::canCompact() const { return carry <= 126; }

}  // namespace storage_v2
