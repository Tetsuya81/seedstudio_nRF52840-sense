#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

// Memory-only NOR model: no device handles, USB calls, or disk image mounting.
class Adafruit_SPIFlash {
public:
  std::vector<uint8_t> bytes = std::vector<uint8_t>(2097152, 0xff);
  bool failErase = false, failWrite = false, shortRead = false;
  bool corruptWrite = false, cutAfterErase = false;
  unsigned reads = 0, writes = 0, erases = 0;

  uint32_t size() const { return uint32_t(bytes.size()); }
  uint32_t readBuffer(uint32_t addr, uint8_t* out, uint32_t len) {
    reads++;
    if (uint64_t(addr) + len > size()) return 0;
    uint32_t n = shortRead ? len / 2 : len;
    shortRead = false;
    std::memcpy(out, bytes.data() + addr, n);
    return n;
  }
  bool eraseSector(uint32_t sector) {
    erases++;
    if (failErase) { failErase = false; return false; }
    uint64_t addr = uint64_t(sector) * 4096;
    if (addr + 4096 > size()) return false;
    std::fill_n(bytes.begin() + addr, 4096, 0xff);
    if (cutAfterErase) { cutAfterErase = false; return false; }
    return true;
  }
  uint32_t writeBuffer(uint32_t addr, const uint8_t* in, uint32_t len) {
    writes++;
    if (failWrite) { failWrite = false; return 0; }
    if (uint64_t(addr) + len > size()) return 0;
    for (uint32_t i = 0; i < len; i++) {
      if ((bytes[addr + i] & in[i]) != in[i]) return i;
      bytes[addr + i] &= in[i];
    }
    if (corruptWrite) { corruptWrite = false; bytes[addr] ^= 1; }
    return len;
  }
};
