#pragma once

#include <cstdint>

namespace mlsm {

// Little-endian fixed-width integer encode/decode. The WAL's fixed header
// fields (checksum, length) are stored little-endian regardless of host byte
// order, so both the Writer and Reader go through these helpers to stay in
// agreement and to keep the on-disk format portable across machines.

inline void EncodeFixed32(char* dst, uint32_t value) {
  uint8_t* p = reinterpret_cast<uint8_t*>(dst);
  p[0] = static_cast<uint8_t>(value & 0xff);
  p[1] = static_cast<uint8_t>((value >> 8) & 0xff);
  p[2] = static_cast<uint8_t>((value >> 16) & 0xff);
  p[3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

inline uint32_t DecodeFixed32(const char* ptr) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(ptr);
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace mlsm
