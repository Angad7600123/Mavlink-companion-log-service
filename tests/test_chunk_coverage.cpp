#include "mcls/ChunkCoverageTracker.hpp"
#include "mcls/MavlinkLogProtocol.hpp"

#include <gtest/gtest.h>

TEST(ChunkCoverageTrackerTest, DetectsOverlapConflict) {
    mcls::ChunkCoverageTracker tracker(180);
    uint8_t a[mcls::kLogChunkSize]{};
    uint8_t b[mcls::kLogChunkSize]{};
    a[0] = 1;
    b[0] = 2;
    EXPECT_TRUE(tracker.record(0, a, static_cast<uint32_t>(mcls::kLogChunkSize), true));
    EXPECT_FALSE(tracker.record(0, b, static_cast<uint32_t>(mcls::kLogChunkSize), true));
    EXPECT_EQ(tracker.overlapConflicts(), 1U);
}

TEST(ChunkCoverageTrackerTest, AllSlotsReceived) {
    mcls::ChunkCoverageTracker tracker(180);
    uint8_t chunk[mcls::kLogChunkSize]{};
    EXPECT_TRUE(tracker.record(0, chunk, static_cast<uint32_t>(mcls::kLogChunkSize), true));
    EXPECT_TRUE(tracker.record(static_cast<uint32_t>(mcls::kLogChunkSize), chunk,
                               static_cast<uint32_t>(mcls::kLogChunkSize), true));
    EXPECT_TRUE(tracker.allSlotsReceived());
}
