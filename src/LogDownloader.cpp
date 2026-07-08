#include "mcls/LogDownloader.hpp"

#include "mcls/DataFlashValidator.hpp"
#include "mcls/FcSampleOffsets.hpp"
#include "mcls/Logger.hpp"
#include "mcls/MavlinkLogProtocol.hpp"
#include "mcls/StreamDownloadSession.hpp"

#include <ardupilotmega/mavlink.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>
#include <thread>
#include <utility>

namespace mcls {

namespace {

constexpr auto kEnumerationIdle = std::chrono::milliseconds(1000);

int64_t unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

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

std::vector<LogDownloader::LogDataChunk> LogDownloader::drainLogDataChunks(const uint16_t log_id) {
    std::lock_guard lock(msg_mutex_);
    std::vector<LogDataChunk> out;
    for (auto it = data_chunks_.begin(); it != data_chunks_.end();) {
        if (it->id != log_id) {
            ++it;
            continue;
        }
        out.push_back(std::move(*it));
        it = data_chunks_.erase(it);
    }
    return out;
}

void LogDownloader::waitForLogDataNotify(const std::chrono::milliseconds timeout) {
    std::unique_lock lock(msg_mutex_);
    msg_cv_.wait_for(lock, timeout);
}

void LogDownloader::trimDataChunkQueue(const std::size_t cap) {
    std::size_t dropped = 0;
    while (data_chunks_.size() > cap) {
        data_chunks_.pop_front();
        ++dropped;
    }
    if (dropped == 0) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    constexpr auto kMinLogInterval = std::chrono::seconds(5);
    if (last_queue_overflow_log_.time_since_epoch().count() == 0 ||
        now - last_queue_overflow_log_ >= kMinLogInterval) {
        std::string msg = "Dropped " + std::to_string(dropped) +
                          " oldest LOG_DATA chunk(s) (queue cap=" + std::to_string(cap) + ")";
        if (queue_overflow_suppressed_ > 0) {
            msg += " [" + std::to_string(queue_overflow_suppressed_) + " similar events suppressed]";
            queue_overflow_suppressed_ = 0;
        }
        logger_.warn(msg);
        last_queue_overflow_log_ = now;
    } else {
        ++queue_overflow_suppressed_;
    }
}

void LogDownloader::abortLogTransfer(const std::string& reason) {
    logger_.warn("Aborting FC log transfer: " + reason);
    clearDataChunks();
    requestLogEnd();
}

ArchiveFailureReason LogDownloader::classifyTimeout() {
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

void LogDownloader::logArchiveSummary(const ArchivePerformanceSummary& summary) const {
    logger_.info(summary.formatLine());
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
            // FC-busy signature: the FC answers but cannot serve the log yet
            // (common right after disarm while the flight log is finalized).
            // Count it for StreamDownloadSession's busy detection; rate-limit
            // the log line — these arrive in long bursts.
            const auto n = zero_length_data_count_.fetch_add(1) + 1;
            if (n % 10 == 1) {
                logger_.warn("Zero-length LOG_DATA (log=" + std::to_string(data.id) +
                             ", offset=" + std::to_string(data.ofs) + ", total seen=" +
                             std::to_string(n) + ") — FC not serving this log yet");
            }
            msg_cv_.notify_all();
            return;
        }

        LogDataChunk chunk;
        chunk.id = data.id;
        chunk.offset = data.ofs;
        chunk.count = data.count;
        chunk.data.assign(data.data, data.data + data.count);

        data_chunks_.push_back(std::move(chunk));
        const std::size_t cap =
            settings_.max_queued_log_data > 0
                ? static_cast<std::size_t>(settings_.max_queued_log_data)
                : 2048;
        trimDataChunkQueue(cap);
        msg_cv_.notify_all();
    }
}

void LogDownloader::requestLogList() {
    mavlink_message_t msg{};
    mavlink_msg_log_request_list_pack(
        255, 190, &msg, client_.targetSystem(), client_.targetComponent(), 0, 0xFFFF);
    client_.sendMessage(msg);
}

bool LogDownloader::requestLogData(const uint16_t log_id,
                                     const uint32_t offset,
                                     const uint32_t count) {
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

    // The FC often has not finished making its logs enumerable for several
    // seconds after disarm (it is still finalizing the just-closed flight log),
    // and a single LOG_REQUEST_LIST or its LOG_ENTRY replies can be lost on the
    // link. So re-issue the request while we keep getting nothing back.
    const int attempts = std::max(1, settings_.enumerate_attempts);
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        requestLogList();

        // Wait until entries stop arriving for kEnumerationIdle (i.e. the list
        // has settled), capped by a hard deadline. NOTE: completion is detected
        // by *new* entries since the last tick — not by pending_entries_ being
        // non-empty, which never drains within a single enumeration and would
        // otherwise pin the loop to the full deadline on every success.
        std::size_t seen = 0;
        {
            std::lock_guard lock(msg_mutex_);
            seen = pending_entries_.size();
        }
        auto last_progress = std::chrono::steady_clock::now();
        const auto deadline = last_progress + std::chrono::seconds(15);

        while (std::chrono::steady_clock::now() < deadline) {
            if (cancelled()) {
                logger_.warn("Enumeration cancelled");
                break;
            }
            std::size_t count;
            {
                std::unique_lock lock(msg_mutex_);
                msg_cv_.wait_for(lock, std::chrono::milliseconds(200));
                count = pending_entries_.size();
            }
            if (count > seen) {
                seen = count;
                last_progress = std::chrono::steady_clock::now();
            } else if (std::chrono::steady_clock::now() - last_progress >= kEnumerationIdle) {
                break;  // settled — no new entries within the idle window
            }
        }

        std::size_t total;
        {
            std::lock_guard lock(msg_mutex_);
            total = pending_entries_.size();
        }
        if (total > 0 || cancelled() || attempt == attempts) {
            break;  // got logs, aborted, or out of attempts
        }
        logger_.warn("Enumeration attempt " + std::to_string(attempt) + "/" +
                     std::to_string(attempts) +
                     " found 0 logs (FC may still be finalizing) — retrying in " +
                     std::to_string(settings_.enumerate_retry_delay_sec) + "s");
        std::this_thread::sleep_for(std::chrono::seconds(settings_.enumerate_retry_delay_sec));
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

    clearDataChunks();
    StreamDownloadSession::Params params{};
    params.log_id = entry.id;
    params.byte_offset = 0;
    params.byte_count = probe_n;
    params.probe_bytes = static_cast<int>(probe_n);
    params.write_file = false;

    std::ofstream unused;
    StreamDownloadSession session(*this, logger_, settings_, params);
    const auto result = session.run(unused);
    requestLogEnd();

    if (!result.success) {
        last_failure_reason_ = result.failure;
        logger_.warn("Probe download failed for log " + std::to_string(entry.id));
        return false;
    }

    if (result.probe_sha256 == candidate.probe_sha256 &&
        static_cast<int>(probe_n) == candidate.probe_bytes) {
        logger_.info("Log " + std::to_string(entry.id) +
                     " confirmed duplicate via probe hash, skipping");
        return true;
    }

    logger_.info("Log " + std::to_string(entry.id) + " probe hash mismatch, downloading full log");
    return false;
}

bool LogDownloader::waitForLogData(const uint16_t log_id,
                                     const uint32_t expected_offset,
                                     std::vector<uint8_t>& out,
                                     const std::chrono::milliseconds timeout) {
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

bool LogDownloader::downloadLogData(const LogEntry& entry,
                                    const std::filesystem::path& partial_path,
                                    std::string& out_sha256,
                                    std::string& out_probe_sha256,
                                    int& out_probe_bytes,
                                    std::chrono::steady_clock::time_point& out_start_time,
                                    StreamDownloadMetrics& out_metrics) {
    out_start_time = std::chrono::steady_clock::now();
    last_failure_reason_ = ArchiveFailureReason::None;

    clearDataChunks();

    std::ofstream file(partial_path, std::ios::binary | std::ios::trunc);
    if (!file) {
        logger_.error("Failed to open partial file: " + partial_path.string());
        last_failure_reason_ = ArchiveFailureReason::StorageError;
        return false;
    }

    StreamDownloadSession::Params params{};
    params.log_id = entry.id;
    params.byte_offset = 0;
    params.byte_count = entry.size;
    params.probe_bytes = settings_.probe_bytes;

    StreamDownloadSession session(*this, logger_, settings_, params);
    const auto result = session.run(file);
    requestLogEnd();

    out_metrics = result.metrics;
    out_probe_bytes = result.probe_bytes;

    if (!result.success) {
        last_failure_reason_ = result.failure;
        return false;
    }

    if (settings_.verify_after_download && result.ranges.bytesReceived() != entry.size) {
        last_failure_reason_ = ArchiveFailureReason::VerificationFailed;
        return false;
    }

    out_sha256 = result.sha256;
    out_probe_sha256 = result.probe_sha256;
    last_failure_reason_ = ArchiveFailureReason::None;
    return true;
}

bool LogDownloader::verifyDataFlashFile(const std::filesystem::path& path) {
    if (!settings_.verify_dataflash_parse) {
        return true;
    }
    const auto result = validateDataFlashFile(path);
    if (!result.ok) {
        logger_.error("DataFlash parse failed: " + result.error);
        return false;
    }
    if (dataFlashResyncRatioExceeded(result, static_cast<uint32_t>(std::filesystem::file_size(path)),
                                     settings_.verify_max_bad_header_ratio)) {
        logger_.error("DataFlash resync ratio too high: resyncs=" +
                      std::to_string(result.resync_count));
        return false;
    }
    return true;
}

bool LogDownloader::verifyFcSampleReread(const LogEntry& entry,
                                           const std::filesystem::path& path,
                                           const std::string& sha256) {
    const auto mode = parseFcRereadMode(settings_.verify_fc_reread);
    if (mode == FcRereadMode::None) {
        return true;
    }

    if (mode == FcRereadMode::Full) {
        clearDataChunks();
        std::ofstream discard;
        StreamDownloadSession::Params params{};
        params.log_id = entry.id;
        params.byte_offset = 0;
        params.byte_count = entry.size;
        params.probe_bytes = 0;
        params.write_file = false;
        StreamDownloadSession session(*this, logger_, settings_, params);
        const auto result = session.run(discard);
        requestLogEnd();
        if (!result.success) {
            return false;
        }
        return result.sha256 == sha256;
    }

    const auto offsets = buildFcSampleOffsets(entry.size, settings_.verify_fc_reread_sample_count,
                                              entry.id, entry.size, sha256);
    if (offsets.empty()) {
        return true;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }

    clearDataChunks();
    for (const uint32_t ofs : offsets) {
        if (!requestLogData(entry.id, ofs, static_cast<uint32_t>(kLogChunkSize))) {
            return false;
        }
        std::vector<uint8_t> chunk;
        if (!waitForLogData(entry.id, ofs, chunk,
                            std::chrono::seconds(settings_.download_timeout_sec))) {
            return false;
        }
        std::vector<char> file_buf(kLogChunkSize);
        in.seekg(static_cast<std::streamoff>(ofs));
        in.read(file_buf.data(), static_cast<std::streamsize>(kLogChunkSize));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got != chunk.size() ||
            !std::equal(chunk.begin(), chunk.end(), file_buf.begin(), file_buf.begin() + got)) {
            logger_.error("FC sample re-read mismatch at offset " + std::to_string(ofs));
            return false;
        }
    }
    requestLogEnd();
    return true;
}

ArchiveResult LogDownloader::archiveOne(const LogEntry& entry) {
    ArchivePerformanceSummary summary;
    summary.log_id = entry.id;
    summary.total_size = entry.size;
    summary.log_erase = "no";

    const auto archive_start = std::chrono::steady_clock::now();

    std::optional<ArchivedLogCandidate> candidate;
    const auto decision = checkDedup(entry, candidate);

    if (decision == DedupDecision::DownloadAfterProbeMismatch && candidate) {
        if (downloadProbeAndCompare(entry, *candidate)) {
            summary.final_decision = "skipped_duplicate";
            summary.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - archive_start)
                                      .count();
            logArchiveSummary(summary);
            database_.incrementStat("flights_skipped");
            return ArchiveResult::SkippedDuplicate;
        }
        if (cancelled()) {
            summary.final_decision = "cancelled";
            logArchiveSummary(summary);
            return ArchiveResult::Cancelled;
        }
    }

