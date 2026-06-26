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
    Cancelled,
};

enum class ArchiveFailureReason {
    None,
    LogDataTimeout,
    EmptyPayload,
    NoProgress,
    IncompleteDownload,
    VerificationFailed,
    ParseFailed,
    RereadMismatch,
    OverlapConflict,
    StorageError,
    Cancelled,
    TransportSendFailed,
    TransportClosed,
    LinkTimeout,
};

inline const char* toString(ArchiveFailureReason reason) {
    switch (reason) {
    case ArchiveFailureReason::None: return "none";
    case ArchiveFailureReason::LogDataTimeout: return "log_data_timeout";
    case ArchiveFailureReason::EmptyPayload: return "empty_payload";
    case ArchiveFailureReason::NoProgress: return "no_progress";
    case ArchiveFailureReason::IncompleteDownload: return "incomplete_download";
    case ArchiveFailureReason::VerificationFailed: return "verification_failed";
    case ArchiveFailureReason::ParseFailed: return "parse_failed";
    case ArchiveFailureReason::RereadMismatch: return "reread_mismatch";
    case ArchiveFailureReason::OverlapConflict: return "overlap_conflict";
    case ArchiveFailureReason::StorageError: return "storage_error";
    case ArchiveFailureReason::Cancelled: return "cancelled";
    case ArchiveFailureReason::TransportSendFailed: return "transport_send_failed";
    case ArchiveFailureReason::TransportClosed: return "transport_closed";
    case ArchiveFailureReason::LinkTimeout: return "link_timeout";
    }
    return "unknown";
}

inline bool isTransportFailure(ArchiveFailureReason reason) {
    return reason == ArchiveFailureReason::TransportSendFailed ||
           reason == ArchiveFailureReason::TransportClosed ||
           reason == ArchiveFailureReason::LinkTimeout;
}

struct ArchiveCycleResult {
    int downloaded = 0;
    int skipped = 0;
    int failed = 0;
    int cancelled = 0;
    bool all_archived = false;
    ArchiveFailureReason last_failure_reason = ArchiveFailureReason::None;
};

} // namespace mcls
