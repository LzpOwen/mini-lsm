// mkdtemp is POSIX; make sure glibc exposes it under strict -std=c++17.
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "mlsm/raft/storage.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "mlsm/raft/messages.h"
#include "mlsm/raft/raft.h"
#include "mlsm/status.h"

using namespace mlsm;
using namespace mlsm::raft;

namespace {

// Each test gets a fresh temp directory holding one raft.log, removed on
// teardown — mirrors the DBTest crash-recovery harness.
class RaftStorageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/mlsm_raft_store_XXXXXX";
    ASSERT_NE(mkdtemp(tmpl), nullptr);
    dir_ = tmpl;
    path_ = dir_ + "/raft.log";
  }
  void TearDown() override {
    std::string cmd = "rm -rf '" + dir_ + "'";
    (void)std::system(cmd.c_str());
  }

  RaftStorage* OpenStore() {
    RaftStorage* s = nullptr;
    Status st = RaftStorage::Open(path_, &s);
    EXPECT_TRUE(st.ok()) << st.ToString();
    return s;
  }

  static Config ThreeNodeConfig(NodeId id) {
    Config cfg;
    cfg.id = id;
    cfg.peers = {1, 2, 3};
    return cfg;
  }

  // A RequestVote the node will grant (empty logs are equally up-to-date).
  static Message VoteRequest(NodeId from, Term term) {
    Message m;
    m.type = MessageType::kRequestVote;
    m.from = from;
    m.to = 1;
    m.term = term;
    m.last_log_index = 0;
    m.last_log_term = 0;
    return m;
  }

  std::string dir_;
  std::string path_;
};

TEST_F(RaftStorageTest, HardStateSurvivesRestart) {
  {
    Raft r(ThreeNodeConfig(1));
    r.Step(VoteRequest(/*from=*/2, /*term=*/1));  // Grants: term=1, vote=2.
    ASSERT_TRUE(r.hard_state_dirty());
    RaftStorage* store = OpenStore();
    ASSERT_TRUE(store->Save(r).ok());
    delete store;  // "Crash": flush and drop the process state.
  }

  RaftStorage* store = OpenStore();
  HardState hs;
  std::vector<LogEntry> entries;
  ASSERT_TRUE(store->Recover(&hs, &entries).ok());
  EXPECT_EQ(hs.term, 1u);
  EXPECT_EQ(hs.vote, 2u);
  EXPECT_TRUE(entries.empty());
  delete store;
}

TEST_F(RaftStorageTest, LogEntriesSurviveRestart) {
  {
    Config cfg;
    cfg.id = 1;
    cfg.peers = {1};  // Single node wins the election immediately.
    cfg.election_timeout = 1;  // One tick is enough to time out and win.
    Raft r(cfg);
    r.Tick();
    ASSERT_EQ(r.role(), Role::kLeader);
    ASSERT_TRUE(r.Propose("a"));
    ASSERT_TRUE(r.Propose("b"));
    ASSERT_TRUE(r.Propose("c"));
    RaftStorage* store = OpenStore();
    ASSERT_TRUE(store->Save(r).ok());
    delete store;
  }

  RaftStorage* store = OpenStore();
  HardState hs;
  std::vector<LogEntry> entries;
  ASSERT_TRUE(store->Recover(&hs, &entries).ok());
  ASSERT_EQ(entries.size(), 3u);
  EXPECT_EQ(entries[0].data, "a");
  EXPECT_EQ(entries[0].index, 1u);
  EXPECT_EQ(entries[2].data, "c");
  EXPECT_EQ(entries[2].index, 3u);

  // Restore into a fresh node and confirm the log is whole.
  Config cfg;
  cfg.id = 1;
  cfg.peers = {1};
  Raft r(cfg);
  r.Restore(hs.term, hs.vote, entries);
  EXPECT_EQ(r.last_index(), 3u);
  EXPECT_EQ(r.entry_at(2).data, "b");
  delete store;
}

