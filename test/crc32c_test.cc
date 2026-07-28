#include "mlsm/crc32c.h"

#include <gtest/gtest.h>

#include <string>

using namespace mlsm;

TEST(Crc32c, EmptyIsZero) {
  EXPECT_EQ(crc32c::Value("", 0), 0u);
}

TEST(Crc32c, CanonicalCheckValue) {
  // The standard CRC check string. CRC32C("123456789") == 0xE3069283.
  const std::string s = "123456789";
  EXPECT_EQ(crc32c::Value(s.data(), s.size()), 0xE3069283u);
}

TEST(Crc32c, ExtendChainsLikeConcatenation) {
  // Splitting the input and chaining via Extend must equal the whole.
  const std::string whole = "hello world";
  uint32_t once = crc32c::Value(whole.data(), whole.size());

  uint32_t chained = crc32c::Value("hello ", 6);
  chained = crc32c::Extend(chained, "world", 5);

  EXPECT_EQ(once, chained);
}

TEST(Crc32c, ValueIsExtendFromZero) {
  const std::string s = "the quick brown fox";
  EXPECT_EQ(crc32c::Value(s.data(), s.size()),
            crc32c::Extend(0, s.data(), s.size()));
}

TEST(Crc32c, DifferentDataDiffersFromEmpty) {
  EXPECT_NE(crc32c::Value("a", 1), crc32c::Value("", 0));
  EXPECT_NE(crc32c::Value("a", 1), crc32c::Value("b", 1));
}

TEST(Crc32c, MaskRoundTrips) {
  for (uint32_t crc : {0u, 1u, 0xE3069283u, 0xffffffffu, 0x12345678u}) {
    EXPECT_EQ(crc, crc32c::Unmask(crc32c::Mask(crc)));
  }
}

TEST(Crc32c, MaskChangesTheValue) {
  // Masking must actually transform the crc, otherwise it's pointless.
  uint32_t crc = crc32c::Value("some payload", 12);
  EXPECT_NE(crc, crc32c::Mask(crc));
}
