#pragma once

#include "mcls/MavlinkLogProtocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mcls {

constexpr int kFcSampleAnchorCount = 7;

/// Build deduped FC sample re-read offsets: fixed anchors + pseudo-random chunk slots.
std::vector<uint32_t> buildFcSampleOffsets(uint32_t size,
                                           int total_samples,
                                           uint16_t fc_log_id,
                                           uint32_t fc_log_size,
                                           const std::string& sha256_hex);

} // namespace mcls
