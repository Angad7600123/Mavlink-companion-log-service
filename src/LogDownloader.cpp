#include "mcls/LogDownloader.hpp"
#include "mcls/Logger.hpp"
#include "mcls/ReceivedRanges.hpp"
#include "mcls/Sha256.hpp"

#include <ardupilotmega/mavlink.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>
#include <thread>

namespace mcls {

namespace {

constexpr int kLogDataMaxBytes = 90;
constexpr auto kEnumerationIdle = std::chrono::milliseconds(1000);

int64_t unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
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

        LogDataChunk chunk;
        chunk.id = data.id;
        chunk.offset = data.ofs;
        chunk.count = data.count;
        chunk.data.assign(data.data, data.data + data.count);
        data_chunks_.push_back(std::move(chunk));
        msg_cv_.notify_all();
    }
}

void LogDownloader::requestLogList() {
    mavlink_message_t msg{};
    mavlink_msg_log_request_list_pack(
        255, 190, &msg, client_.targetSystem(), client_.targetComponent(), 0, 0xFFFF);
    client_.sendMessage(msg);
}

void LogDownloader::requestLogData(uint16_t log_id, uint32_t offset, uint16_t count) {
    mavlink_message_t msg{};
    mavlink_msg_log_request_data_pack(255, 190, &msg, client_.targetSystem(),
                                    client_.targetComponent(), log_id, offset, count);
    client_.sendMessage(msg);
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
        std::unique_lock lock(msg_mutex_);
        msg_cv_.wait_for(lock, std::chrono::milliseconds(50));

        for (auto it = data_chunks_.begin(); it != data_chunks_.end(); ++it) {
            if (it->id == log_id && it->offset == expected_offset) {
                out = std::move(it->data);
                data_chunks_.erase(it);
                return true;
            }
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
    for (int attempt = 0; attempt <= settings_.retry_count && received < count; ++attempt) {
        while (received < count) {
            const uint32_t request_count =
                std::min<uint32_t>(count - received, kLogDataMaxBytes);
            const uint32_t request_offset = offset + received;
            requestLogData(log_id, request_offset, static_cast<uint16_t>(request_count));

            std::vector<uint8_t> chunk;
            if (!waitForLogData(log_id, request_offset, chunk,
                                std::chrono::seconds(settings_.download_timeout_sec))) {
                logger_.warn("Timeout waiting for LOG_DATA at offset " +
                             std::to_string(request_offset));
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

        if (attempt < settings_.retry_count) {
            std::this_thread::sleep_for(std::chrono::seconds(settings_.retry_delay_sec));
        }
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
    const uint32_t probe_n =
        static_cast<uint32_t>(std::min<int>(settings_.probe_bytes, static_cast<int>(entry.size)));
    out_probe_bytes = static_cast<int>(probe_n);

    {
        std::lock_guard lock(msg_mutex_);
        data_chunks_.clear();
    }

    std::ofstream file(partial_path, std::ios::binary | std::ios::trunc);
    if (!file) {
        logger_.error("Failed to open partial file: " + partial_path.string());
        return false;
    }

    ReceivedRanges ranges;
    Sha256 hasher;
    bool probe_finalized = false;
    uint32_t hashed_bytes = 0;
    uint32_t last_progress_bytes = 0;

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
        const auto gaps = ranges.complete(entry.size)
                              ? std::vector<std::pair<uint32_t, uint32_t>>{}
                              : (ranges.bytesReceived() == 0 && entry.size > 0
                                     ? std::vector<std::pair<uint32_t, uint32_t>>{{0, entry.size}}
                                     : ranges.gaps(entry.size));

        for (const auto& [gap_offset, gap_count] : gaps) {
            uint32_t gap_remaining = gap_count;
            uint32_t gap_current = gap_offset;
            while (gap_remaining > 0) {
                const uint16_t request_count =
                    static_cast<uint16_t>(std::min<uint32_t>(gap_remaining, kLogDataMaxBytes));
                requestLogData(entry.id, gap_current, request_count);

                std::vector<uint8_t> chunk;
                if (!waitForLogData(entry.id, gap_current, chunk,
                                    std::chrono::seconds(settings_.download_timeout_sec))) {
                    logger_.warn("Timeout waiting for LOG_DATA at offset " +
                                 std::to_string(gap_current));
                    database_.incrementStat("retries");
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
        }

        if (ranges.complete(entry.size)) {
            break;
        }

        if (attempt < settings_.retry_count) {
            std::this_thread::sleep_for(std::chrono::seconds(settings_.retry_delay_sec));
        }
    }

    file.flush();

    if (!ranges.complete(entry.size)) {
        logger_.error("Incomplete download for log " + std::to_string(entry.id));
        return false;
    }

    if (settings_.verify_after_download &&
        (!ranges.complete(entry.size) || ranges.bytesReceived() != entry.size)) {
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
    }

    logger_.info("Downloading log " + std::to_string(entry.id) + " (" +
                 std::to_string(entry.size) + " bytes)");

    const auto partial = storage_.beginPartialFile();
    std::string sha256;
    std::string probe_sha256;
    int probe_bytes = 0;
    std::chrono::steady_clock::time_point download_start;

    if (!downloadLogData(entry, partial, sha256, probe_sha256, probe_bytes, download_start)) {
        logger_.error("Download failed for log " + std::to_string(entry.id));
        std::filesystem::remove(partial);
        database_.incrementStat("archive_failures");
        return ArchiveResult::Failed;
    }

    logger_.info("Downloaded " + std::to_string(entry.size) + " bytes for log " +
                 std::to_string(entry.id));

    const auto finalized = storage_.finalizePartial(partial, download_start);
    if (!finalized) {
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
    bool any_failed = false;

    for (const auto& entry : logs) {
        const auto archive_result = archiveOne(entry);
        switch (archive_result) {
        case ArchiveResult::SkippedDuplicate:
            ++result.skipped;
            break;
        case ArchiveResult::Downloaded:
            ++result.downloaded;
            break;
        case ArchiveResult::Failed:
            ++result.failed;
            any_failed = true;
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
