#pragma once

#include <cstddef>
#include <cstdint>

#include "mlsm/slice.h"
#include "mlsm/status.h"

namespace mlsm {

class WritableFile;

namespace log {

// LevelDB/RocksDB-style write-ahead log format.
//
// The log is a sequence of 32KB blocks. Each block holds a run of physical
// records; a record never spans block boundaries by itself — a logical record
// that doesn't fit in the block's remainder is split into fragments tagged
// First/Middle/Last. A record that fits whole is tagged Full.
//
// Physical record layout:
//   checksum (4 bytes, little-endian)  -- Mask(crc32c(type_byte + payload))
//   length   (2 bytes, little-endian)  -- payload length
//   type     (1 byte)                  -- RecordType
//   payload  (length bytes)
//
// When fewer than kHeaderSize bytes remain in a block, the writer fills the
// tail with zero bytes (a trailer) and starts the next record in a new block.

static const int kBlockSize = 32768;

// Header = 4-byte checksum + 2-byte length + 1-byte type.
static const int kHeaderSize = 4 + 2 + 1;

enum RecordType {
  kZeroType = 0,  // Reserved; also the value of preallocated/zero-filled space.
  kFullType = 1,

  kFirstType = 2,
  kMiddleType = 3,
  kLastType = 4,
};

static const int kMaxRecordType = kLastType;

// Appends records to an underlying WritableFile using the format above.
// The writer does not own the WritableFile; the caller must keep it alive for
// the writer's lifetime.
class Writer {
 public:
  explicit Writer(WritableFile* dest);

  // Resumes writing to a WAL that already holds dest_length bytes. The block
  // cursor is seeded from the existing length so records stay block-aligned;
  // without this, resuming a non-empty log would let a record straddle a 32KB
  // boundary and corrupt the format.
  Writer(WritableFile* dest, uint64_t dest_length);

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  // Encodes slice as one or more physical records and appends them.
  Status AddRecord(const Slice& slice);

 private:
  Status EmitPhysicalRecord(RecordType type, const char* ptr, size_t length);

  WritableFile* dest_;
  int block_offset_;  // Bytes written into the current block.

  // Precomputed crc of each record type byte, so per-record crc only needs to
  // fold in the payload.
  uint32_t type_crc_[kMaxRecordType + 1];
};

}  // namespace log
}  // namespace mlsm
