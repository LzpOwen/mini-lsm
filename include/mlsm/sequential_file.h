#pragma once

#include <cstddef>

#include "mlsm/slice.h"
#include "mlsm/status.h"

namespace mlsm {

// Abstract source for sequential byte input, symmetric to WritableFile. The WAL
// Reader pulls blocks through this interface so the record/block parsing stays
// independent of where the bytes come from. A real Posix-backed implementation
// arrives in commit 5; tests use an in-memory std::string-backed source.
class SequentialFile {
 public:
  virtual ~SequentialFile() = default;

  // Reads up to n bytes. On success, *result points to the bytes read (backed
  // by scratch, which must have room for n bytes) and result->size() is the
  // count actually read — a short read (including size 0) means end of file.
  virtual Status Read(size_t n, Slice* result, char* scratch) = 0;

  // Skips n bytes forward. Not needed by the Reader yet; present for symmetry.
  virtual Status Skip(size_t n) = 0;
};

}  // namespace mlsm
