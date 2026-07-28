#pragma once

#include <cstddef>
#include <cstdint>

namespace mlsm {
namespace crc32c {

// CRC32C = CRC-32 with the Castagnoli polynomial (reflected form 0x82F63B78).
// Same variant used by LevelDB/RocksDB WAL records. Software table-based impl.

// Returns the crc32c of data[0, n).
uint32_t Value(const char* data, size_t n);

// Continues a crc: Extend(Value(a, alen), b, blen) == crc of the concatenation.
// Value(data, n) is exactly Extend(0, data, n).
uint32_t Extend(uint32_t crc, const char* data, size_t n);

// WAL stores a *masked* crc so that a crc computed over a buffer that itself
// contains a crc doesn't collide in a way that hides corruption. Mask rotates
// and offsets the value; Unmask reverses it. (Same trick as LevelDB.)
static const uint32_t kMaskDelta = 0xa282ead8ul;

inline uint32_t Mask(uint32_t crc) {
  // Rotate right by 15 bits, then add a constant.
  return ((crc >> 15) | (crc << 17)) + kMaskDelta;
}

inline uint32_t Unmask(uint32_t masked_crc) {
  uint32_t rot = masked_crc - kMaskDelta;
  return ((rot >> 17) | (rot << 15));
}

}  // namespace crc32c
}  // namespace mlsm
