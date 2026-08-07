#pragma once

#include <cstdint>

namespace mlsm {
namespace raft {

// Node identifiers are 1-based; 0 is reserved to mean "none" (e.g. no vote yet,
// no known leader).
using NodeId = uint64_t;
using Term = uint64_t;

enum class MessageType {
  kRequestVote,
  kRequestVoteResp,
  // Heartbeat is a simplified empty AppendEntries used during the election
  // phase to keep followers from timing out. Log-carrying AppendEntries arrives
  // with the log-replication commit.
  kHeartbeat,
  kHeartbeatResp,
};

// A single Raft message. The core neither sends nor receives these itself: the
// caller hands inbound messages to Step() and delivers the outbound messages
// returned by TakeMessages().
struct Message {
  MessageType type;
  NodeId from = 0;
  NodeId to = 0;
  Term term = 0;

  // Set on kRequestVoteResp / kHeartbeatResp to indicate acceptance.
  bool vote_granted = false;
  bool success = false;

  // Log fields (lastLogIndex/lastLogTerm, entries, leaderCommit) are added with
  // the log-replication commit.
};

}  // namespace raft
}  // namespace mlsm
