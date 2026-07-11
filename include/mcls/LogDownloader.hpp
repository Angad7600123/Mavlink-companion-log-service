#pragma once

#include "mcls/ArchiveSummary.hpp"
#include "mcls/Config.hpp"
#include "mcls/Database.hpp"
#include "mcls/MavlinkClient.hpp"
#include "mcls/StorageManager.hpp"
#include "mcls/Types.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mcls {

class Logger;
class StreamDownloadSession;

/// Implements MAVLink log protocol: enumerate, download, verify, erase.
class LogDownloader {
public:
    struct LogDataChunk {
        uint16_t id = 0;
        uint32_t offset = 0;
        uint32_t count = 0;
        std::vector<uint8_t> data;
    };

    LogDownloader(MavlinkClient& client,
                  StorageManager& storage,
                  Database& database,
                  const Config::DownloadSettings& settings,
                  Logger& logger);

    std::vector<LogEntry> enumerateLogs();
    void onMessage(const mavlink_message_t& msg);
    ArchiveCycleResult archiveAll(const std::vector<LogEntry>& logs);
    bool eraseFlightControllerLogs();

    void requestCancel();
    void resetCancel();
    bool cancelled() const { return cancel_requested_.load(); }

    ArchiveFailureReason lastFailureReason() const { return last_failure_reason_; }

    struct ActiveArchiveProgress {
        bool active = false;
        uint16_t log_id = 0;
        uint32_t bytes_received = 0;
        uint32_t total_bytes = 0;
        uint32_t bytes_per_sec = 0;  ///< average throughput since the current log began
    };

    /// Thread-safe snapshot of current download progress.
    /// Returns zeroed struct when no archive is running.
    ActiveArchiveProgress activeProgress() const;

    /// Called by DroneLogService after a successful enumerateLogs() to keep
    /// the companion API informed of the current FC log count between cycles.
    void updateProgressBegin(uint16_t log_id, uint32_t total_bytes);
    void updateProgressBytes(uint32_t bytes_received);
    void updateProgressEnd();

private:
    friend class StreamDownloadSession;

    enum class DedupDecision {
        DownloadFull,
        SkipDuplicate,
        DownloadAfterProbeMismatch,
    };

    MavlinkClient& client_;
    StorageManager& storage_;
    Database& database_;
    Config::DownloadSettings settings_;
    Logger& logger_;

    DedupDecision checkDedup(const LogEntry& entry, std::optional<ArchivedLogCandidate>& candidate);
    bool downloadProbeAndCompare(const LogEntry& entry, const ArchivedLogCandidate& candidate);
    ArchiveResult archiveOne(const LogEntry& entry);
    bool downloadLogData(const LogEntry& entry,
                         const std::filesystem::path& partial_path,
                         std::string& out_sha256,
                         std::string& out_probe_sha256,
                         int& out_probe_bytes,
                         std::chrono::steady_clock::time_point& out_start_time,
                         StreamDownloadMetrics& out_metrics);
    bool verifyDataFlashFile(const std::filesystem::path& path);
    bool verifyFcSampleReread(const LogEntry& entry,
                              const std::filesystem::path& path,
                              const std::string& sha256);
    bool waitForLogData(uint16_t log_id,
                        uint32_t expected_offset,
                        std::vector<uint8_t>& out,
                        std::chrono::milliseconds timeout);
    bool requestLogData(uint16_t log_id, uint32_t offset, uint32_t count);
    void requestLogList();
    void requestLogEnd();
    void abortLogTransfer(const std::string& reason);
    void clearDataChunks();
    void logArchiveSummary(const ArchivePerformanceSummary& summary) const;

    std::vector<LogDataChunk> drainLogDataChunks(uint16_t log_id);
    void clearLogDataChunks() { clearDataChunks(); }
    bool sendLogRequestData(uint16_t log_id, uint32_t offset, uint32_t count) {
        return requestLogData(log_id, offset, count);
    }
    void sendLogRequestEnd() { requestLogEnd(); }
    void waitForLogDataNotify(std::chrono::milliseconds timeout);
    /// Sleeps up to `duration`, waking early in ~100ms increments if cancelled()
    /// becomes true, so archive.cancel takes effect promptly even during an
    /// FC-busy or enumeration-retry back-off rather than waiting out the delay.
    void interruptibleSleep(std::chrono::seconds duration);
    ArchiveFailureReason classifyTransferTimeout() { return classifyTimeout(); }

    ArchiveFailureReason classifyTimeout();
    void logStall(uint16_t log_id,
                  uint32_t offset,
                  uint32_t expected,
                  uint32_t received,
                  int attempt,
                  const char* reason) const;
    void trimDataChunkQueue(std::size_t cap);

    mutable std::mutex msg_mutex_;
    mutable std::condition_variable msg_cv_;
    std::vector<LogEntry> pending_entries_;
    std::deque<LogDataChunk> data_chunks_;

    /// Zero-length LOG_DATA received (FC answering but log not servable yet).
    /// Monotonic; StreamDownloadSession snapshots it to detect the FC-busy
    /// signature and back off instead of aborting.
    std::atomic<std::uint64_t> zero_length_data_count_{0};
    std::uint64_t zeroLengthDataCount() const { return zero_length_data_count_.load(); }

    std::atomic<bool> cancel_requested_{false};
    mutable ArchiveFailureReason last_failure_reason_ = ArchiveFailureReason::None;
    std::chrono::steady_clock::time_point last_queue_overflow_log_{};
    std::size_t queue_overflow_suppressed_ = 0;

    mutable std::mutex progress_mutex_;
    ActiveArchiveProgress progress_;
    std::chrono::steady_clock::time_point progress_begin_{};
};

} // namespace mcls
