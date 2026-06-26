#include "mcls/LogDownloader.hpp"
#include "mcls/Logger.hpp"
#include "mcls/ReceivedRanges.hpp"
#include "mcls/Sha256.hpp"

#include <ardupilotmega/mavlink.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>
#include <thread>
#include <utility>

namespace mcls {

namespace {

constexpr int kLogDataMaxBytes = 90;
constexpr auto kEnumerationIdle = std::chrono::milliseconds(1000);

int64_t unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// Run a cleanup action when leaving scope unless dismissed.
template <class F>
class ScopeExit {
public:
    explicit ScopeExit(F f) : f_(std::move(f)) {}
    ~ScopeExit() {
        if (active_) {
            f_();
        }
    }
    ScopeExit(ScopeExit&& other) noexcept : f_(std::move(other.f_)), active_(other.active_) {
        other.active_ = false;
    }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    void dismiss() { active_ = false; }

private:
    F f_;
    bool active_ = true;
};

template <class F>
ScopeExit<F> makeScopeExit(F f) {
    return ScopeExit<F>(std::move(f));
}

} // namespace

LogDownloader::LogDownloader(MavlinkClient& client,
                             StorageManager& storage,
                             Database& database,
                             const Config::DownloadSettings& settings,
                             Logger& logger)
    : client_(client),
      storage_(storage),
      database_(database),
      settings_(settings),
      logger_(logger) {}

void LogDownloader::requestCancel() {
    cancel_requested_.store(true);
    msg_cv_.notify_all();
}

void LogDownloader::resetCancel() {
    cancel_requested_.store(false);
}

void LogDownloader::clearDataChunks() {
    std::lock_guard lock(msg_mutex_);
    data_chunks_.clear();
}

void LogDownloader::abortLogTransfer(const std::string& reason) {
    logger_.warn("Aborting FC log transfer: " + reason);
    clearDataChunks();
    requestLogEnd();
}

ArchiveFailureReason LogDownloader::classifyTimeout() const {
    if (!client_.isConnected()) {
        return ArchiveFailureReason::TransportClosed;
    }
    if (!client_.heartbeatFresh()) {
        return ArchiveFailureReason::LinkTimeout;
    }
    return ArchiveFailureReason::LogDataTimeout;
}

void LogDownloader::logStall(uint16_t log_id,
                             uint32_t offset,
                             uint32_t expected,
                             uint32_t received,
                             int attempt,
                             const char* reason) const {
    logger_.warn("LOG_DATA stall: log=" + std::to_string(log_id) +
                 " offset=" + std::to_string(offset) +
                 " expected=" + std::to_string(expected) + "B" +
                 " received=" + std::to_string(received) + "B" +
                 " attempt=" + std::to_string(attempt + 1) + "/" +
                 std::to_string(settings_.retry_count + 1) + " reason=" + reason);
}

void LogDownloader::onMessage(const mavlink_message_t& msg) {
    std::lock_guard lock(msg_mutex_);

    if (msg.msgid == MAVLINK_MSG_ID_LOG_ENTRY) {
        mavlink_log_entry_t entry{};
        mavlink_msg_log_entry_decode(&msg, &entry);

        LogEntry log;
        log.id = entry.id;
        log.num_logs = entry.num_logs;
        log.last_log_num = entry.last_log_num;
        log.time_utc = entry.time_utc;
        log.size = entry.size;
        pending_entries_.push_back(log);
        msg_cv_.notify_all();
    } else if (msg.msgid == MAVLINK_MSG_ID_LOG_DATA) {
        mavlink_log_data_t data{};
        mavlink_msg_log_data_decode(&msg, &data);

        if (data.count == 0) {
            logger_.warn("Ignoring zero-length LOG_DATA (log=" + std::to_string(data.id) +
                         ", offset=" + std::to_string(data.ofs) + ")");
            return;
        }

        LogDataChunk chunk;
        chunk.id = data.id;
        chunk.offset = data.ofs;
        chunk.count = data.count;
        chunk.data.assign(data.data, data.data + data.count);

        if (chunk.data.empty()) {
            logger_.warn("Ignoring empty LOG_DATA payload (log=" + std::to_string(data.id) +
                         ", offset=" + std::to_string(data.ofs) + ")");
            return;
        }

        data_chunks_.push_back(std::move(chunk));
        const std::size_t cap =
            settings_.max_queued_log_data > 0
                ? static_cast<std::size_t>(settings_.max_queued_log_data)
                : 256;
        while (data_chunks_.size() > cap) {
            data_chunks_.pop_front();
        }
        msg_cv_.notify_all();
    }
}

void LogDownloader::requestLogList() {
    mavlink_message_t msg{};
    mavlink_msg_log_request_list_pack(
        255, 190, &msg, client_.targetSystem(), client_.targetComponent(), 0, 0xFFFF);
    client_.sendMessage(msg);
}

bool LogDownloader::requestLogData(uint16_t log_id, uint32_t offset, uint16_t count) {
    mavlink_message_t msg{};
    mavlink_msg_log_request_data_pack(255, 190, &msg, client_.targetSystem(),
                                      client_.targetComponent(), log_id, offset, count);
    return client_.sendMessage(msg);
}

void LogDownloader::requestLogEnd() {
    mavlink_message_t msg{};
    mavlink_msg_log_request_end_pack(255, 190, &msg, client_.targetSystem(),
                                     client_.targetComponent());
    client_.sendMessage(msg);
}

std::vector<LogEntry> LogDownloader::enumerateLogs() {
    {
        std::lock_guard lock(msg_mutex_);
        pending_entries_.clear();
    }

    logger_.info("Enumerating logs...");
    requestLogList();

    auto last_entry = std::chrono::steady_clock::now();
    const auto deadline = last_entry + std::chrono::seconds(15);

    while (std::chrono::steady_clock::now() < deadline) {
        if (cancelled()) {
            logger_.warn("Enumeration cancelled");
            break;
        }
        std::unique_lock lock(msg_mutex_);
        msg_cv_.wait_for(lock, std::chrono::milliseconds(200));
        if (!pending_entries_.empty()) {
            last_entry = std::chrono::steady_clock::now();
        } else if (std::chrono::steady_clock::now() - last_entry >= kEnumerationIdle) {
            break;
        }
    }

    std::vector<LogEntry> entries;
    {
        std::lock_guard lock(msg_mutex_);
        entries = pending_entries_;
    }

    std::sort(entries.begin(), entries.end(),
              [](const LogEntry& a, const LogEntry& b) { return a.id < b.id; });
    entries.erase(std::unique(entries.begin(), entries.end(),
                              [](const LogEntry& a, const LogEntry& b) { return a.id == b.id; }),
                  entries.end());

    logger_.info("Found " + std::to_string(entries.size()) + " log(s) on flight controller");
    requestLogEnd();
    return entries;
}

LogDownloader::DedupDecision LogDownloader::checkDedup(
    const LogEntry& entry,
    std::optional<ArchivedLogCandidate>& candidate) {
    candidate = database_.findCandidate(entry.id, entry.size);
    if (!candidate) {
        return DedupDecision::DownloadFull;
    }
    return DedupDecision::DownloadAfterProbeMismatch;
}

bool LogDownloader::downloadProbeAndCompare(const LogEntry& entry,
                                            const ArchivedLogCandidate& candidate) {
    const uint32_t probe_n =
        static_cast<uint32_t>(std::min<int>(settings_.probe_bytes, static_cast<int>(entry.size)));
    if (probe_n > entry.size) {
        logger_.error("Invalid probe size for log " + std::to_string(entry.id));
        return false;
    }

    std::vector<uint8_t> probe_data;
    if (!downloadRange(entry.id, 0, probe_n, probe_data)) {
        logger_.warn("Probe download failed for log " + std::to_string(entry.id));
        return false;
    }

    const std::string probe_hash = Sha256::hashHex(probe_data.data(), probe_data.size());
    if (probe_hash == candidate.probe_sha256 &&
        static_cast<int>(probe_n) == candidate.probe_bytes) {
        logger_.info("Log " + std::to_string(entry.id) +
                     " confirmed duplicate via probe hash, skipping");
        return true;
    }

    logger_.info("Log " + std::to_string(entry.id) +
                 " probe hash mismatch, downloading full log");
    return false;
}

bool LogDownloader::waitForLogData(uint16_t log_id,
                                   uint32_t expected_offset,
                                   std::vector<uint8_t>& out,
                                   std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancelled()) {
            return false;
        }
        std::unique_lock lock(msg_mutex_);
        msg_cv_.wait_for(lock, std::chrono::milliseconds(50));

