#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mlsm/raft/messages.h"
#include "mlsm/raft/raft.h"
#include "mlsm/status.h"

namespace mlsm {

class WritableFile;

namespace log {
class Writer;
}

namespace raft {

// Hard state read back on restart, handed to Raft::Restore().
struct HardState {
  Term term = 0;
  NodeId vote = 0;
};

// Durable storage for a Raft node's hard state and log, layered on the
// LevelDB-style WAL (log::Writer/Reader). The Raft core stays I/O-free; this is
// the integration layer, so it is the one piece that links both mlsm (Env/WAL)
// and mlsm_raft.
//
// The file is an append-only sequence of records, each tagged with a leading
// byte:
//   kHardState : {term, vote}          -- latest one wins on replay
//   kEntry     : {term, index, data}   -- appended to the recovered log
//   kTruncate  : {from_index}          -- drop recovered entries >= from_index
//
// Simplifications vs etcd/raft (noted for the learning reference): the commit
// index is not persisted (a leader re-advances it after restart), and there is
// no snapshot / log compaction, so the file grows with the log.
class RaftStorage {
 public:
  // Opens the storage file at `path`, creating its parent directory if needed.
  // Any existing contents are replayed and cached for Recover(); either way the
  // file is then opened for appending. Caller owns *out and must delete it.
  static Status Open(const std::string& path, RaftStorage** out);

  ~RaftStorage();

  RaftStorage(const RaftStorage&) = delete;
  RaftStorage& operator=(const RaftStorage&) = delete;

  // Returns the hard state and log entries recovered at Open time. Entries are
  // in index order starting at 1; feed both into Raft::Restore().
  Status Recover(HardState* hs, std::vector<LogEntry>* entries);

  // Persists whatever the core produced but has not yet stored: the hard state
  // if dirty, a truncation marker if a conflict lowered stable_index, and any
  // new log entries — all covered by a single fsync. Advances the core's
  // stable_index watermark on success. Call after each Step/Tick/Propose and
  // before delivering the resulting messages (durable-before-visible, like the
  // DB write path and etcd's persist-Ready-then-send).
  Status Save(Raft& raft);

 private:
  RaftStorage() = default;

  // Replays the on-disk file into recovered_hs_/recovered_entries_ and seeds
  // persisted_index_. Called once from Open.
  Status ReplayFile();

  std::string path_;
  WritableFile* file_ = nullptr;   // owned
  log::Writer* writer_ = nullptr;  // owned

  uint64_t persisted_index_ = 0;   // Highest entry index written to disk.

  HardState recovered_hs_;
  std::vector<LogEntry> recovered_entries_;
};

}  // namespace raft
}  // namespace mlsm