    if (cancelled()) {
        summary.final_decision = "cancelled";
        logArchiveSummary(summary);
        return ArchiveResult::Cancelled;
    }

    logger_.info("Downloading log " + std::to_string(entry.id) + " (" +
                 std::to_string(entry.size) + " bytes)");

    const auto partial = storage_.beginPartialFile();
    std::string sha256;
    std::string probe_sha256;
    int probe_bytes = 0;
    std::chrono::steady_clock::time_point download_start;
    StreamDownloadMetrics metrics{};

    updateProgressBegin(entry.id, entry.size);
    if (!downloadLogData(entry, partial, sha256, probe_sha256, probe_bytes, download_start,
                         metrics)) {
        updateProgressEnd();
        std::error_code ec;
        std::filesystem::remove(partial, ec);
        summary.download = metrics;
        summary.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - archive_start)
                                  .count();
        summary.final_decision = cancelled() ? "cancelled" : "failed";
        logArchiveSummary(summary);
        if (last_failure_reason_ == ArchiveFailureReason::Cancelled || cancelled()) {
            database_.incrementStat("archive_cancellations");
            return ArchiveResult::Cancelled;
        }
        database_.incrementStat("archive_failures");
        return ArchiveResult::Failed;
    }

    summary.download = metrics;
    summary.sha256 = sha256;

    if (!verifyDataFlashFile(partial)) {
        updateProgressEnd();
        std::error_code ec;
        std::filesystem::remove(partial, ec);
        last_failure_reason_ = ArchiveFailureReason::ParseFailed;
        summary.parse_result = VerificationOutcome::Failed;
        summary.final_decision = "failed";
        summary.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - archive_start)
                                  .count();
        logArchiveSummary(summary);
        database_.incrementStat("archive_failures");
        return ArchiveResult::Failed;
    }
    summary.parse_result = settings_.verify_dataflash_parse ? VerificationOutcome::Ok
                                                            : VerificationOutcome::Skipped;

    if (!verifyFcSampleReread(entry, partial, sha256)) {
        updateProgressEnd();
        std::error_code ec;
        std::filesystem::remove(partial, ec);
        last_failure_reason_ = ArchiveFailureReason::RereadMismatch;
        summary.sample_reread_result = VerificationOutcome::Failed;
        summary.final_decision = "failed";
        summary.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - archive_start)
                                  .count();
        logArchiveSummary(summary);
        database_.incrementStat("archive_failures");
        return ArchiveResult::Failed;
    }
    summary.sample_reread_result =
        parseFcRereadMode(settings_.verify_fc_reread) == FcRereadMode::None
            ? VerificationOutcome::Skipped
            : VerificationOutcome::Ok;

    const auto finalized = storage_.finalizePartial(partial, download_start);
    if (!finalized) {
        updateProgressEnd();
        last_failure_reason_ = ArchiveFailureReason::StorageError;
        summary.final_decision = "failed";
        logArchiveSummary(summary);
        database_.incrementStat("archive_failures");
        return ArchiveResult::Failed;
    }

    updateProgressEnd();
    summary.duration_ms = finalized->archive_duration_ms;
    if (summary.duration_ms > 0) {
        summary.avg_throughput_kbps =
            (static_cast<double>(entry.size) / 1024.0) / (summary.duration_ms / 1000.0);
    }

    database_.insertArchivedLog(sha256, entry.id, entry.size, entry.time_utc, probe_sha256,
                                probe_bytes, unixNow(), finalized->archive_duration_ms,
                                finalized->final_path.string());

    database_.incrementStat("flights_archived");
    database_.incrementStat("bytes_downloaded", static_cast<int64_t>(entry.size));
    database_.setStat("last_archive_time", unixNow());

    summary.final_decision = "archived";
    logArchiveSummary(summary);
    return ArchiveResult::Downloaded;
}

