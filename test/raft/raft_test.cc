#include "mlsm/raft/raft.h"

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "mlsm/raft/messages.h"

using namespace mlsm::raft;

namespace {

// A single-threaded, in-memory cluster harness. It owns the Raft nodes and
// routes messages by hand: Tick() / DeliverAll() drive the whole cluster
// deterministically, so every test is exactly reproducible.
class Cluster {
 public:
  // Builds an n-node cluster (ids 1..n). Each node gets a distinct election
  // timeout so ticking them together can't produce a symmetric split — the core
  // itself draws no randomness.
  Cluster(int n, int base_election_timeout = 10, int heartbeat_timeout = 1) {
    std::vector<NodeId> peers;
    for (int i = 1; i <= n; ++i) peers.push_back(i);
    for (int i = 1; i <= n; ++i) {
      Config cfg;
      cfg.id = i;
      cfg.peers = peers;
      cfg.election_timeout = base_election_timeout + i;
      cfg.heartbeat_timeout = heartbeat_timeout;
      nodes_.emplace(i, Raft(cfg));
    }
  }

  Raft& node(NodeId id) { return nodes_.at(id); }

  bool Propose(NodeId id, const std::string& data) {
    return nodes_.at(id).Propose(data);
  }

  // Simulates a network partition: messages to or from a blocked node are
  // dropped in DeliverAll until it is unblocked.
  void Block(NodeId id) { blocked_.insert(id); }
  void Unblock(NodeId id) { blocked_.erase(id); }

  void Tick(NodeId id) { nodes_.at(id).Tick(); }

  void TickAll() {
    for (auto& kv : nodes_) kv.second.Tick();
  }

  // Ticks node id until it leaves the follower/candidate state or a bound is
  // hit, collecting its outbound messages into the pending queue.
  void TickUntilElection(NodeId id, int max_ticks = 100) {
    for (int i = 0; i < max_ticks; ++i) {
      nodes_.at(id).Tick();
      Collect(id);
      if (nodes_.at(id).role() != Role::kFollower) return;
    }
  }

  // Drains every node's outbound messages into the pending queue.
  void CollectAll() {
    for (auto& kv : nodes_) Collect(kv.first);
  }

  // Delivers all pending messages, collecting any replies, until the cluster
  // goes quiet (or a bound is hit to avoid an infinite loop on a bug).
  void DeliverAll(int max_rounds = 100) {
    for (int round = 0; round < max_rounds && !pending_.empty(); ++round) {
      std::vector<Message> batch;
      batch.swap(pending_);
      for (const Message& m : batch) {
        // Drop messages crossing a partition boundary (either endpoint blocked).
        if (blocked_.count(m.to) || blocked_.count(m.from)) continue;
        auto it = nodes_.find(m.to);
        if (it == nodes_.end()) continue;
        it->second.Step(m);
        Collect(m.to);
      }
    }
  }

  // Ticks the whole cluster and delivers messages for a number of rounds, the
  // usual way to let a leader replicate and heartbeat over time.
  void RunRounds(int rounds) {
    for (int i = 0; i < rounds; ++i) {
      TickAll();
      CollectAll();
      DeliverAll();
    }
  }

 private:
  void Collect(NodeId id) {
    std::vector<Message> msgs = nodes_.at(id).TakeMessages();
    for (Message& m : msgs) pending_.push_back(m);
  }

  std::map<NodeId, Raft> nodes_;
  std::vector<Message> pending_;
  std::set<NodeId> blocked_;  // Partitioned nodes; their messages are dropped.
};

}  // namespace

TEST(RaftElection, SingleNodeBecomesLeaderImmediately) {
  Cluster c(1);
  c.TickUntilElection(1);
  EXPECT_EQ(c.node(1).role(), Role::kLeader);
  EXPECT_EQ(c.node(1).term(), 1u);
  EXPECT_EQ(c.node(1).leader(), 1u);
}

TEST(RaftElection, ThreeNodesElectLeaderWithMajority) {
  Cluster c(3);
  c.TickUntilElection(1);  // Node 1 times out first, becomes candidate.
  EXPECT_EQ(c.node(1).role(), Role::kCandidate);

  c.DeliverAll();  // Votes flow; node 1 gathers a majority.
  EXPECT_EQ(c.node(1).role(), Role::kLeader);
  EXPECT_EQ(c.node(1).term(), 1u);
  // The other two learn the leader from its heartbeat.
  EXPECT_EQ(c.node(2).role(), Role::kFollower);
  EXPECT_EQ(c.node(3).role(), Role::kFollower);
  EXPECT_EQ(c.node(2).leader(), 1u);
  EXPECT_EQ(c.node(3).leader(), 1u);
}

