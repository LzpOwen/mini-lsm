#include "mlsm/wal_writer.h"

#include "mlsm/coding.h"
#include "mlsm/crc32c.h"
#include "mlsm/writable_file.h"

namespace mlsm {
namespace log {

Writer::Writer(WritableFile* dest) : dest_(dest), block_offset_(0) {
  for (int i = 0; i <= kMaxRecordType; ++i) {
    char t = static_cast<char>(i);
    type_crc_[i] = crc32c::Value(&t, 1);
  }
}

Status Writer::AddRecord(const Slice& slice) {
  const char* ptr = slice.data();
  size_t left = slice.size();

  // Fragment the record across blocks if needed. Note the loop runs at least
  // once, so a zero-length record still emits one (Full) physical record.
  Status s;
  bool begin = true;
  do {
    const int leftover = kBlockSize - block_offset_;
    // Not enough room even for a header: pad the tail with zeros and switch
    // to a fresh block.
    if (leftover < kHeaderSize) {
      if (leftover > 0) {
        static_assert(kHeaderSize == 7, "trailer assumes a 6-byte max pad");
        const char kZeroPad[kHeaderSize] = {0, 0, 0, 0, 0, 0, 0};
        s = dest_->Append(Slice(kZeroPad, leftover));
        if (!s.ok()) return s;
      }
      block_offset_ = 0;
    }

    // Bytes available for payload in the current block after its header.
    const int avail = kBlockSize - block_offset_ - kHeaderSize;
    const size_t fragment_length =
        (left < static_cast<size_t>(avail)) ? left : static_cast<size_t>(avail);

    const bool end = (left == fragment_length);
    RecordType type;
    if (begin && end) {
      type = kFullType;
    } else if (begin) {
      type = kFirstType;
    } else if (end) {
      type = kLastType;
    } else {
      type = kMiddleType;
    }

    s = EmitPhysicalRecord(type, ptr, fragment_length);
    ptr += fragment_length;
    left -= fragment_length;
    begin = false;
  } while (s.ok() && left > 0);

  return s;
}

Status Writer::EmitPhysicalRecord(RecordType type, const char* ptr,
                                  size_t length) {
  // length fits in 2 bytes and header + payload fits in the current block:
  // both are guaranteed by AddRecord's fragmentation logic.
  char header[kHeaderSize];
  header[4] = static_cast<char>(length & 0xff);
  header[5] = static_cast<char>((length >> 8) & 0xff);
  header[6] = static_cast<char>(type);

  // Checksum covers the type byte followed by the payload, then gets masked.
  uint32_t crc = crc32c::Extend(type_crc_[type], ptr, length);
  EncodeFixed32(header, crc32c::Mask(crc));

  Status s = dest_->Append(Slice(header, kHeaderSize));
  if (s.ok()) {
    s = dest_->Append(Slice(ptr, length));
    if (s.ok()) {
      s = dest_->Flush();
    }
  }
  block_offset_ += kHeaderSize + static_cast<int>(length);
  return s;
}

}  // namespace log
}  // namespace mlsm
