#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "mlsm/raft/messages.h"
#include "mlsm/raft/raft.h"
#include "mlsm/raft/storage.h"
#include "mlsm/slice.h"
#include "mlsm/status.h"

namespace mlsm {
namespace raft {

// A replicated key-value store: the DB write path routed through Raft. Put and
// Delete are encoded as commands, proposed to the Raft log, persisted, and only
// applied to the in-memory table once committed. This is the integration layer
// that finally hangs a real state machine off the Raft core — the core stays
// I/O-free, RaftStorage owns durability, and this class owns the state machine.
//
// Simplifications vs a production system (noted inline where they bite): apply
// is synchronous (no background loop), the state machine is a std::map with no
// snapshot/compaction, and multi-node crash recovery does not rebuild the
// commit point (single-node recovery is exact; see Open).
class ReplicatedDB {
 public:
  // Opens (or recovers) a node at `dir`, storing the Raft log in dir/raft.log.
  // `peers` is the full membership including `id`. A single-node cluster elects
  // itself so it is immediately ready for writes; a multi-node cluster is left
  // as a follower for the caller to drive (see raft() and the test Cluster).
  static Status Open(const std::string& dir, NodeId id,
                     std::vector<NodeId> peers, ReplicatedDB** out);

  ~ReplicatedDB();

  ReplicatedDB(const ReplicatedDB&) = delete;
  ReplicatedDB& operator=(const ReplicatedDB&) = delete;

  // Writes: rejected with InvalidArgument unless this node is the leader. The
  // command is proposed, persisted, then any newly committed entries applied.
  Status Put(const Slice& key, const Slice& value);
  Status Delete(const Slice& key);

  // Reads the local applied state; NotFound if absent.
  Status Get(const Slice& key, std::string* value);

  // Persists whatever the core just produced and applies newly committed
  // entries. Writes call this internally; the multi-node test calls it after
  // stepping messages into the underlying Raft directly.
  Status Sync();

  bool is_leader() const { return raft_.role() == Role::kLeader; }
  Term term() const { return raft_.term(); }
  uint64_t applied_index() const { return applied_index_; }
  Raft& raft() { return raft_; }  // For test-driven multi-node routing.

 private:
  ReplicatedDB(Raft raft, RaftStorage* storage)
      : raft_(std::move(raft)), storage_(storage) {}

  // Applies committed-but-unapplied entries (applied_index_, commit_index] to
  // table_ by decoding each command.
  void ApplyCommitted();

  Raft raft_;
  RaftStorage* storage_;  // owned
  std::map<std::string, std::string> table_;
  uint64_t applied_index_ = 0;
};

}  // namespace raft
}  // namespace mlsm
