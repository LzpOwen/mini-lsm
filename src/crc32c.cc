#include "mlsm/crc32c.h"

#include <array>

namespace mlsm {
namespace crc32c {

namespace {

// Byte-wise lookup table for the reflected Castagnoli polynomial, built at
// compile time. table[b] is the crc contribution of a single byte b.
constexpr std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1) ? (0x82f63b78u ^ (c >> 1)) : (c >> 1);
    }
    table[i] = c;
  }
  return table;
}

constexpr std::array<uint32_t, 256> kTable = MakeTable();

}  // namespace

uint32_t Extend(uint32_t crc, const char* data, size_t n) {
  // Un-condition the incoming value, fold in each byte, re-condition on exit.
  // This pre/post XOR with all-ones is what makes Extend chainable.
  uint32_t c = crc ^ 0xffffffffu;
  for (size_t i = 0; i < n; ++i) {
    c = kTable[(c ^ static_cast<unsigned char>(data[i])) & 0xff] ^ (c >> 8);
  }
  return c ^ 0xffffffffu;
}

uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }

}  // namespace crc32c
}  // namespace mlsm
