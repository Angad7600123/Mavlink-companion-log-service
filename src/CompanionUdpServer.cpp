#include "mcls/CompanionUdpServer.hpp"

#include "mcls/CompanionProtocol.hpp"
#include "SocketPlatform.hpp"

#include <cstring>
#include <string>

namespace mcls {

namespace {

void handleRequest(const std::string& json_text,
                   const Config::CompanionSettings& settings,
                   Logger& logger,
                   CompanionCommandQueue& commands,
                   const CompanionUdpServer::SnapshotProvider& snapshot_fn,
                   const CompanionUdpServer::FcLogsProvider& fc_logs_fn,
                   int socket_fd);

bool sendToWfb(const std::string& json,
               const Config::CompanionSettings& settings,
               Logger& logger,
               int socket_fd);

} // namespace

CompanionUdpServer::CompanionUdpServer(const Config::CompanionSettings& settings,
                                       Logger& logger,
                                       CompanionCommandQueue& commands,
                                       SnapshotProvider snapshot_fn,
                                       FcLogsProvider fc_logs_fn)
    : settings_(settings),
      logger_(logger),
      commands_(commands),
      snapshot_fn_(std::move(snapshot_fn)),
      fc_logs_fn_(std::move(fc_logs_fn)) {
    detail::ensureWinsock();
}

CompanionUdpServer::~CompanionUdpServer() {
    stop();
}

void CompanionUdpServer::start() {
    if (running_.load()) {
        return;
    }

    // Create and bind the RX socket.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    const std::string port_str = std::to_string(settings_.bind_port);
    addrinfo* result = nullptr;
    if (getaddrinfo(settings_.bind_host.c_str(), port_str.c_str(), &hints, &result) != 0) {
        logger_.error("CompanionUdpServer: failed to resolve bind address " +
                      settings_.bind_host + ":" + port_str);
        return;
    }

    const native_socket_t fd =
        ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd == kInvalidSocket) {
        freeaddrinfo(result);
        logger_.error("CompanionUdpServer: failed to create socket");
        return;
    }

    const int rc = ::bind(fd, result->ai_addr, static_cast<int>(result->ai_addrlen));
    freeaddrinfo(result);
    if (rc != 0) {
        closeSocket(fd);
        logger_.error("CompanionUdpServer: failed to bind " + settings_.bind_host + ":" +
                      port_str);
        return;
    }

    socket_fd_ = static_cast<int>(fd);
    running_.store(true);
    thread_ = std::thread(&CompanionUdpServer::rxLoop, this);
    logger_.info("CompanionUdpServer listening on " + settings_.bind_host + ":" + port_str +
                 ", sending to " + settings_.send_host + ":" +
                 std::to_string(settings_.send_port));
}

