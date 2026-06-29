#include "mcls/CompanionDiagnostics.hpp"

#include "SocketPlatform.hpp"

#include <cerrno>
#include <cstring>
#include <string>

namespace mcls {
namespace CompanionDiagnostics {

std::string socketErrorMessage() {
#ifdef _WIN32
    return "WSA error " + std::to_string(WSAGetLastError());
#else
    return std::string("errno ") + std::to_string(errno) + " " + std::strerror(errno);
#endif
}

void logEffectiveSettings(Logger& logger,
                          const Config::CompanionSettings& settings,
                          const std::string& config_path,
                          const bool companion_table_present) {
    logger.info("Companion config from " + config_path + ": [companion] table " +
                (companion_table_present ? "present" : "absent (using defaults)"));
    logger.info("Companion enabled=" + std::string(settings.enabled ? "true" : "false") +
                " bind=" + settings.bind_host + ":" + std::to_string(settings.bind_port) +
                " send=" + settings.send_host + ":" + std::to_string(settings.send_port) +
                " auth=" + (settings.token.empty() ? "disabled" : "token-configured") +
                " max_request_bytes=" + std::to_string(settings.max_request_bytes) +
                " max_response_bytes=" + std::to_string(settings.max_response_bytes) +
                " max_fc_logs_per_response=" + std::to_string(settings.max_fc_logs_per_response));
    if (!companion_table_present && !settings.enabled) {
        logger.info(
            "Companion hint: add [companion] enabled = true to " + config_path +
            " (not sample_master_config.toml) then restart the service");
    }
    if (settings.enabled && settings.bind_port == settings.send_port) {
        logger.warn("Companion misconfiguration: bind_port and send_port are both " +
                    std::to_string(settings.bind_port) +
                    " — bind_port must differ (wfb-ng owns send_port, typically 14540)");
    }
}

void logUdpPeer(Logger& logger,
                const char* prefix,
                const sockaddr_storage& from,
                const std::uint32_t from_len) {
    char host[NI_MAXHOST] = {};
    char port[NI_MAXSERV] = {};
    const int rc = getnameinfo(reinterpret_cast<const sockaddr*>(&from), from_len, host,
                               sizeof(host), port, sizeof(port),
                               NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc == 0) {
        logger.debug(std::string(prefix) + " from " + host + ":" + port);
    } else {
        logger.debug(std::string(prefix) + " from unknown peer (getnameinfo failed)");
    }
}

} // namespace CompanionDiagnostics
} // namespace mcls
