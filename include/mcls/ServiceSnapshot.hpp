#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mcls {

/// Tier 1 status snapshot: fixed-size summary suitable for a single WFB datagram.
/// Never includes FC log entry lists, SHA-256 values, catalog rows, or filenames.
struct ServiceSnapshot {
    // service
    std::string state;     ///< DroneLogService::State as string (e.g. "wait_arm")
    std::string version;   ///< e.g. "1.0.0"

    // link
    bool transport_connected = false;
    bool heartbeat_fresh = false;

    // vehicle
    bool vehicle_detected = false;
    bool vehicle_armed = false;

    // fc_logs — count only; full entry list via fc.logs op
    int fc_logs_count = 0;
    bool fc_logs_stale = true;  ///< true until first enumeration this cycle

    // archive progress
    bool archive_active = false;
    uint16_t archive_current_log_id = 0;
    uint32_t archive_progress_bytes = 0;
    uint32_t archive_progress_total_bytes = 0;

    // last cycle summary (compact ints only)
    int last_cycle_downloaded = 0;
    int last_cycle_skipped = 0;
    int last_cycle_failed = 0;
    int last_cycle_cancelled = 0;
    bool last_cycle_all_archived = false;

    // storage
    uint64_t storage_used_bytes = 0;
    uint64_t storage_limit_bytes = 0;
    int64_t storage_archived_count = 0;
};

/// Tier 2 FC log entry (id + size only — no timestamps in v1).
struct FcLogEntry {
    uint16_t id = 0;
    uint32_t size = 0;
};

/// Result of a paginated fc.logs request.
struct FcLogsPage {
    int count = 0;       ///< total FC logs known (from cache)
    int offset = 0;
    std::vector<FcLogEntry> entries;
    bool truncated = false;
    bool stale = true;
};

} // namespace mcls
