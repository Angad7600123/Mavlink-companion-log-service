#include "mcls/ReceivedRanges.hpp"

#include <algorithm>

namespace mcls {

void ReceivedRanges::reset() {
    ranges_.clear();
}

void ReceivedRanges::add(uint32_t offset, uint32_t count) {
    if (count == 0) {
        return;
    }
    ranges_.emplace_back(offset, offset + count);
    merge();
}

bool ReceivedRanges::complete(uint32_t total_size) const {
    return total_size == 0 || (ranges_.size() == 1 && ranges_[0].first == 0 && ranges_[0].second >= total_size);
}

std::vector<std::pair<uint32_t, uint32_t>> ReceivedRanges::gaps(uint32_t total_size) const {
    std::vector<std::pair<uint32_t, uint32_t>> result;
    uint32_t cursor = 0;
    for (const auto& [start, end] : ranges_) {
        if (start > cursor) {
            result.emplace_back(cursor, start - cursor);
        }
        cursor = std::max(cursor, end);
    }
    if (cursor < total_size) {
        result.emplace_back(cursor, total_size - cursor);
    }
    return result;
}

uint32_t ReceivedRanges::bytesReceived() const {
    uint32_t total = 0;
    for (const auto& [start, end] : ranges_) {
        total += end - start;
    }
    return total;
}

void ReceivedRanges::merge() {
    if (ranges_.empty()) {
        return;
    }
    std::sort(ranges_.begin(), ranges_.end());
    std::vector<std::pair<uint32_t, uint32_t>> merged;
    merged.push_back(ranges_.front());
    for (std::size_t i = 1; i < ranges_.size(); ++i) {
        auto& last = merged.back();
        if (ranges_[i].first <= last.second) {
            last.second = std::max(last.second, ranges_[i].second);
        } else {
            merged.push_back(ranges_[i]);
        }
    }
    ranges_ = std::move(merged);
}

} // namespace mcls
