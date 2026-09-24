// mkdtemp is POSIX; make sure glibc exposes it under strict -std=c++17.
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "mlsm/raft/replicated_db.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include <unistd.h>

#include "mlsm/raft/messages.h"
#include "mlsm/raft/raft.h"
#include "mlsm/status.h"

using namespace mlsm;
using namespace mlsm::raft;

namespace {

std::string MakeTempDir() {
  char tmpl[] = "/tmp/mlsm_repl_db_XXXXXX";
  EXPECT_NE(mkdtemp(tmpl), nullptr);
  return tmpl;
}

// ---- single-node fixture ---------------------------------------------------

class ReplicatedDBTest : public ::testing::Test {
 protected:
  void SetUp() override { dir_ = MakeTempDir(); }
  void TearDown() override {
    std::string cmd = "rm -rf '" + dir_ + "'";
    (void)std::system(cmd.c_str());
  }

  ReplicatedDB* OpenSingle() {
    ReplicatedDB* db = nullptr;
    Status s = ReplicatedDB::Open(dir_, /*id=*/1, /*peers=*/{1}, &db);
    EXPECT_TRUE(s.ok()) << s.ToString();
    return db;
  }

  std::string dir_;
};

TEST_F(ReplicatedDBTest, SingleNodePutGetDelete) {
  ReplicatedDB* db = OpenSingle();
  ASSERT_NE(db, nullptr);
  ASSERT_TRUE(db->is_leader());  // Single node elected itself in Open.

  ASSERT_TRUE(db->Put("k", "v").ok());
  std::string got;
  ASSERT_TRUE(db->Get("k", &got).ok());
  EXPECT_EQ(got, "v");

  ASSERT_TRUE(db->Put("k", "v2").ok());  // Overwrite.
  ASSERT_TRUE(db->Get("k", &got).ok());
  EXPECT_EQ(got, "v2");

  ASSERT_TRUE(db->Delete("k").ok());
  EXPECT_TRUE(db->Get("k", &got).IsNotFound());
  delete db;
}

TEST_F(ReplicatedDBTest, WritesSurviveRestart) {
  {
    ReplicatedDB* db = OpenSingle();
    ASSERT_TRUE(db->Put("a", "1").ok());
    ASSERT_TRUE(db->Put("b", "2").ok());
    ASSERT_TRUE(db->Put("c", "3").ok());
    ASSERT_TRUE(db->Delete("b").ok());
    delete db;  // "Crash".
  }

  ReplicatedDB* db = OpenSingle();  // Recovers from raft.log.
  std::string got;
  ASSERT_TRUE(db->Get("a", &got).ok());
  EXPECT_EQ(got, "1");
  ASSERT_TRUE(db->Get("c", &got).ok());
  EXPECT_EQ(got, "3");
  EXPECT_TRUE(db->Get("b", &got).IsNotFound());  // Deleted before crash.
  delete db;
}

// ---- multi-node harness ----------------------------------------------------

// Routes messages between several ReplicatedDBs' underlying Raft instances by
// hand, calling Sync() after each step so persistence and apply happen exactly
// as they would in real use. Single-threaded and deterministic, like the Raft
// core's own test Cluster.
class ReplCluster {
 public:
  void Add(NodeId id, ReplicatedDB* db) { dbs_[id] = db; }

  ReplicatedDB* db(NodeId id) { return dbs_.at(id); }

  // Drives `id` to leadership: it times out (election_timeout==1), campaigns,
  // and the delivered votes carry it to a majority.
  void Elect(NodeId id) {
    dbs_.at(id)->raft().Tick();
    CollectAndSync(id);
    DeliverAll();
  }

  // Ticks only the leader(s) so their heartbeat AppendEntries (carrying entries
  // and the committed index) reaches followers; followers are driven purely by
  // delivered messages. Ticking followers here would trip their short election
  // timeout and start spurious elections before the heartbeat arrives.
  void RunRound() {
    for (auto& kv : dbs_) {
      if (!kv.second->is_leader()) continue;
      kv.second->raft().Tick();
      CollectAndSync(kv.first);
    }
    DeliverAll();
  }

 private:
  void CollectAndSync(NodeId id) {
    std::vector<Message> msgs = dbs_.at(id)->raft().TakeMessages();
    for (Message& m : msgs) pending_.push_back(m);
    (void)dbs_.at(id)->Sync();  // Persist + apply whatever this step produced.
  }

  void DeliverAll(int max_rounds = 100) {
    for (int r = 0; r < max_rounds && !pending_.empty(); ++r) {
      std::vector<Message> batch;
      batch.swap(pending_);
      for (const Message& m : batch) {
        auto it = dbs_.find(m.to);
        if (it == dbs_.end()) continue;
        it->second->raft().Step(m);
        CollectAndSync(m.to);
      }
    }
  }

  std::map<NodeId, ReplicatedDB*> dbs_;
  std::vector<Message> pending_;
};

class ReplicatedDBMultiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    for (int i = 1; i <= 3; ++i) {
      dirs_[i] = MakeTempDir();
      ReplicatedDB* db = nullptr;
      Status s = ReplicatedDB::Open(dirs_[i], i, {1, 2, 3}, &db);
      ASSERT_TRUE(s.ok()) << s.ToString();
      cluster_.Add(i, db);
    }
  }
  void TearDown() override {
    for (auto& kv : dirs_) {
      delete cluster_.db(kv.first);
      std::string cmd = "rm -rf '" + kv.second + "'";
      (void)std::system(cmd.c_str());
    }
  }

  ReplCluster cluster_;
  std::map<NodeId, std::string> dirs_;
};

TEST_F(ReplicatedDBMultiTest, NonLeaderRejectsWrite) {
  // Before any election, no node is a leader, so writes are rejected.
  Status s = cluster_.db(2)->Put("k", "v");
  EXPECT_FALSE(s.ok());  // InvalidArgument("not leader").
}

TEST_F(ReplicatedDBMultiTest, LeaderReplicatesWriteToFollowers) {
  cluster_.Elect(1);
  ASSERT_TRUE(cluster_.db(1)->is_leader());

  ASSERT_TRUE(cluster_.db(1)->Put("x", "1").ok());
  // A few rounds carry the entry and the committed index to the followers.
  cluster_.RunRound();
  cluster_.RunRound();

  for (NodeId id : {1u, 2u, 3u}) {
    std::string got;
    ASSERT_TRUE(cluster_.db(id)->Get("x", &got).ok())
        << "node " << id << " missing key";
    EXPECT_EQ(got, "1");
  }
}

TEST_F(ReplicatedDBMultiTest, OverwriteAndDeleteReplicate) {
  cluster_.Elect(1);
  ASSERT_TRUE(cluster_.db(1)->is_leader());

  ASSERT_TRUE(cluster_.db(1)->Put("k", "v1").ok());
  ASSERT_TRUE(cluster_.db(1)->Put("k", "v2").ok());
  ASSERT_TRUE(cluster_.db(1)->Delete("k").ok());
  cluster_.RunRound();
  cluster_.RunRound();

  for (NodeId id : {1u, 2u, 3u}) {
    std::string got;
    EXPECT_TRUE(cluster_.db(id)->Get("k", &got).IsNotFound())
        << "node " << id << " should have deleted key";
  }
}

}  // namespace
