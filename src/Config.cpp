#include "mcls/Config.hpp"

#include <toml++/toml.h>

#include <stdexcept>

namespace mcls {

Config Config::loadFromFile(const std::string& path) {
    Config cfg;
    const auto table = toml::parse_file(path);

    if (auto* sec = table["mavlink"].as_table()) {
        if (auto v = (*sec)["host"].value<std::string>()) {
            cfg.mavlink.host = *v;
        }
        if (auto v = (*sec)["port"].value<int64_t>()) {
            cfg.mavlink.port = static_cast<int>(*v);
        }
        if (auto v = (*sec)["heartbeat_timeout_sec"].value<int64_t>()) {
            cfg.mavlink.heartbeat_timeout_sec = static_cast<int>(*v);
        }
    }

    if (auto* sec = table["download"].as_table()) {
        if (auto v = (*sec)["delay_after_disarm"].value<int64_t>()) {
            cfg.download.delay_after_disarm_sec = static_cast<int>(*v);
        }
        if (auto v = (*sec)["delay_after_disarm_sec"].value<int64_t>()) {
            cfg.download.delay_after_disarm_sec = static_cast<int>(*v);
        }
        if (auto v = (*sec)["download_timeout"].value<int64_t>()) {
            cfg.download.download_timeout_sec = static_cast<int>(*v);
        }
        if (auto v = (*sec)["retry_count"].value<int64_t>()) {
            cfg.download.retry_count = static_cast<int>(*v);
        }
        if (auto v = (*sec)["retry_delay"].value<int64_t>()) {
            cfg.download.retry_delay_sec = static_cast<int>(*v);
        }
        if (auto v = (*sec)["probe_bytes"].value<int64_t>()) {
            cfg.download.probe_bytes = static_cast<int>(*v);
        }
        if (auto v = (*sec)["verify_after_download"].value<bool>()) {
            cfg.download.verify_after_download = *v;
        }
        if (auto v = (*sec)["erase_after_success"].value<bool>()) {
            cfg.download.erase_after_success = *v;
        }
    }

    if (auto* sec = table["storage"].as_table()) {
        if (auto v = (*sec)["directory"].value<std::string>()) {
            cfg.storage.directory = *v;
        }
        if (auto v = (*sec)["max_size_gb"].value<int64_t>()) {
            cfg.storage.max_size_gb = static_cast<int>(*v);
        }
    }

    if (auto* sec = table["logging"].as_table()) {
        if (auto v = (*sec)["verbose"].value<bool>()) {
            cfg.logging.verbose = *v;
        }
        if (auto v = (*sec)["file"].value<std::string>()) {
            cfg.logging.file = *v;
        }
    }

    return cfg;
}

} // namespace mcls
