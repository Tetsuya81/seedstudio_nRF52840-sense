#pragma once
#include <stdint.h>

// Incident 2026-08-28: source-only quarantine; NOT deployed to the board.
// Remove only after the hardware restart checklist has been reviewed.
namespace SafetyPolicy {
static constexpr bool kHardwareQuarantine = true;

inline bool localOnlyCommand(char c) {
  switch (c) {
    case 'h': case '?': case 's': case 'v':
    case 'p': case 'l': case 'd': case '[': case ']':
    case 'i': case 'x': case 'g': return true;
    default: return false;
  }
}

inline bool sectorRange(uint32_t sector, uint32_t count, uint32_t sectors) {
  return count != 0 && sector < sectors && count <= sectors - sector;
}

inline bool byteRange(uint32_t lba, uint32_t bytes, uint32_t capacity) {
  const uint64_t start = uint64_t(lba) * 512u;
  return bytes != 0 && start < capacity && bytes <= capacity - start;
}
}  // namespace SafetyPolicy
