#pragma once

#include <string>

#include "mlsm/slice.h"
#include "mlsm/status.h"

namespace mlsm {

struct Options {
  // Create the database directory if it does not already exist.
  bool create_if_missing = true;
};

// A minimal persistent key-value store. Writes go to a write-ahead log before
// updating the in-memory table, so a crash after a successful Put/Delete does
// not lose the write: Open replays the WAL to rebuild the table.
class DB {
 public:
  // Opens (or creates) the database rooted at directory dbpath. On success
  // *dbptr owns a heap DB the caller must delete.
  static Status Open(const Options& options, const std::string& dbpath,
                     DB** dbptr);

  DB() = default;
  virtual ~DB() = default;

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  virtual Status Put(const Slice& key, const Slice& value) = 0;

  // Copies the value for key into *value. Returns NotFound if the key is absent
  // or has been deleted.
  virtual Status Get(const Slice& key, std::string* value) = 0;

  virtual Status Delete(const Slice& key) = 0;
};

}  // namespace mlsm
