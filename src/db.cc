#include "mlsm/db.h"

#include <cstdint>
#include <map>
#include <string>

#include "mlsm/coding.h"
#include "mlsm/env.h"
#include "mlsm/sequential_file.h"
#include "mlsm/wal_reader.h"
#include "mlsm/wal_writer.h"
#include "mlsm/writable_file.h"

namespace mlsm {

namespace {

// WAL record op types. A write records the key and value; a delete records a
// tombstone (key only) so replay can reproduce the deletion in order.
enum ValueType : uint8_t {
  kTypeDelete = 0,
  kTypePut = 1,
};

const char kWalName[] = "wal.log";

std::string DbFile(const std::string& dbpath, const std::string& name) {
  return dbpath + "/" + name;
}

// Encodes one operation into a WAL record payload:
//   [type:1][klen:fixed32][key]  (delete)
//   [type:1][klen:fixed32][key][vlen:fixed32][value]  (put)
void EncodeRecord(std::string* dst, ValueType type, const Slice& key,
                  const Slice& value) {
  dst->clear();
  dst->push_back(static_cast<char>(type));
  char buf[4];
  EncodeFixed32(buf, static_cast<uint32_t>(key.size()));
  dst->append(buf, 4);
  dst->append(key.data(), key.size());
  if (type == kTypePut) {
    EncodeFixed32(buf, static_cast<uint32_t>(value.size()));
    dst->append(buf, 4);
    dst->append(value.data(), value.size());
  }
}

// Parses a record payload written by EncodeRecord. Returns false if the bytes
// are too short to hold the advertised fields (a corrupt/truncated record).
bool DecodeRecord(const Slice& record, ValueType* type, std::string* key,
                  std::string* value) {
  const char* p = record.data();
  size_t left = record.size();
  if (left < 1 + 4) return false;

  *type = static_cast<ValueType>(static_cast<uint8_t>(p[0]));
  p += 1;
  left -= 1;

  uint32_t klen = DecodeFixed32(p);
  p += 4;
  left -= 4;
  if (left < klen) return false;
  key->assign(p, klen);
  p += klen;
  left -= klen;

  if (*type == kTypePut) {
    if (left < 4) return false;
    uint32_t vlen = DecodeFixed32(p);
    p += 4;
    left -= 4;
    if (left < vlen) return false;
    value->assign(p, vlen);
  } else {
    value->clear();
  }
  return true;
}

// Counts corruption reports during recovery; the count is currently advisory.
class RecoveryReporter : public log::Reader::Reporter {
 public:
  void Corruption(size_t bytes, const Status& status) override {
    (void)bytes;
    (void)status;
    ++corruptions_;
  }
  int corruptions_ = 0;
};

class DBImpl : public DB {
 public:
  DBImpl(std::string dbpath) : dbpath_(std::move(dbpath)) {}

  ~DBImpl() override {
    delete wal_writer_;
    if (wal_file_ != nullptr) {
      wal_file_->Close();
      delete wal_file_;
    }
  }

  // Replays an existing WAL (if any) into the table, then opens the same log
  // for appending so subsequent writes continue where recovery left off.
  Status Recover() {
    const std::string wal_path = DbFile(dbpath_, kWalName);
    if (FileExists(wal_path)) {
      Status s = ReplayWal(wal_path);
      if (!s.ok()) return s;
    }
    // Seed the Writer's block cursor from the existing log length so resumed
    // writes stay block-aligned.
    uint64_t wal_size = 0;
    if (FileExists(wal_path)) {
      Status gs = GetFileSize(wal_path, &wal_size);
      if (!gs.ok()) return gs;
    }
    WritableFile* file = nullptr;
    Status s = NewAppendableFile(wal_path, &file);
    if (!s.ok()) return s;
    wal_file_ = file;
    wal_writer_ = new log::Writer(wal_file_, wal_size);
    return Status::OK();
  }

  Status Put(const Slice& key, const Slice& value) override {
    return Write(kTypePut, key, value);
  }

  Status Delete(const Slice& key) override {
    return Write(kTypeDelete, key, Slice());
  }

  Status Get(const Slice& key, std::string* value) override {
    auto it = table_.find(key.ToString());
    if (it == table_.end()) {
      return Status::NotFound(key.ToString());
    }
    value->assign(it->second);
    return Status::OK();
  }

 private:
  // Durability order: append to the WAL (and fsync) before mutating the table,
  // so a crash can never leave a table entry that isn't in the log.
  Status Write(ValueType type, const Slice& key, const Slice& value) {
    std::string record;
    EncodeRecord(&record, type, key, value);
    Status s = wal_writer_->AddRecord(record);
    if (!s.ok()) return s;
    s = wal_file_->Sync();
    if (!s.ok()) return s;
    Apply(type, key.ToString(), value.ToString());
    return Status::OK();
  }

  void Apply(ValueType type, const std::string& key, const std::string& value) {
    if (type == kTypePut) {
      table_[key] = value;
    } else {
      table_.erase(key);
    }
  }

  Status ReplayWal(const std::string& wal_path) {
    SequentialFile* file = nullptr;
    Status s = NewSequentialFile(wal_path, &file);
    if (!s.ok()) return s;

    RecoveryReporter reporter;
    log::Reader reader(file, &reporter);
    std::string scratch;
    Slice record;
    while (reader.ReadRecord(&record, &scratch)) {
      ValueType type;
      std::string key, value;
      if (DecodeRecord(record, &type, &key, &value)) {
        Apply(type, key, value);
      }
    }
    delete file;
    return Status::OK();
  }

  const std::string dbpath_;
  std::map<std::string, std::string> table_;
  WritableFile* wal_file_ = nullptr;
  log::Writer* wal_writer_ = nullptr;
};

}  // namespace

Status DB::Open(const Options& options, const std::string& dbpath, DB** dbptr) {
  *dbptr = nullptr;
  if (options.create_if_missing) {
    Status s = MakeDir(dbpath);
    if (!s.ok()) return s;
  } else if (!FileExists(dbpath)) {
    return Status::InvalidArgument(dbpath + " does not exist");
  }

  DBImpl* impl = new DBImpl(dbpath);
  Status s = impl->Recover();
  if (!s.ok()) {
    delete impl;
    return s;
  }
  *dbptr = impl;
  return Status::OK();
}

}  // namespace mlsm
