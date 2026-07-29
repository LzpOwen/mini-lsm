#include "mlsm/wal_writer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "mlsm/crc32c.h"
#include "mlsm/slice.h"
#include "mlsm/status.h"
#include "mlsm/writable_file.h"

using namespace mlsm;

namespace {

// In-memory sink: everything appended lands in a std::string we can inspect.
class StringDest : public WritableFile {
 public:
  Status Append(const Slice& data) override {
    contents_.append(data.data(), data.size());
    return Status::OK();
  }
  Status Flush() override { return Status::OK(); }
  Status Sync() override { return Status::OK(); }
  Status Close() override { return Status::OK(); }

  const std::string& contents() const { return contents_; }

 private:
  std::string contents_;
};

struct PhysicalRecord {
  log::RecordType type;
  std::string payload;
};

uint32_t DecodeFixed32(const char* p) {
  const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
  return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
         (static_cast<uint32_t>(b[2]) << 16) |
         (static_cast<uint32_t>(b[3]) << 24);
}

// Walks the raw log stream and returns every physical record found. Zero
// trailers (a run shorter than a header that decodes to type kZeroType) are
// skipped by jumping to the next block boundary, mirroring the writer.
std::vector<PhysicalRecord> ParseAll(const std::string& log) {
  std::vector<PhysicalRecord> out;
  size_t pos = 0;
  while (pos + log::kHeaderSize <= log.size()) {
    const int block_off = static_cast<int>(pos % log::kBlockSize);
    if (log::kBlockSize - block_off < log::kHeaderSize) {
      // Not enough room for a header in this block -> skip its zero trailer.
      pos += (log::kBlockSize - block_off);
      continue;
    }
    const char* h = log.data() + pos;
    const uint32_t stored_crc = DecodeFixed32(h);
    const uint16_t length = static_cast<uint8_t>(h[4]) |
                            (static_cast<uint16_t>(static_cast<uint8_t>(h[5]))
                             << 8);
    const uint8_t type = static_cast<uint8_t>(h[6]);

    if (type == log::kZeroType && length == 0) {
      // Trailer padding: advance to the next block boundary.
      pos += (log::kBlockSize - block_off);
      continue;
    }

    const char* data = h + log::kHeaderSize;
    // Verify the masked crc so tests also cover checksum correctness.
    char tb = static_cast<char>(type);
    uint32_t crc = crc32c::Extend(crc32c::Value(&tb, 1), data, length);
    EXPECT_EQ(stored_crc, crc32c::Mask(crc));

    out.push_back({static_cast<log::RecordType>(type),
                   std::string(data, length)});
    pos += log::kHeaderSize + length;
  }
  return out;
}

}  // namespace

TEST(WalWriter, EmptyRecordEmitsOneFull) {
  StringDest dest;
  log::Writer w(&dest);
  ASSERT_TRUE(w.AddRecord(Slice("", 0)).ok());

  auto recs = ParseAll(dest.contents());
  ASSERT_EQ(recs.size(), 1u);
  EXPECT_EQ(recs[0].type, log::kFullType);
  EXPECT_EQ(recs[0].payload, "");
  EXPECT_EQ(dest.contents().size(), static_cast<size_t>(log::kHeaderSize));
}

TEST(WalWriter, SmallRecordRoundTrips) {
  StringDest dest;
  log::Writer w(&dest);
  const std::string payload = "hello wal";
  ASSERT_TRUE(w.AddRecord(payload).ok());

  auto recs = ParseAll(dest.contents());
  ASSERT_EQ(recs.size(), 1u);
  EXPECT_EQ(recs[0].type, log::kFullType);
  EXPECT_EQ(recs[0].payload, payload);
}

TEST(WalWriter, MultipleRecordsPackTogether) {
  StringDest dest;
  log::Writer w(&dest);
  const std::vector<std::string> payloads = {"a", "bb", "ccc", "dddd"};
  for (const auto& p : payloads) ASSERT_TRUE(w.AddRecord(p).ok());

  auto recs = ParseAll(dest.contents());
  ASSERT_EQ(recs.size(), payloads.size());
  for (size_t i = 0; i < payloads.size(); ++i) {
    EXPECT_EQ(recs[i].type, log::kFullType);
    EXPECT_EQ(recs[i].payload, payloads[i]);
  }
}

TEST(WalWriter, LargeRecordFragmentsAcrossBlocks) {
  StringDest dest;
  log::Writer w(&dest);
  // Bigger than one block forces First/Middle*/Last fragmentation.
  std::string payload;
  payload.reserve(100000);
  for (int i = 0; i < 100000; ++i) {
    payload.push_back(static_cast<char>('A' + (i % 26)));
  }
  ASSERT_TRUE(w.AddRecord(payload).ok());

  auto recs = ParseAll(dest.contents());
  ASSERT_GE(recs.size(), 3u);
  EXPECT_EQ(recs.front().type, log::kFirstType);
  EXPECT_EQ(recs.back().type, log::kLastType);
  for (size_t i = 1; i + 1 < recs.size(); ++i) {
    EXPECT_EQ(recs[i].type, log::kMiddleType);
  }

  std::string reassembled;
  for (const auto& r : recs) reassembled += r.payload;
  EXPECT_EQ(reassembled, payload);
}

TEST(WalWriter, TailShorterThanHeaderIsZeroPadded) {
  StringDest dest;
  log::Writer w(&dest);

  // Fill the first block so that fewer than kHeaderSize bytes remain: leave a
  // gap of exactly (kHeaderSize - 1) bytes at the block tail. A payload of
  // (kBlockSize - kHeaderSize - (kHeaderSize - 1)) does that.
  const int gap = log::kHeaderSize - 1;
  const int first_payload = log::kBlockSize - log::kHeaderSize - gap;
  ASSERT_GT(first_payload, 0);
  ASSERT_TRUE(w.AddRecord(std::string(first_payload, 'x')).ok());

  // Next record must start in a brand-new block after a zero trailer.
  ASSERT_TRUE(w.AddRecord("second").ok());

  auto recs = ParseAll(dest.contents());
  ASSERT_EQ(recs.size(), 2u);
  EXPECT_EQ(recs[0].type, log::kFullType);
  EXPECT_EQ(recs[0].payload, std::string(first_payload, 'x'));
  EXPECT_EQ(recs[1].type, log::kFullType);
  EXPECT_EQ(recs[1].payload, "second");

  // The stream must be at least one full block plus the second record's bytes.
  EXPECT_GE(dest.contents().size(),
            static_cast<size_t>(log::kBlockSize + log::kHeaderSize + 6));
}