TEST(RaftElection, HigherTermForcesStepDown) {
  Cluster c(3);
  c.TickUntilElection(1);
  c.DeliverAll();
  ASSERT_EQ(c.node(1).role(), Role::kLeader);

  // A message from a strictly higher term makes even a leader step down.
  Message m;
  m.type = MessageType::kRequestVote;
  m.from = 2;
  m.to = 1;
  m.term = c.node(1).term() + 5;
  c.node(1).Step(m);

  EXPECT_EQ(c.node(1).role(), Role::kFollower);
  EXPECT_EQ(c.node(1).term(), m.term);
}

TEST(RaftElection, LowerTermRequestVoteIsRejected) {
  Cluster c(3);
  c.TickUntilElection(1);
  c.DeliverAll();
  ASSERT_EQ(c.node(1).role(), Role::kLeader);
  const Term leader_term = c.node(1).term();

  Message m;
  m.type = MessageType::kRequestVote;
  m.from = 2;
  m.to = 1;
  m.term = leader_term - 1;  // Stale candidate.
  c.node(1).Step(m);

  std::vector<Message> out = c.node(1).TakeMessages();
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].type, MessageType::kRequestVoteResp);
  EXPECT_FALSE(out[0].vote_granted);
  EXPECT_EQ(out[0].term, leader_term);
}

TEST(RaftElection, OneVotePerTerm) {
  Cluster c(3);
  // Node 1 votes for candidate 2.
  Message v2;
  v2.type = MessageType::kRequestVote;
  v2.from = 2;
  v2.to = 1;
  v2.term = 1;
  c.node(1).Step(v2);
  std::vector<Message> out = c.node(1).TakeMessages();
  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(out[0].vote_granted);
  EXPECT_EQ(c.node(1).voted_for(), 2u);

  // Same term, different candidate 3: must be rejected.
  Message v3;
  v3.type = MessageType::kRequestVote;
  v3.from = 3;
  v3.to = 1;
  v3.term = 1;
  c.node(1).Step(v3);
  out = c.node(1).TakeMessages();
  ASSERT_EQ(out.size(), 1u);
  EXPECT_FALSE(out[0].vote_granted);
}

TEST(RaftElection, LeaderHeartbeatKeepsFollowersFromElection) {
  Cluster c(3);
  c.TickUntilElection(1);
  c.DeliverAll();
  ASSERT_EQ(c.node(1).role(), Role::kLeader);

  // Drive many ticks. The leader heartbeats every tick (heartbeat_timeout=1),
  // resetting followers' election timers so no one starts a new election.
  for (int i = 0; i < 50; ++i) {
    c.TickAll();
    c.CollectAll();
    c.DeliverAll();
  }
  EXPECT_EQ(c.node(1).role(), Role::kLeader);
  EXPECT_EQ(c.node(2).role(), Role::kFollower);
  EXPECT_EQ(c.node(3).role(), Role::kFollower);
  EXPECT_EQ(c.node(1).term(), 1u);  // Term never advanced past the first win.
}

TEST(RaftElection, SplitVoteRetriesWithHigherTerm) {
  Cluster c(3);
  // Drive node 2 to candidacy but deliver none of its vote requests, so it
  // never reaches a majority — the split-vote situation.
  for (int i = 0; i < 100 && c.node(2).role() == Role::kFollower; ++i) {
    c.node(2).Tick();
  }
  c.node(2).TakeMessages();  // Discard its vote requests (undelivered).
  ASSERT_EQ(c.node(2).role(), Role::kCandidate);
  const Term t2 = c.node(2).term();

  // Without votes, ticking past another timeout bumps the term and retries.
  for (int i = 0; i < 100 && c.node(2).term() == t2; ++i) {
    c.node(2).Tick();
  }
  EXPECT_EQ(c.node(2).role(), Role::kCandidate);
  EXPECT_GT(c.node(2).term(), t2);
}

