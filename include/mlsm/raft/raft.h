#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mlsm/raft/messages.h"

namespace mlsm {
namespace raft {

enum class Role { kFollower, kCandidate, kLeader };

struct Config {
  NodeId id = 0;
  std::vector<NodeId> peers;  // Full cluster membership, including this node.

  // Timeouts are measured in logical ticks, not real time. Real Raft randomizes
  // election_timeout to avoid split votes; here the core is deterministic and
  // the caller supplies distinct timeouts to break symmetry (see tests).
  int election_timeout = 10;
  int heartbeat_timeout = 1;
};

// A deterministic, message-driven Raft state machine (leader election only).
//
// The core does no I/O, spawns no threads, and reads no real clock. It consumes
// inbound messages via Step() and logical time via Tick(), and produces outbound
// messages the caller drains with TakeMessages(). Identical input sequences yield
// identical output, so a cluster can be driven single-threaded in tests.
//
// Log replication (AppendEntries with entries) and durable persistence of the
// hard state arrive in later commits.
class Raft {
 public:
  explicit Raft(const Config& config);

  // Handles one inbound message.
  void Step(const Message& msg);

  // Advances the logical clock by one tick.
  void Tick();

  // Removes and returns the messages queued for delivery since the last call.
  std::vector<Message> TakeMessages();

  Role role() const { return role_; }
  Term term() const { return current_term_; }
  NodeId id() const { return config_.id; }
  NodeId leader() const { return leader_; }  // 0 if unknown.
  NodeId voted_for() const { return voted_for_; }

  // The hard state (current_term_, voted_for_) is what a real Raft must persist
  // before responding. It is marked dirty on change so the caller knows when a
  // durable write would be required; actual persistence lands in a later commit.
  bool hard_state_dirty() const { return hard_state_dirty_; }
  void clear_hard_state_dirty() { hard_state_dirty_ = false; }

 private:
  void BecomeFollower(Term term, NodeId leader);
  void BecomeCandidate();
  void BecomeLeader();

  void StepFollower(const Message& msg);
  void StepCandidate(const Message& msg);
  void StepLeader(const Message& msg);

  void HandleRequestVote(const Message& msg);
  void BroadcastHeartbeat();
  size_t QuorumSize() const;
  void Send(const Message& msg);

  Config config_;
  Role role_ = Role::kFollower;

  // Persistent (hard) state.
  Term current_term_ = 0;
  NodeId voted_for_ = 0;  // 0 = have not voted this term.

  // Volatile state.
  NodeId leader_ = 0;
  int election_elapsed_ = 0;
  int heartbeat_elapsed_ = 0;
  std::vector<NodeId> votes_granted_;  // Peers that voted for us this term.

  bool hard_state_dirty_ = false;
  std::vector<Message> out_msgs_;
};

}  // namespace raft
}  // namespace mlsm
