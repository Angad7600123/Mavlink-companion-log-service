#pragma once

#include "mcls/Config.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace mcls {

class Logger;

/// Abstract byte stream for MAVLink frame I/O (TCP, UDP, or future transports).
class Transport {
public:
    virtual ~Transport() = default;

    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    virtual bool send(const uint8_t* data, std::size_t len) = 0;
    virtual bool readByte(uint8_t& byte) = 0;

    /// Human-readable endpoint description for logging.
    virtual std::string describe() const = 0;
};

/// Create a transport from configuration (`tcp` or `udp`).
std::unique_ptr<Transport> createTransport(const Config::TransportSettings& settings,
                                           Logger& logger);

} // namespace mcls
