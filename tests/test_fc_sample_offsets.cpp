#include "mcls/FcSampleOffsets.hpp"
#include "mcls/MavlinkLogProtocol.hpp"

#include <gtest/gtest.h>
#include <set>

TEST(FcSampleOffsetsTest, IncludesAnchorsAndDedupes) {
    const auto offsets =
        mcls::buildFcSampleOffsets(9000, 64, 3, 9000, "abc123deadbeef0123456789abcdef0123456789abcdef");
    EXPECT_GE(offsets.size(), 7U);
    EXPECT_LE(offsets.size(), 64U);
    EXPECT_EQ(offsets.front(), 0U);
    const std::set<uint32_t> unique(offsets.begin(), offsets.end());
    EXPECT_EQ(unique.size(), offsets.size());
}

TEST(FcSampleOffsetsTest, SmallLogReturnsEmpty) {
    const auto offsets = mcls::buildFcSampleOffsets(10, 64, 1, 10, "aa");
    EXPECT_TRUE(offsets.empty());
}

TEST(FcSampleOffsetsTest, DeterministicRandomComponent) {
    const auto a = mcls::buildFcSampleOffsets(100000, 64, 5, 100000, "deadbeef");
    const auto b = mcls::buildFcSampleOffsets(100000, 64, 5, 100000, "deadbeef");
    EXPECT_EQ(a, b);
}