TEST(RaftElection, HardStateDirtyTracksTermAndVote) {
  Cluster c(3);
  EXPECT_FALSE(c.node(1).hard_state_dirty());

  Message m;
  m.type = MessageType::kRequestVote;
  m.from = 2;
  m.to = 1;
  m.term = 1;
  c.node(1).Step(m);  // Advances term and records a vote.
  EXPECT_TRUE(c.node(1).hard_state_dirty());

  c.node(1).clear_hard_state_dirty();
  EXPECT_FALSE(c.node(1).hard_state_dirty());
}

// ---- raft-2: log replication ----------------------------------------------

// Helper: drive node 1 to leadership of a fresh cluster of n nodes.
static void ElectLeader1(Cluster& c) {
  c.TickUntilElection(1);
  c.DeliverAll();
  ASSERT_EQ(c.node(1).role(), Role::kLeader);
}

TEST(RaftLog, LeaderReplicatesEntryToFollowers) {
  Cluster c(3);
  ElectLeader1(c);

  ASSERT_TRUE(c.Propose(1, "x"));
  c.RunRounds(3);  // Heartbeat carries the entry; followers append it.

  for (NodeId id : {1u, 2u, 3u}) {
    EXPECT_EQ(c.node(id).last_index(), 1u);
    EXPECT_EQ(c.node(id).entry_at(1).data, "x");
    EXPECT_EQ(c.node(id).entry_at(1).term, 1u);
  }
}

TEST(RaftLog, CommitAdvancesOnMajorityAck) {
  Cluster c(3);
  ElectLeader1(c);

  ASSERT_TRUE(c.Propose(1, "a"));
  c.RunRounds(3);

  // A majority (leader + at least one follower) acked, so the entry commits and
  // the commit index propagates to followers via the next heartbeat.
  EXPECT_EQ(c.node(1).commit_index(), 1u);
  EXPECT_EQ(c.node(2).commit_index(), 1u);
  EXPECT_EQ(c.node(3).commit_index(), 1u);
}

TEST(RaftLog, TakeCommittedDrainsNewlyCommitted) {
  Cluster c(3);
  ElectLeader1(c);

  ASSERT_TRUE(c.Propose(1, "hello"));
  c.RunRounds(3);

  std::vector<LogEntry> committed = c.node(1).TakeCommitted();
  ASSERT_EQ(committed.size(), 1u);
  EXPECT_EQ(committed[0].data, "hello");
  EXPECT_EQ(committed[0].index, 1u);

  // Draining again yields nothing: the apply cursor already advanced.
  EXPECT_TRUE(c.node(1).TakeCommitted().empty());
}

TEST(RaftLog, FollowerCatchesUpAfterPartition) {
  Cluster c(3);
  ElectLeader1(c);

  // Node 3 is partitioned away while three entries replicate to {1, 2}.
  c.Block(3);
  ASSERT_TRUE(c.Propose(1, "a"));
  ASSERT_TRUE(c.Propose(1, "b"));
  ASSERT_TRUE(c.Propose(1, "c"));
  c.RunRounds(5);

  EXPECT_EQ(c.node(2).last_index(), 3u);
  EXPECT_EQ(c.node(1).commit_index(), 3u);  // Majority {1,2} is enough.
  EXPECT_EQ(c.node(3).last_index(), 0u);    // Still empty behind the partition.

  // Heal the partition: the leader backfills node 3 from its heartbeats.
  c.Unblock(3);
  c.RunRounds(5);

  EXPECT_EQ(c.node(3).last_index(), 3u);
  EXPECT_EQ(c.node(3).entry_at(3).data, "c");
  EXPECT_EQ(c.node(3).commit_index(), 3u);
}

