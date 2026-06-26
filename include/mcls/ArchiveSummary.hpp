#pragma once

#include <cstdint>
#include <string>

namespace mcls {

enum class FcRereadMode {
    None,
    Sample,
    Full,
};

enum class VerificationOutcome {
    Skipped,
    Ok,
    Failed,
};

struct StreamDownloadMetrics {
    uint64_t duplicate_packets = 0;
    uint64_t gap_fill_requests = 0;
    uint64_t log_data_packets = 0;
    uint64_t log_request_packets = 0;
    uint64_t retries = 0;
    uint64_t overlap_conflicts = 0;
    std::size_t queue_depth_max = 0;
    double estimated_rtt_ms = 0.0;
};

struct ArchivePerformanceSummary {
    uint16_t log_id = 0;
    uint32_t total_size = 0;
    int64_t duration_ms = 0;
    double avg_throughput_kbps = 0.0;
    StreamDownloadMetrics download{};
    VerificationOutcome parse_result = VerificationOutcome::Skipped;
    VerificationOutcome sample_reread_result = VerificationOutcome::Skipped;
    std::string sha256;
    std::string final_decision;
    std::string log_erase = "no";

    std::string formatLine() const;
};

} // namespace mcls
