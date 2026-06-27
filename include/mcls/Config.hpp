#pragma once

#include "mcls/ArchiveSummary.hpp"

#include <cstdint>
#include <string>

namespace mcls {

struct Config {
    struct TransportSettings {
        std::string transport = "tcp";
        std::string host = "127.0.0.1";
        int port = 5760;
        std::string bind_host = "0.0.0.0";
        int bind_port = 0;
        int heartbeat_timeout_sec = 5;
    } transport;

    struct DownloadSettings {
        int delay_after_disarm_sec = 2;
        int download_timeout_sec = 5;
        int retry_count = 3;
        int retry_delay_sec = 2;
        int probe_bytes = 51200;
        bool verify_after_download = true;
        bool erase_after_success = true;
        int stall_abort_attempts = 3;
        int max_queued_log_data = 2048;
        bool reconnect_on_transport_failure = true;
        int reconnect_after_consecutive_failures = 3;
        int gap_fill_idle_ms = 500;
        bool verify_dataflash_parse = true;
        double verify_max_bad_header_ratio = 0.001;
        bool detect_overlap_conflict = true;
        std::string verify_fc_reread = "sample";
        int verify_fc_reread_sample_count = 64;
        bool benchmark_download = false;
    } download;

    struct StorageSettings {
        std::string directory = "/var/lib/mcls";
        int max_size_gb = 1;
    } storage;

    struct LoggingSettings {
        bool verbose = true;
        std::string file;
    } logging;

    struct CompanionSettings {
        bool enabled = false;
        std::string bind_host = "127.0.0.1";
        int bind_port = 14541;
        std::string send_host = "127.0.0.1";
        int send_port = 14540;
        std::string token;
        int max_request_bytes = 2048;
        int max_response_bytes = 1200;
        int max_fc_logs_per_response = 8;
    } companion;

    static Config loadFromFile(const std::string& path);
};

inline FcRereadMode parseFcRereadMode(const std::string& value) {
    if (value == "none") {
        return FcRereadMode::None;
    }
    if (value == "full") {
        return FcRereadMode::Full;
    }
    return FcRereadMode::Sample;
}

} // namespace mcls