void CompanionUdpServer::stop() {
    running_.store(false);
    if (socket_fd_ != -1) {
        closeSocket(static_cast<native_socket_t>(socket_fd_));
        socket_fd_ = -1;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void CompanionUdpServer::rxLoop() {
    constexpr std::size_t kBufSize = 4096;
    std::string buf(kBufSize, '\0');

    while (running_.load()) {
        sockaddr_storage from{};
#ifdef _WIN32
        int from_len = sizeof(from);
#else
        socklen_t from_len = sizeof(from);
#endif
        const int n = ::recvfrom(static_cast<native_socket_t>(socket_fd_),
                                 buf.data(),
                                 static_cast<int>(kBufSize - 1),
                                 0,
                                 reinterpret_cast<sockaddr*>(&from),
#ifdef _WIN32
                                 &from_len
#else
                                 &from_len
#endif
        );
        if (n <= 0) {
            if (running_.load()) {
                logger_.warn("CompanionUdpServer: recvfrom error, stopping");
            }
            break;
        }

        buf[static_cast<std::size_t>(n)] = '\0';
        handleRequest(buf.substr(0, static_cast<std::size_t>(n)),
                      settings_, logger_, commands_, snapshot_fn_, fc_logs_fn_,
                      socket_fd_);
    }
}

// ---------------------------------------------------------------------------
// File-static helpers (keep socket types out of the public header)
// ---------------------------------------------------------------------------

namespace {

bool sendToWfb(const std::string& json,
               const Config::CompanionSettings& settings,
               Logger& logger,
               int socket_fd) {
    // Always send responses to the wfb-ng inject port (send_host:send_port),
    // not back to the datagram source, because wfb-ng delivers uplink packets
    // from its own local address, not from Android.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    const std::string port_str = std::to_string(settings.send_port);
    addrinfo* result = nullptr;
    if (getaddrinfo(settings.send_host.c_str(), port_str.c_str(), &hints, &result) != 0) {
        logger.error("CompanionUdpServer: failed to resolve send address " +
                     settings.send_host + ":" + port_str);
        return false;
    }

    const int sent = ::sendto(static_cast<native_socket_t>(socket_fd),
                              json.data(),
                              static_cast<int>(json.size()),
                              0,
                              result->ai_addr,
                              static_cast<int>(result->ai_addrlen));
    freeaddrinfo(result);

    if (sent != static_cast<int>(json.size())) {
        logger.warn("CompanionUdpServer: sendto failed (sent=" + std::to_string(sent) + ")");
        return false;
    }
    return true;
}

void handleRequest(const std::string& json_text,
                   const Config::CompanionSettings& settings,
                   Logger& logger,
                   CompanionCommandQueue& commands,
                   const CompanionUdpServer::SnapshotProvider& snapshot_fn,
                   const CompanionUdpServer::FcLogsProvider& fc_logs_fn,
                   int socket_fd) {
    if (static_cast<int>(json_text.size()) > settings.max_request_bytes) {
        logger.warn("CompanionUdpServer: request too large (" +
                    std::to_string(json_text.size()) + " bytes), dropping");
        return;
    }

    const auto req_opt = CompanionProtocol::parseRequest(json_text);
    if (!req_opt) {
        logger.warn("CompanionUdpServer: failed to parse request");
        return;
    }
    const auto& req = *req_opt;

    // Token check for mutating ops.
    const bool is_mutating = (req.op == "archive.start" || req.op == "archive.cancel");
    if (is_mutating && !settings.token.empty() && req.token != settings.token) {
        const auto resp = CompanionProtocol::buildError(req.id, "bad_token");
        sendToWfb(resp.json, settings, logger, socket_fd);
        return;
    }

    const std::size_t max_bytes = static_cast<std::size_t>(settings.max_response_bytes);

    if (req.op == "status") {
        const ServiceSnapshot snap = snapshot_fn();
        const auto resp = CompanionProtocol::buildStatus(req.id, snap, max_bytes);
        sendToWfb(resp.json, settings, logger, socket_fd);

    } else if (req.op == "fc.logs") {
        const int limit = std::min(req.limit, settings.max_fc_logs_per_response);
        const FcLogsPage page = fc_logs_fn(req.offset, limit);
        const auto resp = CompanionProtocol::buildFcLogs(req.id, page, max_bytes);
        sendToWfb(resp.json, settings, logger, socket_fd);

    } else if (req.op == "archive.start") {
        commands.push(StartArchiveCommand{});
        const auto resp = CompanionProtocol::buildAck(req.id, true);
        sendToWfb(resp.json, settings, logger, socket_fd);

    } else if (req.op == "archive.cancel") {
        commands.push(CancelArchiveCommand{});
        const auto resp = CompanionProtocol::buildAck(req.id, true);
        sendToWfb(resp.json, settings, logger, socket_fd);

    } else {
        const auto resp = CompanionProtocol::buildError(req.id, "bad_request");
        sendToWfb(resp.json, settings, logger, socket_fd);
    }
}

} // namespace

} // namespace mcls
