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

/// Why a log archive attempt ended without success.
enum class ArchiveFailureReason {
    None,
    LogDataTimeout,      ///< No LOG_DATA for requested offset, but link still alive.
    EmptyPayload,        ///< Matching LOG_DATA arrived with no usable bytes.
    NoProgress,          ///< Repeated attempts advanced the offset by zero.
    IncompleteDownload,  ///< Retries exhausted with gaps remaining.
    VerificationFailed,  ///< Received byte count did not match LOG_ENTRY size.
    StorageError,        ///< Local file/storage failure.
    Cancelled,           ///< Aborted by arm/disarm/shutdown.
    TransportSendFailed, ///< sendMessage / sendto failed.
    TransportClosed,     ///< Transport reported not connected.
    LinkTimeout,         ///< No inbound MAVLink within heartbeat window during transfer.
};

inline const char* toString(ArchiveFailureReason reason) {
    switch (reason) {
    case ArchiveFailureReason::None: return "none";
    case ArchiveFailureReason::LogDataTimeout: return "log_data_timeout";
    case ArchiveFailureReason::EmptyPayload: return "empty_payload";
    case ArchiveFailureReason::NoProgress: return "no_progress";
    case ArchiveFailureReason::IncompleteDownload: return "incomplete_download";
    case ArchiveFailureReason::VerificationFailed: return "verification_failed";
    case ArchiveFailureReason::StorageError: return "storage_error";
    case ArchiveFailureReason::Cancelled: return "cancelled";
    case ArchiveFailureReason::TransportSendFailed: return "transport_send_failed";
    case ArchiveFailureReason::TransportClosed: return "transport_closed";
    case ArchiveFailureReason::LinkTimeout: return "link_timeout";
    }
    return "unknown";
}

/// True for reasons that indicate the transport/link itself is suspect.
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
