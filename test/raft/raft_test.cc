#include "mlsm/raft/raft.h"

#include <gtest/gtest.h>

#include <map>
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
        auto it = nodes_.find(m.to);
        if (it == nodes_.end()) continue;
        it->second.Step(m);
        Collect(m.to);
      }
    }
  }

 private:
  void Collect(NodeId id) {
    std::vector<Message> msgs = nodes_.at(id).TakeMessages();
    for (Message& m : msgs) pending_.push_back(m);
  }

  std::map<NodeId, Raft> nodes_;
  std::vector<Message> pending_;
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
