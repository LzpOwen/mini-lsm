// db_bench — a small, dependency-free benchmark harness for the mini-lsm DB.
//
// It measures a few baseline numbers so later work (skiplist, SST, leveling)
// has a before/after to compare against. This is a skeleton: hand-rolled
// std::chrono timing, no statistical rigor, no external benchmark library.
//
//   ./db_bench [num_entries]     (default 100000)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <unistd.h>

#include <chrono>

#include "mlsm/db.h"

using namespace mlsm;

namespace {

// Fixed-width key so records are uniform: "key" + zero-padded index.
std::string MakeKey(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%012d", i);
  return std::string(buf);
}

// A fixed-length value, cheap to build and constant across entries.
std::string MakeValue() { return std::string(100, 'v'); }

class Timer {
 public:
  Timer() : start_(std::chrono::steady_clock::now()) {}
  double ElapsedSeconds() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - start_).count();
  }

 private:
  std::chrono::steady_clock::time_point start_;
};

void PrintResult(const char* name, long ops, double seconds) {
  const double per_op_us = seconds > 0 ? (seconds * 1e6 / ops) : 0.0;
  const double ops_per_sec = seconds > 0 ? (ops / seconds) : 0.0;
  std::printf("%-12s %8ld ops  %8.3f s  %12.0f ops/s  %8.2f us/op\n", name, ops,
              seconds, ops_per_sec, per_op_us);
}

// Aborts the benchmark loudly if the DB reports an error — a bad status here
// means the numbers would be meaningless.
void Check(const Status& s, const char* what) {
  if (!s.ok()) {
    std::fprintf(stderr, "FATAL: %s: %s\n", what, s.ToString().c_str());
    std::exit(1);
  }
}

DB* OpenAt(const std::string& dbpath) {
  DB* db = nullptr;
  Options options;
  Check(DB::Open(options, dbpath, &db), "DB::Open");
  return db;
}

// Sequentially Put n entries. Each Put appends to the WAL and fsyncs, so this
// measures durable write throughput.
void BenchFillSeq(const std::string& dbpath, int n) {
  DB* db = OpenAt(dbpath);
  const std::string value = MakeValue();
  Timer t;
  for (int i = 0; i < n; ++i) {
    Check(db->Put(MakeKey(i), value), "Put");
  }
  PrintResult("fillseq", n, t.ElapsedSeconds());
  delete db;
}

// Random point reads over the keys written by BenchFillSeq. Measures in-memory
// table lookups (all keys are resident after recovery).
void BenchReadHot(const std::string& dbpath, int n) {
  DB* db = OpenAt(dbpath);
  std::mt19937 rng(12345);  // Fixed seed for reproducibility.
  std::uniform_int_distribution<int> pick(0, n - 1);
  std::string value;
  Timer t;
  for (int i = 0; i < n; ++i) {
    Check(db->Get(MakeKey(pick(rng)), &value), "Get");
  }
  PrintResult("readhot", n, t.ElapsedSeconds());
  delete db;
}

// Close the DB, then reopen the same directory and time how long WAL replay
// takes to rebuild the memtable. This is the crash-recovery path from commit 5.
void BenchRecover(const std::string& dbpath) {
  Timer t;
  DB* db = OpenAt(dbpath);
  const double seconds = t.ElapsedSeconds();
  // A read confirms the table is actually populated after replay.
  std::string value;
  Check(db->Get(MakeKey(0), &value), "Get after recover");
  PrintResult("recover", 1, seconds);
  delete db;
}

}  // namespace

int main(int argc, char** argv) {
  int num_entries = 100000;
  if (argc > 1) {
    num_entries = std::atoi(argv[1]);
    if (num_entries <= 0) {
      std::fprintf(stderr, "usage: %s [num_entries > 0]\n", argv[0]);
      return 1;
    }
  }

  char tmpl[] = "/tmp/mlsm_bench_XXXXXX";
  if (mkdtemp(tmpl) == nullptr) {
    std::perror("mkdtemp");
    return 1;
  }
  const std::string dbpath = tmpl;

  std::printf("mini-lsm db_bench: %d entries, value=%zu bytes, dir=%s\n",
              num_entries, MakeValue().size(), dbpath.c_str());

  BenchFillSeq(dbpath, num_entries);  // Populate + measure writes.
  BenchReadHot(dbpath, num_entries);  // Random reads over the live table.
  BenchRecover(dbpath);               // Reopen: time WAL replay.

  // Best-effort cleanup of the temporary database directory.
  std::string cmd = "rm -rf '" + dbpath + "'";
  (void)std::system(cmd.c_str());
  return 0;
}
