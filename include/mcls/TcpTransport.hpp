#pragma once

#include "mcls/Transport.hpp"

#include <atomic>
#include <string>

namespace mcls {

class Logger;

/// MAVLink transport over a TCP connection to host:port.
class TcpTransport : public Transport {
public:
    TcpTransport(std::string host, int port, Logger& logger);

    bool connect() override;
    void disconnect() override;
    bool isConnected() const override;
    bool send(const uint8_t* data, std::size_t len) override;
    bool readByte(uint8_t& byte) override;
    std::string describe() const override;

private:
    std::string host_;
    int port_;
    Logger& logger_;
    int socket_fd_ = -1;
    std::atomic<bool> connected_{false};
};

} // namespace mcls
