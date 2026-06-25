#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace mcls {

/// Tracks received byte intervals for in-session LOG_DATA gap detection.
class ReceivedRanges {
public:
    void reset();
    void add(uint32_t offset, uint32_t count);
    bool complete(uint32_t total_size) const;
    std::vector<std::pair<uint32_t, uint32_t>> gaps(uint32_t total_size) const;
    uint32_t bytesReceived() const;

private:
    std::vector<std::pair<uint32_t, uint32_t>> ranges_;
    void merge();
};

} // namespace mcls
