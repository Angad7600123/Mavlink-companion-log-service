#include "mcls/Config.hpp"

#include <toml++/toml.h>

#include <stdexcept>

namespace mcls {

namespace {

void loadTransportTable(const toml::table& sec, Config::TransportSettings& out) {
    if (auto v = sec["transport"].value<std::string>()) {
        out.transport = *v;
    }
    if (auto v = sec["host"].value<std::string>()) {
        out.host = *v;
    }
    if (auto v = sec["port"].value<int64_t>()) {
        out.port = static_cast<int>(*v);
    }
    if (auto v = sec["bind_host"].value<std::string>()) {
        out.bind_host = *v;
    }
    if (auto v = sec["bind_port"].value<int64_t>()) {
        out.bind_port = static_cast<int>(*v);
    }
    if (auto v = sec["heartbeat_timeout_sec"].value<int64_t>()) {
        out.heartbeat_timeout_sec = static_cast<int>(*v);
    }
}

} // namespace

Config Config::loadFromFile(const std::string& path) {
    Config cfg;
    const auto table = toml::parse_file(path);

    if (auto* sec = table["transport"].as_table()) {
        loadTransportTable(*sec, cfg.transport);
    } else if (auto* sec = table["mavlink"].as_table()) {
        loadTransportTable(*sec, cfg.transport);
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
        if (auto v = (*sec)["stall_abort_attempts"].value<int64_t>()) {
            cfg.download.stall_abort_attempts = static_cast<int>(*v);
        }
        if (auto v = (*sec)["max_queued_log_data"].value<int64_t>()) {
            cfg.download.max_queued_log_data = static_cast<int>(*v);
        }
        if (auto v = (*sec)["reconnect_on_transport_failure"].value<bool>()) {
            cfg.download.reconnect_on_transport_failure = *v;
        }
        if (auto v = (*sec)["reconnect_after_consecutive_failures"].value<int64_t>()) {
            cfg.download.reconnect_after_consecutive_failures = static_cast<int>(*v);
        }
        if (auto v = (*sec)["gap_fill_idle_ms"].value<int64_t>()) {
            cfg.download.gap_fill_idle_ms = static_cast<int>(*v);
        }
        if (auto v = (*sec)["verify_dataflash_parse"].value<bool>()) {
            cfg.download.verify_dataflash_parse = *v;
        }
        if (auto v = (*sec)["verify_max_bad_header_ratio"].value<double>()) {
            cfg.download.verify_max_bad_header_ratio = *v;
        }
        if (auto v = (*sec)["detect_overlap_conflict"].value<bool>()) {
            cfg.download.detect_overlap_conflict = *v;
        }
        if (auto v = (*sec)["verify_fc_reread"].value<std::string>()) {
            cfg.download.verify_fc_reread = *v;
        }
        if (auto v = (*sec)["verify_fc_reread_sample_count"].value<int64_t>()) {
            cfg.download.verify_fc_reread_sample_count = static_cast<int>(*v);
        }
        if (auto v = (*sec)["benchmark_download"].value<bool>()) {
            cfg.download.benchmark_download = *v;
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

    if (auto* sec = table["companion"].as_table()) {
        cfg.companion_table_present = true;
        if (auto v = (*sec)["enabled"].value<bool>()) {
            cfg.companion.enabled = *v;
        }
        if (auto v = (*sec)["bind_host"].value<std::string>()) {
            cfg.companion.bind_host = *v;
        }
        if (auto v = (*sec)["bind_port"].value<int64_t>()) {
            cfg.companion.bind_port = static_cast<int>(*v);
        }
        if (auto v = (*sec)["send_host"].value<std::string>()) {
            cfg.companion.send_host = *v;
        }
        if (auto v = (*sec)["send_port"].value<int64_t>()) {
            cfg.companion.send_port = static_cast<int>(*v);
        }
        if (auto v = (*sec)["token"].value<std::string>()) {
            cfg.companion.token = *v;
        }
        if (auto v = (*sec)["max_request_bytes"].value<int64_t>()) {
            cfg.companion.max_request_bytes = static_cast<int>(*v);
        }
        if (auto v = (*sec)["max_response_bytes"].value<int64_t>()) {
            cfg.companion.max_response_bytes = static_cast<int>(*v);
        }
        if (auto v = (*sec)["max_fc_logs_per_response"].value<int64_t>()) {
            cfg.companion.max_fc_logs_per_response = static_cast<int>(*v);
        }
    }

    return cfg;
}

} // namespace mcls
