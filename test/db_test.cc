// mkdtemp is POSIX; make sure glibc exposes it under strict -std=c++17.
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "mlsm/db.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include <unistd.h>

#include "mlsm/status.h"

using namespace mlsm;

namespace {

// Creates a unique temporary directory under /tmp and removes it on teardown,
// so each test gets a fresh on-disk database that leaves nothing behind.
class DBTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/mlsm_db_test_XXXXXX";
    ASSERT_NE(mkdtemp(tmpl), nullptr);
    dbpath_ = tmpl;
  }
  void TearDown() override {
    // Remove the wal and directory; ignore errors (best-effort cleanup).
    std::string cmd = "rm -rf '" + dbpath_ + "'";
    (void)std::system(cmd.c_str());
  }

  // Opens (or reopens) the database at dbpath_.
  DB* OpenDB() {
    DB* db = nullptr;
    Options options;
    Status s = DB::Open(options, dbpath_, &db);
    EXPECT_TRUE(s.ok()) << s.ToString();
    return db;
  }

  std::string dbpath_;
};

TEST_F(DBTest, PutGetAndMissingKey) {
  DB* db = OpenDB();
  ASSERT_NE(db, nullptr);
  ASSERT_TRUE(db->Put("name", "kiro").ok());

  std::string value;
  ASSERT_TRUE(db->Get("name", &value).ok());
  EXPECT_EQ(value, "kiro");

  Status s = db->Get("absent", &value);
  EXPECT_TRUE(s.IsNotFound());
  delete db;
}

TEST_F(DBTest, DeleteRemovesKey) {
  DB* db = OpenDB();
  ASSERT_TRUE(db->Put("k", "v").ok());
  ASSERT_TRUE(db->Delete("k").ok());

  std::string value;
  EXPECT_TRUE(db->Get("k", &value).IsNotFound());
  delete db;
}

TEST_F(DBTest, OverwriteReturnsLatestValue) {
  DB* db = OpenDB();
  ASSERT_TRUE(db->Put("k", "first").ok());
  ASSERT_TRUE(db->Put("k", "second").ok());

  std::string value;
  ASSERT_TRUE(db->Get("k", &value).ok());
  EXPECT_EQ(value, "second");
  delete db;
}

TEST_F(DBTest, RecoversDataAfterReopen) {
  DB* db = OpenDB();
  ASSERT_TRUE(db->Put("a", "1").ok());
  ASSERT_TRUE(db->Put("b", "2").ok());
  ASSERT_TRUE(db->Put("c", "3").ok());
  delete db;  // Closes and flushes the WAL.

  db = OpenDB();  // Replays the WAL.
  ASSERT_NE(db, nullptr);
  std::string value;
  ASSERT_TRUE(db->Get("a", &value).ok());
  EXPECT_EQ(value, "1");
  ASSERT_TRUE(db->Get("b", &value).ok());
  EXPECT_EQ(value, "2");
  ASSERT_TRUE(db->Get("c", &value).ok());
  EXPECT_EQ(value, "3");
  delete db;
}

TEST_F(DBTest, DeleteSurvivesRecovery) {
  DB* db = OpenDB();
  ASSERT_TRUE(db->Put("k", "v").ok());
  ASSERT_TRUE(db->Delete("k").ok());
  delete db;

  db = OpenDB();
  std::string value;
  EXPECT_TRUE(db->Get("k", &value).IsNotFound());
  delete db;
}

TEST_F(DBTest, AppendAfterRecoveryKeepsAllData) {
  DB* db = OpenDB();
  ASSERT_TRUE(db->Put("a", "1").ok());
  ASSERT_TRUE(db->Put("b", "2").ok());
  ASSERT_TRUE(db->Put("c", "3").ok());
  delete db;

  db = OpenDB();  // Recover, then keep writing to the same WAL.
  ASSERT_TRUE(db->Put("d", "4").ok());
  delete db;

  db = OpenDB();  // Recover again; must see both the old and the appended keys.
  std::string value;
  for (const auto& kv : {std::pair<std::string, std::string>{"a", "1"},
                         {"b", "2"}, {"c", "3"}, {"d", "4"}}) {
    ASSERT_TRUE(db->Get(kv.first, &value).ok()) << "missing " << kv.first;
    EXPECT_EQ(value, kv.second);
  }
  delete db;
}

TEST_F(DBTest, RecoversManyRecordsAcrossBlocks) {
  // Enough small records that the WAL crosses several 32KB blocks, exercising
  // block-aligned resume: reopen midway and confirm nothing is lost.
  DB* db = OpenDB();
  for (int i = 0; i < 5000; ++i) {
    ASSERT_TRUE(db->Put("key" + std::to_string(i),
                        "value" + std::to_string(i))
                    .ok());
  }
  delete db;

  db = OpenDB();
  ASSERT_TRUE(db->Put("after", "reopen").ok());  // Append past a block boundary.
  delete db;

  db = OpenDB();
  std::string value;
  ASSERT_TRUE(db->Get("key0", &value).ok());
  EXPECT_EQ(value, "value0");
  ASSERT_TRUE(db->Get("key4999", &value).ok());
  EXPECT_EQ(value, "value4999");
  ASSERT_TRUE(db->Get("after", &value).ok());
  EXPECT_EQ(value, "reopen");
  delete db;
}

}  // namespace
