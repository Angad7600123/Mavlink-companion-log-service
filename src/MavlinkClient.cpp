#include "mcls/MavlinkClient.hpp"
#include "mcls/Logger.hpp"

#include <ardupilotmega/mavlink.h>

#include <chrono>
#include <cstring>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline int closeSocket(socket_t s) {
    return closesocket(s);
}
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
inline int closeSocket(socket_t s) {
    return close(s);
}
#endif

namespace mcls {

namespace {

#ifdef _WIN32
struct WinsockInit {
    WinsockInit() {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WinsockInit() {
        WSACleanup();
    }
};
#endif

} // namespace

MavlinkClient::MavlinkClient(std::string host,
                             int port,
                             int heartbeat_timeout_sec,
                             Logger& logger)
    : host_(std::move(host)),
      port_(port),
      heartbeat_timeout_sec_(heartbeat_timeout_sec),
      logger_(logger) {
#ifdef _WIN32
    static WinsockInit winsock_init;
#endif
    last_heartbeat_.store(std::chrono::steady_clock::now());
}

MavlinkClient::~MavlinkClient() {
    disconnect();
}

bool MavlinkClient::connect() {
    disconnect();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    const std::string port_str = std::to_string(port_);
    if (getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &result) != 0) {
        logger_.error("Failed to resolve host: " + host_);
        return false;
    }

    socket_fd_ = kInvalidSocket;
    for (addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        socket_fd_ = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (socket_fd_ == kInvalidSocket) {
            continue;
        }
        if (::connect(socket_fd_, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) == 0) {
            break;
        }
        closeSocket(socket_fd_);
        socket_fd_ = kInvalidSocket;
    }
    freeaddrinfo(result);

    if (socket_fd_ == kInvalidSocket) {
        logger_.error("Failed to connect to mavlink-router at " + host_ + ":" + port_str);
        return false;
    }

    connected_.store(true);
    running_.store(true);
    rx_thread_ = std::thread(&MavlinkClient::rxLoop, this);
    logger_.info("Connected to mavlink-router at " + host_ + ":" + port_str);
    return true;
}

void MavlinkClient::disconnect() {
    running_.store(false);
    connected_.store(false);

    if (socket_fd_ != kInvalidSocket) {
        closeSocket(socket_fd_);
        socket_fd_ = kInvalidSocket;
    }

    if (rx_thread_.joinable()) {
        rx_thread_.join();
    }
}

bool MavlinkClient::isConnected() const {
    return connected_.load();
}

bool MavlinkClient::sendMessage(const mavlink_message_t& msg) {
    if (!connected_.load()) {
        return false;
    }

    uint8_t buffer[MAVLINK_MAX_PACKET_LEN];
    const uint16_t len = mavlink_msg_to_send_buffer(buffer, const_cast<mavlink_message_t*>(&msg));

    std::lock_guard lock(send_mutex_);
    const int sent = ::send(socket_fd_, reinterpret_cast<const char*>(buffer),
                            static_cast<int>(len), 0);
    return sent == static_cast<int>(len);
}

void MavlinkClient::setMessageHandler(MessageHandler handler) {
    std::lock_guard lock(handler_mutex_);
    handler_ = std::move(handler);
}

bool MavlinkClient::waitForHeartbeat(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (heartbeatFresh()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

bool MavlinkClient::heartbeatFresh() const {
    const auto last = last_heartbeat_.load();
    const auto age = std::chrono::steady_clock::now() - last;
    return age <= std::chrono::seconds(heartbeat_timeout_sec_);
}

void MavlinkClient::rxLoop() {
    mavlink_message_t msg{};
    mavlink_status_t status{};

    while (running_.load()) {
        uint8_t byte = 0;
        const int n = ::recv(socket_fd_, reinterpret_cast<char*>(&byte), 1, 0);
        if (n <= 0) {
            if (running_.load()) {
                logger_.warn("MAVLink connection closed");
                connected_.store(false);
            }
            break;
        }

        if (mavlink_parse_char(MAVLINK_COMM_0, byte, &msg, &status)) {
            handleParsedMessage(msg);
        }
    }
}

void MavlinkClient::handleParsedMessage(const mavlink_message_t& msg) {
    if (msg.msgid == MAVLINK_MSG_ID_HEARTBEAT) {
        last_heartbeat_.store(std::chrono::steady_clock::now());
        target_system_ = msg.sysid;
        target_component_ = msg.compid;
    }

    std::lock_guard lock(handler_mutex_);
    if (handler_) {
        handler_(msg);
    }
}

} // namespace mcls
