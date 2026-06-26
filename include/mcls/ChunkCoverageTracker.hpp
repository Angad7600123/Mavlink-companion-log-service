#pragma once

#include "mcls/MavlinkLogProtocol.hpp"

#include <cstdint>
#include <vector>

namespace mcls {

/// Tracks MAVLink log chunk slots (bitmap) and optional per-slot CRC32 fingerprints.
class ChunkCoverageTracker {
public:
    explicit ChunkCoverageTracker(uint32_t total_size);

    uint32_t slotCount() const { return slot_count_; }

    /// Record bytes at offset. Returns false if overlap conflict detected.
    bool record(uint32_t offset, const uint8_t* data, uint32_t count, bool detect_overlap);

    bool allSlotsReceived() const;
    uint32_t slotsReceived() const { return slots_received_; }
    uint32_t overlapConflicts() const { return overlap_conflicts_; }

private:
    void markSlot(uint32_t slot, uint32_t fingerprint, bool detect_overlap);

    uint32_t total_size_ = 0;
    uint32_t slot_count_ = 0;
    uint32_t slots_received_ = 0;
    uint32_t overlap_conflicts_ = 0;
    std::vector<uint8_t> bitmap_;
    std::vector<uint32_t> fingerprints_;
};

} // namespace mcls
