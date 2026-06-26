#include "mcls/FcSampleOffsets.hpp"

#include <algorithm>
#include <cstdint>
#include <set>

namespace mcls {

namespace {

uint64_t seedFromParams(const uint16_t fc_log_id,
                        const uint32_t fc_log_size,
                        const std::string& sha256_hex) {
    uint64_t seed = static_cast<uint64_t>(fc_log_id) << 32;
    seed ^= fc_log_size;
    for (std::size_t i = 0; i < sha256_hex.size() && i < 16; i += 2) {
        const char hi = sha256_hex[i];
        const char lo = (i + 1 < sha256_hex.size()) ? sha256_hex[i + 1] : '0';
        auto nybble = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') {
                return static_cast<uint8_t>(c - '0');
            }
            if (c >= 'a' && c <= 'f') {
                return static_cast<uint8_t>(10 + c - 'a');
            }
            if (c >= 'A' && c <= 'F') {
                return static_cast<uint8_t>(10 + c - 'A');
            }
            return 0;
        };
        seed ^= static_cast<uint64_t>((nybble(hi) << 4) | nybble(lo)) << (i * 2);
    }
    return seed ? seed : 1;
}

uint32_t xorshift64(uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return static_cast<uint32_t>(state);
}

void addAnchor(std::set<uint32_t>& out, const uint32_t size, const uint32_t offset) {
    const uint32_t c = static_cast<uint32_t>(kLogChunkSize);
    if (size < c) {
        return;
    }
    const uint32_t max_ofs = size - c;
    if (offset <= max_ofs) {
        out.insert(alignDownToChunk(offset));
    }
}

} // namespace

std::vector<uint32_t> buildFcSampleOffsets(const uint32_t size,
                                           const int total_samples,
                                           const uint16_t fc_log_id,
                                           const uint32_t fc_log_size,
                                           const std::string& sha256_hex) {
    const uint32_t c = static_cast<uint32_t>(kLogChunkSize);
    if (size < c || total_samples <= 0) {
        return {};
    }

    const uint32_t max_ofs = size - c;
    const uint32_t max_slot = chunkSlotIndex(max_ofs);

    std::set<uint32_t> offsets;

    addAnchor(offsets, size, 0);
    addAnchor(offsets, size, c);
    addAnchor(offsets, size, size / 4);
    addAnchor(offsets, size, size / 2);
    addAnchor(offsets, size, (3 * size) / 4);
    if (size >= 2 * c) {
        addAnchor(offsets, size, size - 2 * c);
    }
    addAnchor(offsets, size, size - c);

    const int random_want =
        std::max(0, total_samples - static_cast<int>(offsets.size()));
    uint64_t rng = seedFromParams(fc_log_id, fc_log_size, sha256_hex);

    int attempts = 0;
    while (static_cast<int>(offsets.size()) < total_samples && attempts < random_want * 20) {
        ++attempts;
        const uint32_t slot = xorshift64(rng) % (max_slot + 1);
        const uint32_t ofs = slot * c;
        offsets.insert(ofs);
    }

    std::vector<uint32_t> result(offsets.begin(), offsets.end());
    if (static_cast<int>(result.size()) > total_samples) {
        result.resize(static_cast<std::size_t>(total_samples));
    }
    return result;
}

} // namespace mcls
