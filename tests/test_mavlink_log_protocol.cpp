#include "mcls/MavlinkLogProtocol.hpp"

#include <gtest/gtest.h>

TEST(MavlinkLogProtocolTest, ChunkSizeMatchesMavlink) {
    EXPECT_EQ(mcls::kLogChunkSize, static_cast<std::size_t>(MAVLINK_MSG_LOG_DATA_FIELD_DATA_LEN));
}

TEST(MavlinkLogProtocolTest, AlignmentHelpers) {
    EXPECT_EQ(mcls::alignDownToChunk(95), 90U);
    EXPECT_EQ(mcls::chunkSlotIndex(180), 2U);
    EXPECT_EQ(mcls::chunkCountForSize(0), 0U);
    EXPECT_EQ(mcls::chunkCountForSize(1), 1U);
    EXPECT_EQ(mcls::chunkCountForSize(90), 1U);
    EXPECT_EQ(mcls::chunkCountForSize(91), 2U);
}
