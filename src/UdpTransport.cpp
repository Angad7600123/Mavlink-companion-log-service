#include "mcls/UdpTransport.hpp"
#include "mcls/Logger.hpp"

#include "SocketPlatform.hpp"

#include <cstring>

namespace mcls {

UdpTransport::UdpTransport(std::string host,
                           int port,
                           std::string bind_host,
                           int bind_port,
                           Logger& logger)
    : host_(std::move(host)),
      port_(port),
      bind_host_(std::move(bind_host)),
      bind_port_(bind_port),
      logger_(logger) {
    detail::ensureWinsock();
}

bool UdpTransport::connect() {
    disconnect();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    const std::string port_str = std::to_string(port_);
    addrinfo* remote_result = nullptr;
    if (getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &remote_result) != 0) {
        logger_.error("UDP transport: failed to resolve remote " + host_);
        return false;
    }

    native_socket_t fd = ::socket(remote_result->ai_family, remote_result->ai_socktype,
                                  remote_result->ai_protocol);
    if (fd == kInvalidSocket) {
        freeaddrinfo(remote_result);
        logger_.error("UDP transport: failed to create socket");
        return false;
    }

    std::memcpy(&remote_addr_, remote_result->ai_addr, remote_result->ai_addrlen);
    remote_addr_len_ = static_cast<int>(remote_result->ai_addrlen);
    remote_resolved_ = true;
    freeaddrinfo(remote_result);

    if (!bind_host_.empty()) {
        addrinfo bind_hints{};
        bind_hints.ai_family = AF_UNSPEC;
        bind_hints.ai_socktype = SOCK_DGRAM;
        bind_hints.ai_flags = AI_PASSIVE;

        const std::string bind_port_str =
            bind_port_ > 0 ? std::to_string(bind_port_) : "0";
        addrinfo* bind_result = nullptr;
        if (getaddrinfo(bind_host_.c_str(), bind_port_str.c_str(), &bind_hints,
                        &bind_result) != 0) {
            closeSocket(fd);
            logger_.error("UDP transport: failed to resolve bind address " + bind_host_);
            return false;
        }

        const int bind_rc =
            ::bind(fd, bind_result->ai_addr, static_cast<int>(bind_result->ai_addrlen));
        freeaddrinfo(bind_result);
        if (bind_rc != 0) {
            closeSocket(fd);
            logger_.error("UDP transport: failed to bind " + bind_host_ + ":" + bind_port_str);
            return false;
        }
    }

    socket_fd_ = static_cast<int>(fd);
    connected_.store(true);
    logger_.info("UDP transport ready: " + describe());
    return true;
}

void UdpTransport::disconnect() {
    connected_.store(false);
    remote_resolved_ = false;
    remote_addr_len_ = 0;
    if (socket_fd_ != -1) {
        closeSocket(static_cast<native_socket_t>(socket_fd_));
        socket_fd_ = -1;
    }
}

bool UdpTransport::isConnected() const {
    return connected_.load();
}

bool UdpTransport::send(const uint8_t* data, std::size_t len) {
    if (!connected_.load() || socket_fd_ == -1 || !remote_resolved_) {
        return false;
    }
    const int sent = ::sendto(static_cast<native_socket_t>(socket_fd_),
                              reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
                              reinterpret_cast<sockaddr*>(&remote_addr_), remote_addr_len_);
    return sent == static_cast<int>(len);
}

bool UdpTransport::readByte(uint8_t& byte) {
    if (!connected_.load() || socket_fd_ == -1) {
        return false;
    }

    sockaddr_storage from{};
#ifdef _WIN32
    int from_len = sizeof(from);
#else
    socklen_t from_len = sizeof(from);
#endif
    const int n = ::recvfrom(static_cast<native_socket_t>(socket_fd_),
                             reinterpret_cast<char*>(&byte), 1, 0,
                             reinterpret_cast<sockaddr*>(&from),
#ifdef _WIN32
                             &from_len
#else
                             &from_len
#endif
                             );
    if (n <= 0) {
        connected_.store(false);
        return false;
    }

    if (!remote_resolved_) {
        std::memcpy(&remote_addr_, &from, from_len);
        remote_addr_len_ = static_cast<int>(from_len);
        remote_resolved_ = true;
    }
    return true;
}

std::string UdpTransport::describe() const {
    std::string desc = "udp://" + host_ + ":" + std::to_string(port_);
    if (!bind_host_.empty()) {
        desc += " (bind " + bind_host_;
        if (bind_port_ > 0) {
            desc += ":" + std::to_string(bind_port_);
        }
        desc += ")";
    }
    return desc;
}

} // namespace mcls
