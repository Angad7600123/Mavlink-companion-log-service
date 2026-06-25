#include "mcls/ReceivedRanges.hpp"

#include <gtest/gtest.h>

TEST(ReceivedRangesTest, DetectsGapsAndCompletion) {
    mcls::ReceivedRanges ranges;
    ranges.add(0, 360);
    ranges.add(360, 360);
    ranges.add(810, 90);

    EXPECT_FALSE(ranges.complete(900));
    const auto gaps = ranges.gaps(900);
    ASSERT_EQ(gaps.size(), 1U);
    EXPECT_EQ(gaps[0].first, 720U);
    EXPECT_EQ(gaps[0].second, 90U);

    ranges.add(720, 90);
    EXPECT_TRUE(ranges.complete(900));
    EXPECT_EQ(ranges.bytesReceived(), 900U);
}
