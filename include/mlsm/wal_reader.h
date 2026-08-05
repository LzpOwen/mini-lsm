#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "mlsm/slice.h"
#include "mlsm/status.h"
#include "mlsm/wal_writer.h"  // kBlockSize / kHeaderSize / RecordType

namespace mlsm {

class SequentialFile;

namespace log {

// Reads back the records written by log::Writer, verifying each physical
// record's crc and reassembling fragmented (First/Middle/Last) records into
// whole logical records. Corruption and truncation are reported through an
// optional Reporter rather than failing the read, so a caller replaying a WAL
// after a crash can decide how to react to a torn tail.
class Reader {
 public:
  // Reports bytes dropped due to corruption or truncation. The Reader keeps
  // going after reporting, resynchronizing at the next block boundary.
  struct Reporter {
    virtual ~Reporter() = default;
    virtual void Corruption(size_t bytes, const Status& status) = 0;
  };

  // Does not own file or reporter; both must outlive the Reader. reporter may
  // be nullptr to ignore corruption.
  Reader(SequentialFile* file, Reporter* reporter);

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  // Reads the next whole logical record into *record. When the record spans
  // fragments, *scratch is used as the reassembly buffer and *record points
  // into it; otherwise *record points into the internal block buffer and is
  // valid until the next ReadRecord call. Returns false at end of input.
  bool ReadRecord(Slice* record, std::string* scratch);

 private:
  // Return values that don't fit in RecordType: kEof signals no more data,
  // kBadRecord signals a dropped physical record the caller should skip.
  enum {
    kEof = kMaxRecordType + 1,
    kBadRecord = kMaxRecordType + 2,
  };

  // Reads one physical record. Returns its RecordType (or kEof/kBadRecord) and
  // sets *result to its payload on success.
  unsigned int ReadPhysicalRecord(Slice* result);

  void ReportDrop(size_t bytes, const Status& reason);

  SequentialFile* file_;
  Reporter* reporter_;
  bool eof_;  // Last Read returned a short block; no more data to pull.

  std::string backing_store_;  // Owns the bytes behind buffer_.
  Slice buffer_;               // Unconsumed bytes of the current block.
};

}  // namespace log
}  // namespace mlsm
