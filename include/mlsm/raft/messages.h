#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mlsm {
namespace raft {

// Node identifiers are 1-based; 0 is reserved to mean "none" (e.g. no vote yet,
// no known leader).
using NodeId = uint64_t;
using Term = uint64_t;

// One replicated log entry. `data` is an opaque byte string: the Raft core never
// interprets it. A later commit has the DB layer encode Put/Delete into it, which
// keeps the core free of any storage semantics (matches etcd/raft's []byte Data).
struct LogEntry {
  Term term = 0;
  uint64_t index = 0;
  std::string data;
};

enum class MessageType {
  kRequestVote,
  kRequestVoteResp,
  // AppendEntries carries log entries from the leader to followers. An empty
  // `entries` acts as the heartbeat used to suppress follower election timeouts,
  // so there is no separate heartbeat message (the Raft paper / MIT 6.824 do the
  // same). Note: etcd/raft keeps a distinct MsgHeartbeat/MsgHeartbeatResp pair.
  kAppendEntries,
  kAppendEntriesResp,
};

// A single Raft message. The core neither sends nor receives these itself: the
// caller hands inbound messages to Step() and delivers the outbound messages
// returned by TakeMessages().
struct Message {
  MessageType type;
  NodeId from = 0;
  NodeId to = 0;
  Term term = 0;

  // Set on kRequestVoteResp / kAppendEntriesResp to indicate acceptance.
  bool vote_granted = false;
  bool success = false;

  // kRequestVote: candidate's last log position, for the up-to-date check
  // (paper 5.4.1).
  uint64_t last_log_index = 0;
  Term last_log_term = 0;

  // kAppendEntries: the entry immediately preceding `entries` (consistency
  // check), the entries to append (empty = heartbeat), and the leader's commit
  // index.
  uint64_t prev_log_index = 0;
  Term prev_log_term = 0;
  std::vector<LogEntry> entries;
  uint64_t leader_commit = 0;

  // kAppendEntriesResp: on success, the highest log index now known to match the
  // leader, so the leader can advance matchIndex/nextIndex for this follower.
  uint64_t match_index = 0;
};

}  // namespace raft
}  // namespace mlsm
