#include "mcls/Transport.hpp"
#include "mcls/TcpTransport.hpp"
#include "mcls/UdpTransport.hpp"
#include "mcls/Logger.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace mcls {

namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

} // namespace

std::unique_ptr<Transport> createTransport(const Config::TransportSettings& settings,
                                           Logger& logger) {
    const std::string type = lower(settings.transport);
    if (type == "tcp") {
        return std::make_unique<TcpTransport>(settings.host, settings.port, logger);
    }
    if (type == "udp") {
        return std::make_unique<UdpTransport>(settings.host, settings.port, settings.bind_host,
                                              settings.bind_port, logger);
    }
    throw std::runtime_error("Unsupported transport type: " + settings.transport);
}

} // namespace mcls
