#include "mlsm/wal_reader.h"

#include <cstdint>

#include "mlsm/coding.h"
#include "mlsm/crc32c.h"
#include "mlsm/sequential_file.h"

namespace mlsm {
namespace log {

Reader::Reader(SequentialFile* file, Reporter* reporter)
    : file_(file), reporter_(reporter), eof_(false) {}

void Reader::ReportDrop(size_t bytes, const Status& reason) {
  if (reporter_ != nullptr) {
    reporter_->Corruption(bytes, reason);
  }
}

unsigned int Reader::ReadPhysicalRecord(Slice* result) {
  while (true) {
    // Fewer than a header's worth of bytes left in the block: it's either the
    // zero-padded trailer or an exhausted buffer. Drop it and pull the next
    // block. If we're already at EOF, there are no more records.
    if (buffer_.size() < static_cast<size_t>(kHeaderSize)) {
      if (eof_) {
        buffer_ = Slice();
        return kEof;
      }
      backing_store_.resize(kBlockSize);
      Slice read_result;
      Status s = file_->Read(kBlockSize, &read_result, &backing_store_[0]);
      buffer_ = read_result;
      if (!s.ok()) {
        buffer_ = Slice();
        eof_ = true;
        ReportDrop(kBlockSize, s);
        return kEof;
      }
      // A short block means we've hit the end of the file.
      if (read_result.size() < static_cast<size_t>(kBlockSize)) {
        eof_ = true;
      }
      continue;
    }

    // Parse the header: checksum (4) | length (2) | type (1).
    const char* header = buffer_.data();
    const uint32_t masked_crc = DecodeFixed32(header);
    const uint32_t length = static_cast<uint8_t>(header[4]) |
                            (static_cast<uint32_t>(static_cast<uint8_t>(
                                 header[5]))
                             << 8);
    const unsigned int type = static_cast<uint8_t>(header[6]);

    // The payload claims to run past what's left in this block. A well-formed
    // log never does that (records don't straddle blocks), so it's corruption
    // mid-stream or a torn tail after a crash.
    if (kHeaderSize + length > buffer_.size()) {
      const size_t drop_size = buffer_.size();
      buffer_ = Slice();
      if (!eof_) {
        ReportDrop(drop_size, Status::Corruption("bad record length"));
        return kBadRecord;
      }
      // At EOF this is an incomplete record from an interrupted write: report
      // nothing and treat it as a clean end.
      return kEof;
    }

    // A zero-type, zero-length record is preallocated/padding space; skip it.
    if (type == kZeroType && length == 0) {
      buffer_ = Slice(header + kHeaderSize, buffer_.size() - kHeaderSize);
      return kBadRecord;
    }

    // Verify the crc over the type byte + payload against the unmasked header.
    const uint32_t expected_crc = crc32c::Unmask(masked_crc);
    const uint32_t actual_crc = crc32c::Value(header + 6, 1 + length);
    if (actual_crc != expected_crc) {
      const size_t drop_size = buffer_.size();
      buffer_ = Slice();
      ReportDrop(drop_size, Status::Corruption("checksum mismatch"));
      return kBadRecord;
    }

    // Consume the record and hand back its payload.
    buffer_ = Slice(header + kHeaderSize + length,
                    buffer_.size() - kHeaderSize - length);
    *result = Slice(header + kHeaderSize, length);
    return type;
  }
}

bool Reader::ReadRecord(Slice* record, std::string* scratch) {
  scratch->clear();
  *record = Slice();
  bool in_fragmented_record = false;

  Slice fragment;
  while (true) {
    const unsigned int record_type = ReadPhysicalRecord(&fragment);
    switch (record_type) {
      case kFullType:
        if (in_fragmented_record) {
          ReportDrop(scratch->size(),
                     Status::Corruption("partial record without end(1)"));
        }
        *record = fragment;
        scratch->clear();
        return true;

      case kFirstType:
        if (in_fragmented_record) {
          ReportDrop(scratch->size(),
                     Status::Corruption("partial record without end(2)"));
        }
        scratch->assign(fragment.data(), fragment.size());
        in_fragmented_record = true;
        break;

      case kMiddleType:
        if (!in_fragmented_record) {
          ReportDrop(fragment.size(),
                     Status::Corruption("missing start of fragmented record(1)"));
        } else {
          scratch->append(fragment.data(), fragment.size());
        }
        break;

      case kLastType:
        if (!in_fragmented_record) {
          ReportDrop(fragment.size(),
                     Status::Corruption("missing start of fragmented record(2)"));
        } else {
          scratch->append(fragment.data(), fragment.size());
          *record = Slice(*scratch);
          return true;
        }
        break;

      case kEof:
        // A half-written fragment at the tail is a torn record from a crash;
        // drop it silently (already accounted for) and report end of input.
        if (in_fragmented_record) {
          scratch->clear();
        }
        return false;

      case kBadRecord:
        if (in_fragmented_record) {
          ReportDrop(scratch->size(),
                     Status::Corruption("error in middle of record"));
          in_fragmented_record = false;
          scratch->clear();
        }
        break;

      default:
        ReportDrop((fragment.size() + (in_fragmented_record ? scratch->size() : 0)),
                   Status::Corruption("unknown record type"));
        in_fragmented_record = false;
        scratch->clear();
        break;
    }
  }
}

}  // namespace log
}  // namespace mlsm
