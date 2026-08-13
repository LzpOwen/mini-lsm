#include "mlsm/raft/storage.h"

#include <string>

#include "mlsm/coding.h"
#include "mlsm/env.h"
#include "mlsm/sequential_file.h"
#include "mlsm/slice.h"
#include "mlsm/wal_reader.h"
#include "mlsm/wal_writer.h"
#include "mlsm/writable_file.h"

namespace mlsm {
namespace raft {

namespace {

// Leading byte of each stored record.
enum RecordTag : uint8_t {
  kHardState = 1,
  kEntry = 2,
  kTruncate = 3,
};

// coding.h only ships 32-bit fixed helpers, so a u64 is two little-endian
// halves (low then high).
void PutU32(std::string* s, uint32_t v) {
  char buf[4];
  EncodeFixed32(buf, v);
  s->append(buf, 4);
}

void PutU64(std::string* s, uint64_t v) {
  PutU32(s, static_cast<uint32_t>(v & 0xffffffffu));
  PutU32(s, static_cast<uint32_t>(v >> 32));
}

// Cursor over a decoded record payload. Every getter bounds-checks and flips
// `ok` to false on underrun so a torn/short record is rejected rather than
// read past its end.
struct Decoder {
  const char* p;
  size_t remaining;
  bool ok = true;

  uint32_t U32() {
    if (remaining < 4) {
      ok = false;
      return 0;
    }
    uint32_t v = DecodeFixed32(p);
    p += 4;
    remaining -= 4;
    return v;
  }

  uint64_t U64() {
    uint32_t lo = U32();
    uint32_t hi = U32();
    return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
  }

  // Consumes `n` bytes as a string; empties `ok` if fewer remain.
  std::string Bytes(uint32_t n) {
    if (remaining < n) {
      ok = false;
      return {};
    }
    std::string out(p, n);
    p += n;
    remaining -= n;
    return out;
  }
};

// Counts torn/corrupt bytes during replay. A torn tail is expected after a
// crash mid-write, so we tolerate it and recover to the last good record.
struct CountingReporter : public log::Reader::Reporter {
  size_t dropped = 0;
  void Corruption(size_t bytes, const Status&) override { dropped += bytes; }
};

// Returns the parent directory of `path`, or "." when there is no separator.
std::string ParentDir(const std::string& path) {
  size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return ".";
  if (slash == 0) return "/";
  return path.substr(0, slash);
}

}  // namespace

Status RaftStorage::Open(const std::string& path, RaftStorage** out) {
  Status s = MakeDir(ParentDir(path));
  if (!s.ok()) return s;

  RaftStorage* store = new RaftStorage();
  store->path_ = path;

  if (FileExists(path)) {
    s = store->ReplayFile();
    if (!s.ok()) {
      delete store;
      return s;
    }
  }

  // Resume appending, seeding the Writer's block cursor from the existing size
  // so records stay 32KB-block-aligned (same as the DB WAL resume path).
  uint64_t size = 0;
  if (FileExists(path)) {
    s = GetFileSize(path, &size);
    if (!s.ok()) {
      delete store;
      return s;
    }
  }
  s = NewAppendableFile(path, &store->file_);
  if (!s.ok()) {
    delete store;
    return s;
  }
  store->writer_ = new log::Writer(store->file_, size);

  *out = store;
  return Status::OK();
}

RaftStorage::~RaftStorage() {
  delete writer_;
  delete file_;  // WritableFile::~ closes the fd.
}

Status RaftStorage::ReplayFile() {
  SequentialFile* file = nullptr;
  Status s = NewSequentialFile(path_, &file);
  if (!s.ok()) return s;

  CountingReporter reporter;
  log::Reader reader(file, &reporter);

  Slice record;
  std::string scratch;
  while (reader.ReadRecord(&record, &scratch)) {
    if (record.empty()) continue;
    Decoder d{record.data() + 1, record.size() - 1};
    const uint8_t tag = static_cast<uint8_t>(record[0]);
    switch (tag) {
      case kHardState: {
        HardState hs;
        hs.term = d.U64();
        hs.vote = d.U64();
        if (d.ok) recovered_hs_ = hs;
        break;
      }
      case kEntry: {
        LogEntry e;
        e.term = d.U64();
        e.index = d.U64();
        uint32_t len = d.U32();
        e.data = d.Bytes(len);
        if (d.ok) recovered_entries_.push_back(e);
        break;
      }
      case kTruncate: {
        uint64_t from = d.U64();
        if (d.ok) {
          while (!recovered_entries_.empty() &&
                 recovered_entries_.back().index >= from) {
            recovered_entries_.pop_back();
          }
        }
        break;
      }
      default:
        break;  // Unknown tag: skip (forward-compat / stray byte).
    }
  }

  delete file;
  persisted_index_ =
      recovered_entries_.empty() ? 0 : recovered_entries_.back().index;
  return Status::OK();
}

Status RaftStorage::Recover(HardState* hs, std::vector<LogEntry>* entries) {
  *hs = recovered_hs_;
  *entries = recovered_entries_;
  return Status::OK();
}

Status RaftStorage::Save(Raft& raft) {
  // 1. Hard state, if it changed since the last durable write.
  if (raft.hard_state_dirty()) {
    std::string rec(1, static_cast<char>(kHardState));
    PutU64(&rec, raft.term());
    PutU64(&rec, raft.voted_for());
    Status s = writer_->AddRecord(Slice(rec));
    if (!s.ok()) return s;
    raft.clear_hard_state_dirty();
  }

  // 2. A conflict truncation lowered the watermark below what we've stored:
  //    record it so replay drops the invalidated suffix.
  if (raft.stable_index() < persisted_index_) {
    std::string rec(1, static_cast<char>(kTruncate));
    PutU64(&rec, raft.stable_index() + 1);
    Status s = writer_->AddRecord(Slice(rec));
    if (!s.ok()) return s;
    persisted_index_ = raft.stable_index();
  }

  // 3. Any log entries appended since the last save.
  for (uint64_t i = persisted_index_ + 1; i <= raft.last_index(); ++i) {
    const LogEntry& e = raft.entry_at(i);
    std::string rec(1, static_cast<char>(kEntry));
    PutU64(&rec, e.term);
    PutU64(&rec, e.index);
    PutU32(&rec, static_cast<uint32_t>(e.data.size()));
    rec.append(e.data);
    Status s = writer_->AddRecord(Slice(rec));
    if (!s.ok()) return s;
  }

  // 4. One fsync covers the whole batch, then advance the watermarks.
  Status s = file_->Sync();
  if (!s.ok()) return s;
  persisted_index_ = raft.last_index();
  raft.set_stable_index(raft.last_index());
  return Status::OK();
}

}  // namespace raft
}  // namespace mlsm
