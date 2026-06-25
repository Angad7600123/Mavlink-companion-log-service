#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mcls {

struct LogEntry {
    uint16_t id = 0;
    uint16_t num_logs = 0;
    uint16_t last_log_num = 0;
    uint32_t time_utc = 0;
    uint32_t size = 0;
};

struct ArchivedLogCandidate {
    std::string sha256;
    std::string probe_sha256;
    int probe_bytes = 0;
    std::string filename;
};

enum class ArchiveResult {
    SkippedDuplicate,
    Downloaded,
    Failed,
};

struct ArchiveCycleResult {
    int downloaded = 0;
    int skipped = 0;
    int failed = 0;
    bool all_archived = false;
};

} // namespace mcls
