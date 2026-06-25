#pragma once

#include "mcls/Config.hpp"
#include "mcls/Database.hpp"
#include "mcls/MavlinkClient.hpp"
#include "mcls/StorageManager.hpp"
#include "mcls/Types.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <vector>

namespace mcls {

class Logger;

/// Implements MAVLink log protocol: enumerate, download, verify, erase.
class LogDownloader {
public:
    LogDownloader(MavlinkClient& client,
                  StorageManager& storage,
                  Database& database,
                  const Config::DownloadSettings& settings,
                  Logger& logger);

    std::vector<LogEntry> enumerateLogs();
    void onMessage(const mavlink_message_t& msg);
    ArchiveCycleResult archiveAll(const std::vector<LogEntry>& logs);
    bool eraseFlightControllerLogs();

private:
    struct LogDataChunk {
        uint16_t id = 0;
        uint32_t offset = 0;
        uint32_t count = 0;
        std::vector<uint8_t> data;
    };

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
                         std::chrono::steady_clock::time_point& out_start_time);
    bool downloadRange(uint16_t log_id,
                       uint32_t offset,
                       uint32_t count,
                       std::vector<uint8_t>& out);
    bool waitForLogData(uint16_t log_id,
                        uint32_t expected_offset,
                        std::vector<uint8_t>& out,
                        std::chrono::milliseconds timeout);
    void requestLogData(uint16_t log_id, uint32_t offset, uint16_t count);
    void requestLogList();
    void requestLogEnd();

    mutable std::mutex msg_mutex_;
    std::condition_variable msg_cv_;
    std::vector<LogEntry> pending_entries_;
    std::deque<LogDataChunk> data_chunks_;
};

} // namespace mcls
