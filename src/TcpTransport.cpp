#include "mcls/TcpTransport.hpp"
#include "mcls/Logger.hpp"

#include "SocketPlatform.hpp"

namespace mcls {

TcpTransport::TcpTransport(std::string host, int port, Logger& logger)
    : host_(std::move(host)), port_(port), logger_(logger) {
    detail::ensureWinsock();
}

bool TcpTransport::connect() {
    disconnect();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    const std::string port_str = std::to_string(port_);
    if (getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &result) != 0) {
        logger_.error("TCP transport: failed to resolve " + host_);
        return false;
    }

    native_socket_t fd = kInvalidSocket;
    for (addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == kInvalidSocket) {
            continue;
        }
        if (::connect(fd, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) == 0) {
            break;
        }
        closeSocket(fd);
        fd = kInvalidSocket;
    }
    freeaddrinfo(result);

    if (fd == kInvalidSocket) {
        logger_.error("TCP transport: failed to connect to " + describe());
        return false;
    }

    socket_fd_ = static_cast<int>(fd);
    connected_.store(true);
    logger_.info("TCP transport connected to " + describe());
    return true;
}

void TcpTransport::disconnect() {
    connected_.store(false);
    if (socket_fd_ != -1) {
        closeSocket(static_cast<native_socket_t>(socket_fd_));
        socket_fd_ = -1;
    }
}

bool TcpTransport::isConnected() const {
    return connected_.load();
}

bool TcpTransport::send(const uint8_t* data, std::size_t len) {
    if (!connected_.load() || socket_fd_ == -1) {
        return false;
    }
    const int sent =
        ::send(static_cast<native_socket_t>(socket_fd_), reinterpret_cast<const char*>(data),
               static_cast<int>(len), 0);
    return sent == static_cast<int>(len);
}

bool TcpTransport::readByte(uint8_t& byte) {
    if (!connected_.load() || socket_fd_ == -1) {
        return false;
    }
    const int n = ::recv(static_cast<native_socket_t>(socket_fd_),
                         reinterpret_cast<char*>(&byte), 1, 0);
    if (n <= 0) {
        connected_.store(false);
        return false;
    }
    return true;
}

std::string TcpTransport::describe() const {
    return host_ + ":" + std::to_string(port_);
}

} // namespace mcls