ArchiveCycleResult LogDownloader::archiveAll(const std::vector<LogEntry>& logs) {
    ArchiveCycleResult result;
    result.last_failure_reason = ArchiveFailureReason::None;

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

LogDownloader::ActiveArchiveProgress LogDownloader::activeProgress() const {
    std::lock_guard lock(progress_mutex_);
    return progress_;
}

void LogDownloader::updateProgressBegin(uint16_t log_id, uint32_t total_bytes) {
    std::lock_guard lock(progress_mutex_);
    progress_.active = true;
    progress_.log_id = log_id;
    progress_.bytes_received = 0;
    progress_.total_bytes = total_bytes;
    progress_.bytes_per_sec = 0;
    progress_begin_ = std::chrono::steady_clock::now();
}

void LogDownloader::updateProgressBytes(uint32_t bytes_received) {
    std::lock_guard lock(progress_mutex_);
    progress_.bytes_received = bytes_received;
    // Average throughput since this log began — stable enough for a progress bar
    // and surfaced to the companion status API (complements benchmark_download).
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - progress_begin_).count();
    if (elapsed > 0.2) {
        progress_.bytes_per_sec = static_cast<uint32_t>(bytes_received / elapsed);
    }
}

void LogDownloader::updateProgressEnd() {
    std::lock_guard lock(progress_mutex_);
    progress_ = ActiveArchiveProgress{};
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
