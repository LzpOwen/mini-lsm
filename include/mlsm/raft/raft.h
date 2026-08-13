#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
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

  // Proposes a new command. Only a leader accepts it: the entry is appended to
  // the local log at index LastIndex()+1 with the current term, and replication
  // happens on subsequent broadcasts. Returns false (and does nothing) if this
  // node is not the leader.
  bool Propose(const std::string& data);

  // Removes and returns the entries committed since the last call — the range
  // (last_applied_, commit_index_] — advancing the apply cursor. This is the
  // minimal analogue of etcd/raft's Ready.CommittedEntries.
  std::vector<LogEntry> TakeCommitted();

  // Reinstalls persisted state after a restart. Call once, right after
  // construction and before feeding any message: sets the hard state and
  // repopulates the log from `entries` (which must be in index order starting
  // at 1), marks everything as already stable, and clears the dirty flag.
  // commit_index is intentionally not restored — a leader's AppendEntries
  // re-advances it (etcd/raft instead persists commit inside its HardState).
  void Restore(Term term, NodeId vote, const std::vector<LogEntry>& entries);

  Role role() const { return role_; }
  Term term() const { return current_term_; }
  NodeId id() const { return config_.id; }
  NodeId leader() const { return leader_; }  // 0 if unknown.
  NodeId voted_for() const { return voted_for_; }
  uint64_t commit_index() const { return commit_index_; }
  uint64_t last_index() const { return LastIndex(); }
  // Read-only introspection for tests: the entry at a log index (0 = sentinel).
  const LogEntry& entry_at(uint64_t index) const { return log_[index]; }

  // Persistence watermark: the highest log index the caller has durably written.
  // The integration layer (RaftStorage) reads it to know which entries still
  // need flushing, and lowers it via set_stable_index() when a conflict
  // truncation invalidates already-persisted entries. The core itself does no
  // I/O; this is just a marker it maintains for the caller.
  uint64_t stable_index() const { return stable_index_; }
  void set_stable_index(uint64_t index) { stable_index_ = index; }

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
  void HandleAppendEntries(const Message& msg);      // follower / candidate side
  void HandleAppendEntriesResp(const Message& msg);  // leader side
  void BroadcastAppendEntries();                     // to every peer
  void SendAppendEntries(NodeId peer);               // empty entries == heartbeat
  void MaybeCommit();                                // leader advances commit_index_
  size_t QuorumSize() const;
  void Send(const Message& msg);

  // Log accessors. log_ carries an index-0 sentinel {term:0,index:0} so that a
  // 1-based Raft index i lives at log_[i]; LastIndex() is log_.size()-1 and
  // TermAt(0) is 0, which makes prev_log_index==0 pass the consistency check
  // without special-casing an empty log. (etcd/raft instead splits log into an
  // unstable buffer plus a truncatable storage offset; the sentinel is a
  // deliberate simplification for a learning project.)
  uint64_t LastIndex() const;
  Term LastTerm() const;
  Term TermAt(uint64_t index) const;

  Config config_;
  Role role_ = Role::kFollower;

  // Persistent (hard) state.
  Term current_term_ = 0;
  NodeId voted_for_ = 0;  // 0 = have not voted this term.

  // Replicated log. Carries an index-0 sentinel (see LastIndex()), so the log is
  // never empty and index i maps to log_[i].
  std::vector<LogEntry> log_;
  uint64_t commit_index_ = 0;   // Highest index known to be committed.
  uint64_t last_applied_ = 0;   // Apply cursor drained by TakeCommitted().
  uint64_t stable_index_ = 0;   // Highest index the caller has durably stored.

  // Volatile state.
  NodeId leader_ = 0;
  int election_elapsed_ = 0;
  int heartbeat_elapsed_ = 0;
  std::vector<NodeId> votes_granted_;  // Peers that voted for us this term.

  // Leader-only volatile state, reset on each election win: for every peer, the
  // next log index to send and the highest index known to be replicated there.
  std::map<NodeId, uint64_t> next_index_;
  std::map<NodeId, uint64_t> match_index_;

  bool hard_state_dirty_ = false;
  std::vector<Message> out_msgs_;
};

}  // namespace raft
}  // namespace mlsm
