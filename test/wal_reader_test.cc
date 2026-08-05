#include "mlsm/wal_reader.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "mlsm/sequential_file.h"
#include "mlsm/slice.h"
#include "mlsm/status.h"
#include "mlsm/wal_writer.h"
#include "mlsm/writable_file.h"

using namespace mlsm;

namespace {

// In-memory sink so we can drive a real log::Writer and feed its output back
// into the Reader — every test is a genuine Writer -> Reader round-trip.
class StringDest : public WritableFile {
 public:
  Status Append(const Slice& data) override {
    contents_.append(data.data(), data.size());
    return Status::OK();
  }
  Status Flush() override { return Status::OK(); }
  Status Sync() override { return Status::OK(); }
  Status Close() override { return Status::OK(); }

  std::string contents_;
};

// In-memory source: hands out bytes of a std::string sequentially.
class StringSource : public SequentialFile {
 public:
  explicit StringSource(std::string data) : data_(std::move(data)), pos_(0) {}

  Status Read(size_t n, Slice* result, char* scratch) override {
    const size_t left = data_.size() - pos_;
    const size_t to_copy = (n < left) ? n : left;
    std::memcpy(scratch, data_.data() + pos_, to_copy);
    pos_ += to_copy;
    *result = Slice(scratch, to_copy);
    return Status::OK();
  }
  Status Skip(size_t n) override {
    pos_ += n;
    return Status::OK();
  }

 private:
  std::string data_;
  size_t pos_;
};

// Accumulates corruption reports for assertions.
class TestReporter : public log::Reader::Reporter {
 public:
  void Corruption(size_t bytes, const Status& status) override {
    ++count_;
    dropped_bytes_ += bytes;
    last_message_ = status.message();
  }
  int count_ = 0;
  size_t dropped_bytes_ = 0;
  std::string last_message_;
};

// Encodes each payload through a real Writer and returns the raw log stream.
std::string WriteRecords(const std::vector<std::string>& payloads) {
  StringDest dest;
  log::Writer w(&dest);
  for (const auto& p : payloads) {
    EXPECT_TRUE(w.AddRecord(p).ok());
  }
  return dest.contents_;
}

// Reads every whole record back out of a raw log stream.
std::vector<std::string> ReadAll(const std::string& log, TestReporter* reporter) {
  StringSource src(log);
  log::Reader reader(&src, reporter);
  std::vector<std::string> out;
  std::string scratch;
  Slice record;
  while (reader.ReadRecord(&record, &scratch)) {
    out.push_back(record.ToString());
  }
  return out;
}

}  // namespace

TEST(WalReader, EmptyRecordRoundTrips) {
  TestReporter reporter;
  auto recs = ReadAll(WriteRecords({""}), &reporter);
  ASSERT_EQ(recs.size(), 1u);
  EXPECT_EQ(recs[0], "");
  EXPECT_EQ(reporter.count_, 0);
}

TEST(WalReader, SmallRecordRoundTrips) {
  TestReporter reporter;
  auto recs = ReadAll(WriteRecords({"hello wal"}), &reporter);
  ASSERT_EQ(recs.size(), 1u);
  EXPECT_EQ(recs[0], "hello wal");
  EXPECT_EQ(reporter.count_, 0);
}

TEST(WalReader, MultipleRecordsReadInOrder) {
  const std::vector<std::string> payloads = {"a", "bb", "ccc", "dddd"};
  TestReporter reporter;
  auto recs = ReadAll(WriteRecords(payloads), &reporter);
  EXPECT_EQ(recs, payloads);
  EXPECT_EQ(reporter.count_, 0);
}

TEST(WalReader, LargeFragmentedRecordReassembles) {
  std::string payload;
  payload.reserve(100000);
  for (int i = 0; i < 100000; ++i) {
    payload.push_back(static_cast<char>('A' + (i % 26)));
  }
  TestReporter reporter;
  auto recs = ReadAll(WriteRecords({payload}), &reporter);
  ASSERT_EQ(recs.size(), 1u);
  EXPECT_EQ(recs[0], payload);
  EXPECT_EQ(reporter.count_, 0);
}

TEST(WalReader, CorruptedPayloadIsReportedAndDropped) {
  std::string log = WriteRecords({"the quick brown fox"});
  // Flip a byte inside the payload (header is 7 bytes, so index 10 is payload).
  ASSERT_GT(log.size(), 10u);
  log[10] ^= 0xff;

  TestReporter reporter;
  auto recs = ReadAll(log, &reporter);
  EXPECT_TRUE(recs.empty());
  EXPECT_EQ(reporter.count_, 1);
  EXPECT_GT(reporter.dropped_bytes_, 0u);
  EXPECT_EQ(reporter.last_message_, "checksum mismatch");
}

TEST(WalReader, GoodRecordSurvivesTrailingCorruption) {
  // Two records; corrupt only the second. The first must still read back.
  std::string log = WriteRecords({"keep-me", "drop-me"});
  // Corrupt the second record's payload. First record occupies
  // kHeaderSize + 7 bytes; its payload byte lands right after that.
  const size_t second_payload = log::kHeaderSize + 7 + log::kHeaderSize + 1;
  ASSERT_GT(log.size(), second_payload);
  log[second_payload] ^= 0xff;

  TestReporter reporter;
  auto recs = ReadAll(log, &reporter);
  ASSERT_EQ(recs.size(), 1u);
  EXPECT_EQ(recs[0], "keep-me");
  EXPECT_EQ(reporter.count_, 1);
}

TEST(WalReader, TruncatedTailReturnsCleanEof) {
  // A record whose write was interrupted: keep the full header but chop the
  // payload short. The Reader should stop cleanly without flagging corruption.
  std::string log = WriteRecords({std::string(200, 'z')});
  ASSERT_GT(log.size(), static_cast<size_t>(log::kHeaderSize + 50));
  log.resize(log::kHeaderSize + 50);  // header claims 200, only 50 present

  TestReporter reporter;
  auto recs = ReadAll(log, &reporter);
  EXPECT_TRUE(recs.empty());
  EXPECT_EQ(reporter.count_, 0);  // truncation at EOF is not corruption
}
