#include "mcls/StreamDownloadSession.hpp"

#include "mcls/ChunkCoverageTracker.hpp"
#include "mcls/LogDownloader.hpp"
#include "mcls/Logger.hpp"

#include <algorithm>
#include <thread>

namespace mcls {

StreamDownloadSession::StreamDownloadSession(LogDownloader& owner,
                                             Logger& logger,
                                             const Config::DownloadSettings& settings,
                                             const Params params)
    : owner_(owner), logger_(logger), settings_(settings), params_(params) {}

bool StreamDownloadSession::processQueuedChunks(
    std::ofstream& file,
    ReceivedRanges& ranges,
    ChunkCoverageTracker& coverage,
    Sha256& hasher,
    bool& probe_finalized,
    uint32_t& hashed_bytes,
    std::string& probe_sha256,
    bool& saw_short_eof,
    StreamDownloadMetrics& metrics) {
    const auto chunks = owner_.drainLogDataChunks(params_.log_id);
    const uint32_t probe_n =
        static_cast<uint32_t>(std::min<int>(params_.probe_bytes, static_cast<int>(params_.byte_count)));

    const std::size_t queue_depth = chunks.size();
    if (queue_depth > metrics.queue_depth_max) {
        metrics.queue_depth_max = queue_depth;
    }

    for (const auto& chunk : chunks) {
        metrics.log_data_packets++;

        if (chunk.offset < params_.byte_offset ||
            chunk.offset >= params_.byte_offset + params_.byte_count) {
            continue;
        }

        uint32_t write_ofs = chunk.offset;
        uint32_t write_len = chunk.count;
        if (write_ofs + write_len > params_.byte_offset + params_.byte_count) {
            write_len = params_.byte_offset + params_.byte_count - write_ofs;
        }

        if (write_len == 0) {
            continue;
        }

        const uint32_t slots_before = coverage.slotsReceived();

        if (params_.write_file) {
            file.seekp(static_cast<std::streamoff>(write_ofs));
            file.write(reinterpret_cast<const char*>(chunk.data.data()),
                       static_cast<std::streamsize>(write_len));
        }

        if (!coverage.record(write_ofs, chunk.data.data(), write_len,
                             settings_.detect_overlap_conflict)) {
            return false;
        }

        if (coverage.slotsReceived() == slots_before && write_ofs % kLogChunkSize == 0) {
            metrics.duplicate_packets++;
        }

        if (!probe_finalized && hashed_bytes + write_len >= probe_n) {
            const uint32_t probe_take = probe_n - hashed_bytes;
            if (probe_take > 0) {
                hasher.update(chunk.data.data(), probe_take);
            }
            probe_sha256 = hasher.clone().finalizeHex();
            probe_finalized = true;
            if (probe_take < write_len) {
                hasher.update(chunk.data.data() + probe_take, write_len - probe_take);
            }
        } else {
            hasher.update(chunk.data.data(), write_len);
        }
        hashed_bytes += write_len;

        ranges.add(write_ofs, write_len);

        if (write_len < kLogChunkSize &&
            write_ofs + write_len == params_.byte_offset + params_.byte_count) {
            saw_short_eof = true;
        }
    }

    return true;
}

void StreamDownloadSession::requestMergedGaps(const ReceivedRanges& ranges,
                                              const uint32_t total_size) {
    const auto gaps = ranges.gaps(total_size);
    const uint32_t c = static_cast<uint32_t>(kLogChunkSize);

    for (const auto& [gap_offset, gap_count] : gaps) {
        if (gap_count == 0) {
            continue;
        }

        uint32_t start = alignDownToChunk(gap_offset);
        if (start < gap_offset) {
            start += c;
        }
        if (start >= gap_offset + gap_count) {
            start = gap_offset;
        }

        uint32_t end = gap_offset + gap_count;
        uint32_t req_start = start;
        while (req_start < end) {
            uint32_t req_count = end - req_start;
            uint32_t coalesced = c;
            while (req_start + coalesced < end) {
                coalesced += c;
            }
            req_count = std::min(req_count, coalesced);
            req_count = std::min(req_count, kLogStreamRequestCount);
            owner_.sendLogRequestData(params_.log_id, req_start, req_count);
            req_start += req_count;
        }
    }
}

void StreamDownloadSession::logBenchmark(const StreamDownloadMetrics& metrics,
                                         const uint32_t bytes_received,
                                         const std::chrono::steady_clock::time_point start) const {
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double sec =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 1000.0;
    const double kbps = sec > 0 ? (bytes_received / 1024.0) / sec : 0.0;
    logger_.info("Benchmark log=" + std::to_string(params_.log_id) + " kbps=" +
                 std::to_string(static_cast<int>(kbps)) + " chunks=" +
                 std::to_string(metrics.log_data_packets) + " gap_fills=" +
                 std::to_string(metrics.gap_fill_requests) + " queue_max=" +
                 std::to_string(metrics.queue_depth_max) + " rtt_ms=" +
                 std::to_string(static_cast<int>(metrics.estimated_rtt_ms)));
}

StreamDownloadResult StreamDownloadSession::run(std::ofstream& file) {
    StreamDownloadResult result;
    result.failure = ArchiveFailureReason::None;

    const uint32_t total = params_.byte_count;
    if (total == 0) {
        result.success = true;
        result.probe_bytes = params_.probe_bytes;
        result.probe_sha256 = Sha256::hashHex(nullptr, 0);
        result.sha256 = Sha256::hashHex(nullptr, 0);
        return result;
    }

    ReceivedRanges ranges;
    ChunkCoverageTracker coverage(total);
    Sha256 hasher;
    bool probe_finalized = false;
    uint32_t hashed_bytes = 0;
    bool saw_short_eof = false;
    int no_progress_attempts = 0;
    uint32_t last_progress_bytes = 0;
    bool logged_first = false;

    const uint32_t probe_n =
        static_cast<uint32_t>(std::min<int>(params_.probe_bytes, static_cast<int>(total)));
    result.probe_bytes = static_cast<int>(probe_n);

    const auto start_time = std::chrono::steady_clock::now();
    auto last_data_time = start_time;
    auto last_benchmark = start_time;
    auto last_gap_request = std::chrono::steady_clock::time_point{};

    owner_.clearLogDataChunks();

    for (int attempt = 0; attempt <= settings_.retry_count; ++attempt) {
        if (owner_.cancelled()) {
            result.failure = ArchiveFailureReason::Cancelled;
            return result;
        }

        const uint32_t before = ranges.bytesReceived();

        if (before == 0 || attempt > 0) {
            owner_.sendLogRequestData(params_.log_id, params_.byte_offset, kLogStreamRequestCount);
            result.metrics.log_request_packets++;
            last_data_time = std::chrono::steady_clock::now();
        }

        bool session_done = false;
        while (!owner_.cancelled()) {
            if (!processQueuedChunks(file, ranges, coverage, hasher, probe_finalized, hashed_bytes,
                                     result.probe_sha256, saw_short_eof, result.metrics)) {
                result.failure = ArchiveFailureReason::OverlapConflict;
                result.metrics.overlap_conflicts = coverage.overlapConflicts();
                return result;
            }

            const uint32_t received = ranges.bytesReceived();
            if (received > last_progress_bytes) {
                last_data_time = std::chrono::steady_clock::now();
                last_progress_bytes = received;
                no_progress_attempts = 0;
                owner_.updateProgressBytes(received);
                if (!logged_first) {
                    logged_first = true;
                    logger_.info("Log " + std::to_string(params_.log_id) + ": streaming (" +
                                 std::to_string(received) + "/" + std::to_string(total) +
                                 " bytes)");
                }
            }

            if (settings_.benchmark_download) {
                const auto now = std::chrono::steady_clock::now();
                if (now - last_benchmark >= std::chrono::seconds(5)) {
                    logBenchmark(result.metrics, received, start_time);
                    last_benchmark = now;
                }
            }

            const bool range_complete = ranges.complete(total);
            const bool slots_complete = coverage.allSlotsReceived();
            if (range_complete && slots_complete) {
                session_done = true;
                break;
            }

            const auto idle_ms = std::chrono::milliseconds(settings_.gap_fill_idle_ms);
            const auto now = std::chrono::steady_clock::now();
            if (!range_complete && now - last_data_time >= idle_ms &&
                (last_gap_request.time_since_epoch().count() == 0 ||
                 now - last_gap_request >= idle_ms)) {
                requestMergedGaps(ranges, total);
                result.metrics.gap_fill_requests++;
                last_gap_request = now;
            }

            if (now - last_data_time >=
                std::chrono::seconds(settings_.download_timeout_sec)) {
                result.failure = owner_.classifyTransferTimeout();
                result.metrics.retries++;
                break;
            }

            owner_.waitForLogDataNotify(std::chrono::milliseconds(10));
        }

        if (session_done) {
            result.success = true;
            break;
        }

        if (ranges.bytesReceived() <= before) {
            if (++no_progress_attempts >= settings_.stall_abort_attempts) {
                if (result.failure == ArchiveFailureReason::None) {
                    result.failure = ArchiveFailureReason::NoProgress;
                }
                owner_.abortLogTransfer("no forward progress");
                return result;
            }
        }

        if (attempt < settings_.retry_count) {
            owner_.clearLogDataChunks();
            owner_.sendLogRequestEnd();
            std::this_thread::sleep_for(std::chrono::seconds(settings_.retry_delay_sec));
        }
    }

    if (params_.write_file) {
        file.flush();
    }

    if (!result.success) {
        if (result.failure == ArchiveFailureReason::None) {
            result.failure = ranges.complete(total) ? ArchiveFailureReason::VerificationFailed
                                                    : ArchiveFailureReason::IncompleteDownload;
        }
        return result;
    }

    if (coverage.overlapConflicts() > 0) {
        result.failure = ArchiveFailureReason::OverlapConflict;
        result.success = false;
        return result;
    }

    if (probe_n == 0) {
        result.probe_sha256 = Sha256::hashHex(nullptr, 0);
    } else if (!probe_finalized) {
        result.probe_sha256 = hasher.clone().finalizeHex();
    }

    result.sha256 = hasher.finalizeHex();
    result.ranges = ranges;
    result.metrics.overlap_conflicts = coverage.overlapConflicts();
    (void)saw_short_eof;

    if (settings_.benchmark_download) {
        logBenchmark(result.metrics, ranges.bytesReceived(), start_time);
    }

    return result;
}

} // namespace mcls
