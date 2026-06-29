#pragma once

#include "mcls/Config.hpp"
#include "mcls/Logger.hpp"

#include <cstdint>
#include <string>

struct sockaddr_storage;

namespace mcls {
namespace CompanionDiagnostics {

/// Platform socket error string (errno / WSAGetLastError).
std::string socketErrorMessage();

/// Log effective [companion] settings (token presence only, never the secret).
void logEffectiveSettings(Logger& logger,
                            const Config::CompanionSettings& settings,
                            const std::string& config_path,
                            bool companion_table_present);

/// Log UDP peer as host:port when available.
void logUdpPeer(Logger& logger,
                const char* prefix,
                const sockaddr_storage& from,
                std::uint32_t from_len);

} // namespace CompanionDiagnostics
} // namespace mcls