TEST_F(RaftStorageTest, ConflictTruncationReflectedAfterRecovery) {
  {
    Raft r(ThreeNodeConfig(1));

    // Term-1 leader replicates three entries.
    Message a1;
    a1.type = MessageType::kAppendEntries;
    a1.from = 2;
    a1.to = 1;
    a1.term = 1;
    a1.prev_log_index = 0;
    a1.prev_log_term = 0;
    a1.entries = {LogEntry{1, 1, "a"}, LogEntry{1, 2, "b"}, LogEntry{1, 3, "c"}};
    r.Step(a1);
    ASSERT_EQ(r.last_index(), 3u);

    RaftStorage* store = OpenStore();
    ASSERT_TRUE(store->Save(r).ok());  // Persist a,b,c.

    // A term-2 leader overwrites from index 2: index 1 stays, 2..3 truncated.
    Message a2;
    a2.type = MessageType::kAppendEntries;
    a2.from = 3;
    a2.to = 1;
    a2.term = 2;
    a2.prev_log_index = 1;
    a2.prev_log_term = 1;
    a2.entries = {LogEntry{2, 2, "X"}};
    r.Step(a2);
    ASSERT_EQ(r.last_index(), 2u);

    ASSERT_TRUE(store->Save(r).ok());  // Persist truncate + X.
    delete store;
  }

  RaftStorage* store = OpenStore();
  HardState hs;
  std::vector<LogEntry> entries;
  ASSERT_TRUE(store->Recover(&hs, &entries).ok());
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].data, "a");
  EXPECT_EQ(entries[1].data, "X");
  EXPECT_EQ(entries[1].term, 2u);
  delete store;
}

TEST_F(RaftStorageTest, TornTailRecoversToLastGoodRecord) {
  {
    Config cfg;
    cfg.id = 1;
    cfg.peers = {1};
    cfg.election_timeout = 1;  // One tick is enough to time out and win.
    Raft r(cfg);
    r.Tick();
    ASSERT_TRUE(r.Propose("one"));
    ASSERT_TRUE(r.Propose("two"));
    RaftStorage* store = OpenStore();
    ASSERT_TRUE(store->Save(r).ok());
    delete store;
  }

  // Simulate a crash mid-write: append garbage after the last good record.
  {
    std::ofstream f(path_, std::ios::binary | std::ios::app);
    std::string garbage(64, '\xff');
    f.write(garbage.data(), garbage.size());
  }

  RaftStorage* store = OpenStore();
  HardState hs;
  std::vector<LogEntry> entries;
  ASSERT_TRUE(store->Recover(&hs, &entries).ok());
  ASSERT_EQ(entries.size(), 2u);  // Torn tail dropped; good records survive.
  EXPECT_EQ(entries[0].data, "one");
  EXPECT_EQ(entries[1].data, "two");
  delete store;
}

TEST_F(RaftStorageTest, OneVotePerTermSurvivesRestart) {
  {
    Raft r(ThreeNodeConfig(1));
    r.Step(VoteRequest(/*from=*/2, /*term=*/1));  // Vote for candidate 2.
    ASSERT_EQ(r.voted_for(), 2u);
    RaftStorage* store = OpenStore();
    ASSERT_TRUE(store->Save(r).ok());
    delete store;
  }

  RaftStorage* store = OpenStore();
  HardState hs;
  std::vector<LogEntry> entries;
  ASSERT_TRUE(store->Recover(&hs, &entries).ok());

  Raft r(ThreeNodeConfig(1));
  r.Restore(hs.term, hs.vote, entries);
  ASSERT_EQ(r.voted_for(), 2u);

  // Same term, a different candidate must be denied — the vote is remembered
  // across the restart, preserving the one-vote-per-term safety property.
  r.Step(VoteRequest(/*from=*/3, /*term=*/1));
  std::vector<Message> out = r.TakeMessages();
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].type, MessageType::kRequestVoteResp);
  EXPECT_FALSE(out[0].vote_granted);
  delete store;
}

}  // namespace
