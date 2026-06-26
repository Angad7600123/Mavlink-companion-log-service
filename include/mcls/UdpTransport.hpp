#pragma once

#include "mcls/Transport.hpp"

#include "SocketPlatform.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <string>

namespace mcls {

class Logger;

/// MAVLink transport over UDP datagrams to host:port.
class UdpTransport : public Transport {
public:
    UdpTransport(std::string host,
                 int port,
                 std::string bind_host,
                 int bind_port,
                 Logger& logger);

    bool connect() override;
    void disconnect() override;
    bool isConnected() const override;
    bool send(const uint8_t* data, std::size_t len) override;
    bool readByte(uint8_t& byte) override;
    std::string describe() const override;

private:
    static constexpr std::size_t kMaxDatagramSize = 2048;

    bool refillRxBuffer();

    std::string host_;
    int port_;
    std::string bind_host_;
    int bind_port_;
    Logger& logger_;
    int socket_fd_ = -1;
    std::atomic<bool> connected_{false};
    bool remote_resolved_ = false;
    sockaddr_storage remote_addr_{};
    int remote_addr_len_ = 0;
    std::array<uint8_t, kMaxDatagramSize> rx_buffer_{};
    std::size_t rx_buffer_len_ = 0;
    std::size_t rx_buffer_pos_ = 0;
};

} // namespace mcls
