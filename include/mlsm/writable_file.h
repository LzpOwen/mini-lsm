#pragma once

#include "mlsm/slice.h"
#include "mlsm/status.h"

namespace mlsm {

// Abstract sink for sequential byte output. The WAL Writer appends encoded
// records through this interface so the record/block logic stays independent
// of where the bytes actually land. A real Posix-backed implementation arrives
// in commit 5; tests use an in-memory std::string-backed sink.
class WritableFile {
 public:
  virtual ~WritableFile() = default;

  // Appends data to the end of the file (may buffer internally).
  virtual Status Append(const Slice& data) = 0;

  // Pushes any buffered data to the OS (does not fsync).
  virtual Status Flush() = 0;

  // Flushes and durably persists to stable storage.
  virtual Status Sync() = 0;

  virtual Status Close() = 0;
};

}  // namespace mlsm