        for (auto it = data_chunks_.begin(); it != data_chunks_.end(); ++it) {
            if (it->id != log_id || it->offset != expected_offset) {
                continue;
            }

            if (it->data.empty()) {
                logger_.warn("Discarding zero-length LOG_DATA at offset " +
                             std::to_string(expected_offset));
                data_chunks_.erase(it);
                break;
            }

            out = std::move(it->data);
            data_chunks_.erase(it);
            return true;
        }
    }
    return false;
}

bool LogDownloader::downloadRange(uint16_t log_id,
                                  uint32_t offset,
                                  uint32_t count,
                                  std::vector<uint8_t>& out) {
    out.assign(count, 0);
    if (count == 0) {
        return true;
    }

    uint32_t received = 0;
    int no_progress_attempts = 0;
    for (int attempt = 0; attempt <= settings_.retry_count && received < count; ++attempt) {
        if (cancelled()) {
            last_failure_reason_ = ArchiveFailureReason::Cancelled;
            return false;
        }

        const uint32_t before = received;
        while (received < count) {
            if (cancelled()) {
                last_failure_reason_ = ArchiveFailureReason::Cancelled;
                return false;
            }

            const uint32_t request_count =
                std::min<uint32_t>(count - received, kLogDataMaxBytes);
            const uint32_t request_offset = offset + received;

            if (!requestLogData(log_id, request_offset, static_cast<uint16_t>(request_count))) {
                last_failure_reason_ = ArchiveFailureReason::TransportSendFailed;
                logStall(log_id, request_offset, request_count, 0, attempt, "send_failed");
                break;
            }

            std::vector<uint8_t> chunk;
            if (!waitForLogData(log_id, request_offset, chunk,
                                std::chrono::seconds(settings_.download_timeout_sec))) {
                if (cancelled()) {
                    last_failure_reason_ = ArchiveFailureReason::Cancelled;
                    return false;
                }
                last_failure_reason_ = classifyTimeout();
                logStall(log_id, request_offset, request_count, 0, attempt,
                         toString(last_failure_reason_));
                database_.incrementStat("retries");
                break;
            }

            if (chunk.empty()) {
                last_failure_reason_ = ArchiveFailureReason::EmptyPayload;
                logStall(log_id, request_offset, request_count, 0, attempt, "empty_payload");
                database_.incrementStat("retries");
                break;
            }

            if (chunk.size() > count - received) {
                chunk.resize(count - received);
            }
            std::copy(chunk.begin(), chunk.end(), out.begin() + received);
            received += static_cast<uint32_t>(chunk.size());
        }

        if (received >= count) {
            return true;
        }

        if (received <= before) {
            if (++no_progress_attempts >= settings_.stall_abort_attempts) {
                if (last_failure_reason_ == ArchiveFailureReason::None) {
                    last_failure_reason_ = ArchiveFailureReason::NoProgress;
                }
                logger_.warn("No forward progress for probe of log " + std::to_string(log_id) +
                             " after " + std::to_string(no_progress_attempts) + " attempts");
                return false;
            }
        } else {
            no_progress_attempts = 0;
        }

        if (attempt < settings_.retry_count) {
            clearDataChunks();
            requestLogEnd();
            std::this_thread::sleep_for(std::chrono::seconds(settings_.retry_delay_sec));
        }
    }

    if (received < count && last_failure_reason_ == ArchiveFailureReason::None) {
        last_failure_reason_ = ArchiveFailureReason::IncompleteDownload;
    }
    return received >= count;
}