TEST(RaftLog, ConflictingSuffixIsOverwritten) {
  // Drive a single follower with crafted AppendEntries to force a conflict.
  Cluster c(3);
  Raft& f = c.node(2);

  // Leader of term 1 replicates three entries.
  Message a1;
  a1.type = MessageType::kAppendEntries;
  a1.from = 1;
  a1.to = 2;
  a1.term = 1;
  a1.prev_log_index = 0;
  a1.prev_log_term = 0;
  a1.entries = {LogEntry{1, 1, "a"}, LogEntry{1, 2, "b"}, LogEntry{1, 3, "c"}};
  f.Step(a1);
  ASSERT_EQ(f.last_index(), 3u);
  f.TakeMessages();

  // A new leader (term 2) overwrites from index 2 onward. Index 1 still matches
  // and is kept; index 2's conflicting term truncates it and the tail.
  Message a2;
  a2.type = MessageType::kAppendEntries;
  a2.from = 3;
  a2.to = 2;
  a2.term = 2;
  a2.prev_log_index = 1;
  a2.prev_log_term = 1;
  a2.entries = {LogEntry{2, 2, "X"}};
  f.Step(a2);

  EXPECT_EQ(f.last_index(), 2u);
  EXPECT_EQ(f.entry_at(1).data, "a");   // Unchanged.
  EXPECT_EQ(f.entry_at(2).data, "X");   // Overwritten.
  EXPECT_EQ(f.entry_at(2).term, 2u);
}

TEST(RaftLog, StaleAppendEntriesIsRejected) {
  // A follower rejects AppendEntries when it lacks the prev entry (log gap).
  Cluster c(3);
  Raft& f = c.node(2);

  Message ae;
  ae.type = MessageType::kAppendEntries;
  ae.from = 1;
  ae.to = 2;
  ae.term = 1;
  ae.prev_log_index = 5;  // Follower's log is empty; nothing at index 5.
  ae.prev_log_term = 1;
  ae.entries = {LogEntry{1, 6, "z"}};
  f.Step(ae);

  std::vector<Message> out = f.TakeMessages();
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].type, MessageType::kAppendEntriesResp);
  EXPECT_FALSE(out[0].success);
  EXPECT_EQ(f.last_index(), 0u);  // Nothing appended.
}

TEST(RaftLog, LeaderRetriesWithLowerIndexAfterRejection) {
  Cluster c(3);
  ElectLeader1(c);

  // Get node 2 replicated to index 1 so its next_index is 2.
  ASSERT_TRUE(c.Propose(1, "a"));
  c.RunRounds(3);
  ASSERT_EQ(c.node(2).last_index(), 1u);

  // A rejection should walk next_index back so the next AppendEntries resends
  // from a lower prev_log_index.
  Message reject;
  reject.type = MessageType::kAppendEntriesResp;
  reject.from = 2;
  reject.to = 1;
  reject.term = c.node(1).term();
  reject.success = false;
  c.node(1).Step(reject);

  c.node(1).TakeMessages();  // Discard anything queued.
  c.Tick(1);                 // Trigger a fresh broadcast.
  std::vector<Message> out = c.node(1).TakeMessages();

  bool saw_retry = false;
  for (const Message& m : out) {
    if (m.type == MessageType::kAppendEntries && m.to == 2) {
      EXPECT_EQ(m.prev_log_index, 0u);  // Walked back from 1.
      saw_retry = true;
    }
  }
  EXPECT_TRUE(saw_retry);
}

TEST(RaftLog, VoteGrantedWhenCandidateLogUpToDate) {
  Cluster c(3);
  Raft& v = c.node(1);  // Empty log, term 0.

  Message rv;
  rv.type = MessageType::kRequestVote;
  rv.from = 2;
  rv.to = 1;
  rv.term = 1;
  rv.last_log_index = 0;
  rv.last_log_term = 0;  // Equally empty log is up-to-date enough.
  v.Step(rv);

  std::vector<Message> out = v.TakeMessages();
  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(out[0].vote_granted);
}

TEST(RaftLog, VoteDeniedWhenCandidateLogBehind) {
  Cluster c(3);
  Raft& v = c.node(1);

  // Give the voter one entry at term 1 so its log is ahead of the candidate's.
  Message ae;
  ae.type = MessageType::kAppendEntries;
  ae.from = 3;
  ae.to = 1;
  ae.term = 1;
  ae.prev_log_index = 0;
  ae.prev_log_term = 0;
  ae.entries = {LogEntry{1, 1, "a"}};
  v.Step(ae);
  v.TakeMessages();

  // Candidate at a higher term but with an empty (stale) log must be denied.
  Message rv;
  rv.type = MessageType::kRequestVote;
  rv.from = 2;
  rv.to = 1;
  rv.term = 2;
  rv.last_log_index = 0;
  rv.last_log_term = 0;
  v.Step(rv);

  std::vector<Message> out = v.TakeMessages();
  ASSERT_EQ(out.size(), 1u);
  EXPECT_FALSE(out[0].vote_granted);
}
