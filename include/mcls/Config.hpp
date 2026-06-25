#pragma once

#include <cstdint>
#include <string>

namespace mcls {

/// Runtime configuration loaded from TOML.
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
    } download;

    struct StorageSettings {
        std::string directory = "/var/lib/mcls";
        int max_size_gb = 1;
    } storage;

    struct LoggingSettings {
        bool verbose = true;
        std::string file;
    } logging;

    /// Load configuration from a TOML file path.
    static Config loadFromFile(const std::string& path);
};

} // namespace mcls
