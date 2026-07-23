#include "mlsm/slice.h"
#include "mlsm/status.h"

#include <gtest/gtest.h>

using namespace mlsm;

TEST(Sanity, BuildWorks) {
  EXPECT_EQ(1, 1);
}

TEST(Slice, BasicOps) {
  Slice a("hello");
  EXPECT_EQ(a.size(), 5u);
  EXPECT_FALSE(a.empty());
  EXPECT_EQ(a.ToString(), "hello");

  Slice b(std::string("hello"));
  EXPECT_EQ(a, b);

  Slice c("world");
  EXPECT_NE(a, c);
}

TEST(Status, OkAndErrors) {
  Status ok = Status::OK();
  EXPECT_TRUE(ok.ok());

  Status nf = Status::NotFound("missing key");
  EXPECT_FALSE(nf.ok());
  EXPECT_TRUE(nf.IsNotFound());
  EXPECT_EQ(nf.ToString(), "NotFound: missing key");
}