bool LogDownloader::downloadLogData(const LogEntry& entry,
                                    const std::filesystem::path& partial_path,
                                    std::string& out_sha256,
                                    std::string& out_probe_sha256,
                                    int& out_probe_bytes,
                                    std::chrono::steady_clock::time_point& out_start_time) {
    out_start_time = std::chrono::steady_clock::now();
    last_failure_reason_ = ArchiveFailureReason::None;

    const uint32_t probe_n =
        static_cast<uint32_t>(std::min<int>(settings_.probe_bytes, static_cast<int>(entry.size)));
    out_probe_bytes = static_cast<int>(probe_n);

    clearDataChunks();

    std::ofstream file(partial_path, std::ios::binary | std::ios::trunc);
    if (!file) {
        logger_.error("Failed to open partial file: " + partial_path.string());
        last_failure_reason_ = ArchiveFailureReason::StorageError;
        return false;
    }

    ReceivedRanges ranges;
    Sha256 hasher;
    bool probe_finalized = false;
    uint32_t hashed_bytes = 0;
    uint32_t last_progress_bytes = 0;
    int no_progress_attempts = 0;

    auto feedHasher = [&](const uint8_t* data, std::size_t len) {
        if (len == 0) {
            return;
        }
        if (!probe_finalized && hashed_bytes + len >= probe_n) {
            const uint32_t probe_take = probe_n - hashed_bytes;
            if (probe_take > 0) {
                hasher.update(data, probe_take);
            }
            out_probe_sha256 = hasher.clone().finalizeHex();
            probe_finalized = true;
            if (probe_take < len) {
                hasher.update(data + probe_take, len - probe_take);
            }
        } else {
            hasher.update(data, len);
        }
        hashed_bytes += static_cast<uint32_t>(len);
    };

    for (int attempt = 0; attempt <= settings_.retry_count; ++attempt) {
        if (cancelled()) {
            last_failure_reason_ = ArchiveFailureReason::Cancelled;
            return false;
        }

        const uint32_t before = ranges.bytesReceived();
        const auto gaps = ranges.complete(entry.size)
                              ? std::vector<std::pair<uint32_t, uint32_t>>{}
                              : (ranges.bytesReceived() == 0 && entry.size > 0
                                     ? std::vector<std::pair<uint32_t, uint32_t>>{{0, entry.size}}
                                     : ranges.gaps(entry.size));

        bool gap_failed = false;
        for (const auto& [gap_offset, gap_count] : gaps) {
            uint32_t gap_remaining = gap_count;
            uint32_t gap_current = gap_offset;
            while (gap_remaining > 0) {
                if (cancelled()) {
                    last_failure_reason_ = ArchiveFailureReason::Cancelled;
                    return false;
                }

                const uint16_t request_count =
                    static_cast<uint16_t>(std::min<uint32_t>(gap_remaining, kLogDataMaxBytes));

                if (!requestLogData(entry.id, gap_current, request_count)) {
                    last_failure_reason_ = ArchiveFailureReason::TransportSendFailed;
                    logStall(entry.id, gap_current, request_count, 0, attempt, "send_failed");
                    gap_failed = true;
                    break;
                }

                std::vector<uint8_t> chunk;
                if (!waitForLogData(entry.id, gap_current, chunk,
                                    std::chrono::seconds(settings_.download_timeout_sec))) {
                    if (cancelled()) {
                        last_failure_reason_ = ArchiveFailureReason::Cancelled;
                        return false;
                    }
                    last_failure_reason_ = classifyTimeout();
                    logStall(entry.id, gap_current, request_count, 0, attempt,
                             toString(last_failure_reason_));
                    database_.incrementStat("retries");
                    gap_failed = true;
                    break;
                }

                if (chunk.empty()) {
                    last_failure_reason_ = ArchiveFailureReason::EmptyPayload;
                    logStall(entry.id, gap_current, request_count, 0, attempt, "empty_payload");
                    database_.incrementStat("retries");
                    gap_failed = true;
                    break;
                }

                file.seekp(static_cast<std::streamoff>(gap_current));
                file.write(reinterpret_cast<const char*>(chunk.data()),
                           static_cast<std::streamsize>(chunk.size()));
                feedHasher(chunk.data(), chunk.size());
                ranges.add(gap_current, static_cast<uint32_t>(chunk.size()));

                const uint32_t received_total = ranges.bytesReceived();
                if (received_total - last_progress_bytes >= 65536 ||
                    (received_total == entry.size && entry.size > 0)) {
                    logger_.info("Log " + std::to_string(entry.id) + " progress: " +
                                 std::to_string(received_total) + "/" +
                                 std::to_string(entry.size) + " bytes");
                    last_progress_bytes = received_total;
                }

                gap_current += static_cast<uint32_t>(chunk.size());
                gap_remaining -= static_cast<uint32_t>(chunk.size());
            }
            if (gap_failed) {
                break;
            }
        }

        if (ranges.complete(entry.size)) {
            last_failure_reason_ = ArchiveFailureReason::None;
            break;
        }

        if (ranges.bytesReceived() <= before) {
            if (++no_progress_attempts >= settings_.stall_abort_attempts) {
                if (last_failure_reason_ == ArchiveFailureReason::None) {
                    last_failure_reason_ = ArchiveFailureReason::NoProgress;
                }
                logger_.warn("No forward progress for log " + std::to_string(entry.id) +
                             " after " + std::to_string(no_progress_attempts) +
                             " attempts; aborting (reason=" + toString(last_failure_reason_) + ")");
                abortLogTransfer("no forward progress for log " + std::to_string(entry.id));
                return false;
            }
        } else {
            no_progress_attempts = 0;
        }

        if (attempt < settings_.retry_count) {
            clearDataChunks();
            requestLogEnd();
            std::this_thread::sleep_for(std::chrono::seconds(settings_.retry_delay_sec));
        }
    }

    file.flush();

    if (!ranges.complete(entry.size)) {
        if (last_failure_reason_ == ArchiveFailureReason::None) {
            last_failure_reason_ = ArchiveFailureReason::IncompleteDownload;
        }
        logger_.error("Incomplete download for log " + std::to_string(entry.id) +
                      " (reason=" + toString(last_failure_reason_) + ")");
        return false;
    }

    if (settings_.verify_after_download && ranges.bytesReceived() != entry.size) {
        last_failure_reason_ = ArchiveFailureReason::VerificationFailed;
        logger_.error("Verification failed: incomplete ranges for log " +
                      std::to_string(entry.id));
        return false;
    }

    if (probe_n == 0) {
        out_probe_sha256 = Sha256::hashHex(nullptr, 0);
        out_probe_bytes = 0;
    } else if (!probe_finalized) {
        out_probe_sha256 = hasher.clone().finalizeHex();
    }

    out_sha256 = hasher.finalizeHex();
    last_failure_reason_ = ArchiveFailureReason::None;
    return true;
}

