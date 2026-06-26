#include "mcls/ChunkCoverageTracker.hpp"
#include "mcls/Crc32.hpp"

#include <algorithm>
#include <cstring>

namespace mcls {

namespace {

constexpr uint32_t kEmptyFingerprint = 0;

bool bitmapTest(const std::vector<uint8_t>& bitmap, uint32_t slot) {
    const uint32_t byte = slot / 8;
    const uint8_t bit = static_cast<uint8_t>(1U << (slot % 8));
    return byte < bitmap.size() && (bitmap[byte] & bit) != 0;
}

void bitmapSet(std::vector<uint8_t>& bitmap, uint32_t slot) {
    const uint32_t byte = slot / 8;
    const uint8_t bit = static_cast<uint8_t>(1U << (slot % 8));
    if (byte < bitmap.size()) {
        bitmap[byte] |= bit;
    }
}

uint32_t fingerprintForSlot(const uint8_t* data, uint32_t count) {
    uint8_t padded[kLogChunkSize]{};
    const uint32_t take = std::min<uint32_t>(count, static_cast<uint32_t>(kLogChunkSize));
    if (take > 0) {
        std::memcpy(padded, data, take);
    }
    const uint32_t fp = crc32(padded, kLogChunkSize);
    return fp == kEmptyFingerprint ? 1U : fp;
}

} // namespace

ChunkCoverageTracker::ChunkCoverageTracker(const uint32_t total_size)
    : total_size_(total_size),
      slot_count_(chunkCountForSize(total_size)),
      bitmap_((slot_count_ + 7) / 8, 0),
      fingerprints_(slot_count_, kEmptyFingerprint) {}

bool ChunkCoverageTracker::record(const uint32_t offset,
                                  const uint8_t* data,
                                  const uint32_t count,
                                  const bool detect_overlap) {
    if (count == 0 || data == nullptr) {
        return true;
    }

    const uint32_t c = static_cast<uint32_t>(kLogChunkSize);
    if (offset % c != 0) {
        return true;
    }

    const uint32_t slot = chunkSlotIndex(offset);
    if (slot >= slot_count_) {
        return true;
    }

    markSlot(slot, fingerprintForSlot(data, count), detect_overlap);
    return overlap_conflicts_ == 0 || !detect_overlap;
}

void ChunkCoverageTracker::markSlot(const uint32_t slot,
                                    const uint32_t fingerprint,
                                    const bool detect_overlap) {
    if (slot >= slot_count_) {
        return;
    }

    const bool was_set = bitmapTest(bitmap_, slot);
    if (was_set && detect_overlap) {
        if (fingerprints_[slot] != fingerprint) {
            ++overlap_conflicts_;
            return;
        }
        return;
    }

    if (!was_set) {
        bitmapSet(bitmap_, slot);
        fingerprints_[slot] = fingerprint;
        ++slots_received_;
    } else if (fingerprints_[slot] != fingerprint && detect_overlap) {
        ++overlap_conflicts_;
    }
}

bool ChunkCoverageTracker::allSlotsReceived() const {
    if (slot_count_ == 0) {
        return total_size_ == 0;
    }
    return slots_received_ >= slot_count_;
}

} // namespace mcls
