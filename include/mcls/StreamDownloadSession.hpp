#pragma once

#include "mcls/ArchiveSummary.hpp"
#include "mcls/Config.hpp"
#include "mcls/MavlinkLogProtocol.hpp"
#include "mcls/ReceivedRanges.hpp"
#include "mcls/Sha256.hpp"
#include "mcls/Types.hpp"

#include <chrono>
#include <fstream>
#include <functional>
#include <vector>

namespace mcls {

class LogDownloader;
class Logger;

struct StreamDownloadResult {
    bool success = false;
    ArchiveFailureReason failure = ArchiveFailureReason::None;
    StreamDownloadMetrics metrics{};
    ReceivedRanges ranges{};
    std::string sha256;
    std::string probe_sha256;
    int probe_bytes = 0;
};

/// MAVLink streaming log download session (Mission Planner-style).
class StreamDownloadSession {
public:
    struct Params {
        uint16_t log_id = 0;
        uint32_t byte_offset = 0;
        uint32_t byte_count = 0;
        int probe_bytes = 0;
        bool write_file = true;
    };

    StreamDownloadSession(LogDownloader& owner,
                          Logger& logger,
                          const Config::DownloadSettings& settings,
                          Params params);

    StreamDownloadResult run(std::ofstream& file);

private:
    LogDownloader& owner_;
    Logger& logger_;
    const Config::DownloadSettings& settings_;
    Params params_;

    bool processQueuedChunks(std::ofstream& file,
                             ReceivedRanges& ranges,
                             class ChunkCoverageTracker& coverage,
                             Sha256& hasher,
                             bool& probe_finalized,
                             uint32_t& hashed_bytes,
                             std::string& probe_sha256,
                             bool& saw_short_eof,
                             StreamDownloadMetrics& metrics);

    void requestMergedGaps(const ReceivedRanges& ranges, uint32_t total_size);
    void logBenchmark(const StreamDownloadMetrics& metrics,
                      uint32_t bytes_received,
                      std::chrono::steady_clock::time_point start) const;
};

} // namespace mcls