ArchiveResult LogDownloader::archiveOne(const LogEntry& entry) {
    std::optional<ArchivedLogCandidate> candidate;
    const auto decision = checkDedup(entry, candidate);

    if (decision == DedupDecision::DownloadAfterProbeMismatch && candidate) {
        if (downloadProbeAndCompare(entry, *candidate)) {
            database_.incrementStat("flights_skipped");
            return ArchiveResult::SkippedDuplicate;
        }
        if (cancelled()) {
            return ArchiveResult::Cancelled;
        }
    }

    if (cancelled()) {
        return ArchiveResult::Cancelled;
    }

    logger_.info("Downloading log " + std::to_string(entry.id) + " (" +
                 std::to_string(entry.size) + " bytes)");

    const auto partial = storage_.beginPartialFile();
    std::string sha256;
    std::string probe_sha256;
    int probe_bytes = 0;
    std::chrono::steady_clock::time_point download_start;

    if (!downloadLogData(entry, partial, sha256, probe_sha256, probe_bytes, download_start)) {
        std::error_code ec;
        std::filesystem::remove(partial, ec);
        if (last_failure_reason_ == ArchiveFailureReason::Cancelled || cancelled()) {
            logger_.warn("Download cancelled for log " + std::to_string(entry.id));
            database_.incrementStat("archive_cancellations");
            return ArchiveResult::Cancelled;
        }
        logger_.error("Download failed for log " + std::to_string(entry.id) +
                      " (reason=" + toString(last_failure_reason_) + ")");
        database_.incrementStat("archive_failures");
        return ArchiveResult::Failed;
    }

    logger_.info("Downloaded " + std::to_string(entry.size) + " bytes for log " +
                 std::to_string(entry.id));

    const auto finalized = storage_.finalizePartial(partial, download_start);
    if (!finalized) {
        last_failure_reason_ = ArchiveFailureReason::StorageError;
        database_.incrementStat("archive_failures");
        return ArchiveResult::Failed;
    }

    logger_.info("Verification successful for log " + std::to_string(entry.id));

    database_.insertArchivedLog(sha256, entry.id, entry.size, entry.time_utc, probe_sha256,
                                probe_bytes, unixNow(), finalized->archive_duration_ms,
                                finalized->final_path.string());

    database_.incrementStat("flights_archived");
    database_.incrementStat("bytes_downloaded", static_cast<int64_t>(entry.size));
    database_.setStat("last_archive_time", unixNow());

    return ArchiveResult::Downloaded;
}

