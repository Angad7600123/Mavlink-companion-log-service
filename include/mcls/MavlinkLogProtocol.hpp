#pragma once

#include <ardupilotmega/mavlink.h>

#include <cstddef>
#include <cstdint>

namespace mcls {

constexpr std::size_t kLogChunkSize = MAVLINK_MSG_LOG_DATA_FIELD_DATA_LEN;
constexpr uint32_t kLogStreamRequestCount = 0xFFFFFFFFU;

inline uint32_t alignDownToChunk(uint32_t offset) {
    const auto c = static_cast<uint32_t>(kLogChunkSize);
    return offset - (offset % c);
}

inline uint32_t chunkSlotIndex(uint32_t offset) {
    return offset / static_cast<uint32_t>(kLogChunkSize);
}

inline uint32_t chunkCountForSize(uint32_t size) {
    if (size == 0) {
        return 0;
    }
    return (size + static_cast<uint32_t>(kLogChunkSize) - 1) /
           static_cast<uint32_t>(kLogChunkSize);
}

} // namespace mcls