ArchiveCycleResult LogDownloader::archiveAll(const std::vector<LogEntry>& logs) {
    ArchiveCycleResult result;
    result.last_failure_reason = ArchiveFailureReason::None;

    // Always release the FC log-transfer session when the cycle ends, no matter how.
    auto session_guard = makeScopeExit([this] {
        clearDataChunks();
        requestLogEnd();
    });

    bool any_failed = false;
    for (const auto& entry : logs) {
        if (cancelled()) {
            logger_.warn("Archive cancelled before log " + std::to_string(entry.id));
            ++result.cancelled;
            result.last_failure_reason = ArchiveFailureReason::Cancelled;
            any_failed = true;
            break;
        }

        const auto archive_result = archiveOne(entry);
        switch (archive_result) {
        case ArchiveResult::SkippedDuplicate:
            ++result.skipped;
            break;
        case ArchiveResult::Downloaded:
            ++result.downloaded;
            break;
        case ArchiveResult::Cancelled:
            ++result.cancelled;
            result.last_failure_reason = ArchiveFailureReason::Cancelled;
            any_failed = true;
            break;
        case ArchiveResult::Failed:
            ++result.failed;
            result.last_failure_reason = last_failure_reason_;
            any_failed = true;
            break;
        }

        if (archive_result == ArchiveResult::Cancelled) {
            break;
        }
    }

    result.all_archived = !any_failed;
    return result;
}

bool LogDownloader::eraseFlightControllerLogs() {
    logger_.info("Erasing FC logs");
    mavlink_message_t msg{};
    mavlink_msg_log_erase_pack(255, 190, &msg, client_.targetSystem(), client_.targetComponent());
    const bool sent = client_.sendMessage(msg);
    if (sent) {
        database_.markAllErased();
        logger_.info("Cleanup complete");
    }
    return sent;
}

} // namespace mcls
