#include "mcls/CompanionUdpServer.hpp"

#include "mcls/CompanionDiagnostics.hpp"
#include "mcls/CompanionProtocol.hpp"
#include "SocketPlatform.hpp"

#include <cerrno>
#include <cstring>
#include <string>

namespace mcls {

namespace {

void handleRequest(const std::string& json_text,
                   const sockaddr_storage& from,
                   std::uint32_t from_len,
                   const Config::CompanionSettings& settings,
                   Logger& logger,
                   CompanionCommandQueue& commands,
                   const CompanionUdpServer::SnapshotProvider& snapshot_fn,
                   const CompanionUdpServer::FcLogsProvider& fc_logs_fn,
                   int socket_fd);

bool sendToWfb(const std::string& json,
               const Config::CompanionSettings& settings,
               Logger& logger,
               int socket_fd,
               int request_id,
               const char* op);

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
    logger_.debug("CompanionUdpServer constructed");
}

CompanionUdpServer::~CompanionUdpServer() {
    stop();
}

bool CompanionUdpServer::start() {
    if (running_.load()) {
        logger_.warn("CompanionUdpServer: start() called but already running on " +
                     settings_.bind_host + ":" + std::to_string(settings_.bind_port));
        return true;
    }

    logger_.info("CompanionUdpServer: opening UDP socket for bind " + settings_.bind_host + ":" +
                 std::to_string(settings_.bind_port));

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    const std::string bind_port_str = std::to_string(settings_.bind_port);
    addrinfo* result = nullptr;
    const int gai_rc =
        getaddrinfo(settings_.bind_host.c_str(), bind_port_str.c_str(), &hints, &result);
    if (gai_rc != 0) {
        logger_.error("CompanionUdpServer: getaddrinfo(bind " + settings_.bind_host + ":" +
                      bind_port_str + ") failed: " + std::string(gai_strerror(gai_rc)));
        return false;
    }

    const native_socket_t fd =
        ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd == kInvalidSocket) {
        freeaddrinfo(result);
        logger_.error("CompanionUdpServer: socket() failed: " +
                      CompanionDiagnostics::socketErrorMessage());
        return false;
    }

    const int bind_rc = ::bind(fd, result->ai_addr, static_cast<int>(result->ai_addrlen));
    freeaddrinfo(result);
    if (bind_rc != 0) {
        logger_.error("CompanionUdpServer: bind(" + settings_.bind_host + ":" + bind_port_str +
                      ") failed: " + CompanionDiagnostics::socketErrorMessage() +
                      " — is another process using this port? (wfb-ng uses 14540, mcls uses 14541)");
        closeSocket(fd);
        return false;
    }

    socket_fd_ = static_cast<int>(fd);
    running_.store(true);
    thread_ = std::thread(&CompanionUdpServer::rxLoop, this);

    logger_.info("Companion UDP server listening on " + settings_.bind_host + ":" + bind_port_str +
                 ", sending responses to " + settings_.send_host + ":" +
                 std::to_string(settings_.send_port) +
                 " (wfb-ng companion stream 0xc0 up / 0x40 down)");
    return true;
}

void CompanionUdpServer::stop() {
    if (!running_.load() && socket_fd_ == -1) {
        return;
    }

    logger_.info("CompanionUdpServer: stopping RX thread");
    running_.store(false);

    if (socket_fd_ != -1) {
        closeSocket(static_cast<native_socket_t>(socket_fd_));
        socket_fd_ = -1;
    }

    if (thread_.joinable()) {
        thread_.join();
    }

    logger_.info("CompanionUdpServer: stopped");
}

void CompanionUdpServer::rxLoop() {
    logger_.info("CompanionUdpServer: RX thread started, waiting for datagrams on fd " +
                 std::to_string(socket_fd_));

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
                if (n < 0) {
                    logger_.warn("CompanionUdpServer: recvfrom failed: " +
                                 CompanionDiagnostics::socketErrorMessage());
                } else {
                    logger_.debug("CompanionUdpServer: recvfrom returned 0");
                }
            }
            break;
        }

        buf[static_cast<std::size_t>(n)] = '\0';
        handleRequest(buf.substr(0, static_cast<std::size_t>(n)), from,
                      static_cast<std::uint32_t>(from_len), settings_, logger_, commands_,
                      snapshot_fn_, fc_logs_fn_, socket_fd_);
    }

    logger_.info("CompanionUdpServer: RX thread exiting");
}

namespace {

bool sendToWfb(const std::string& json,
               const Config::CompanionSettings& settings,
               Logger& logger,
               const int socket_fd,
               const int request_id,
               const char* op) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    const std::string send_port_str = std::to_string(settings.send_port);
    addrinfo* result = nullptr;
    const int gai_rc =
        getaddrinfo(settings.send_host.c_str(), send_port_str.c_str(), &hints, &result);
    if (gai_rc != 0) {
        logger.error("CompanionUdpServer: getaddrinfo(send " + settings.send_host + ":" +
                     send_port_str + ") failed for op=" + std::string(op) +
                     " id=" + std::to_string(request_id) + ": " + std::string(gai_strerror(gai_rc)));
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
        logger.error("CompanionUdpServer: sendto(" + settings.send_host + ":" + send_port_str +
                     ") failed for op=" + std::string(op) + " id=" + std::to_string(request_id) +
                     " bytes=" + std::to_string(json.size()) + ": " +
                     CompanionDiagnostics::socketErrorMessage());
        return false;
    }

    logger.debug("CompanionUdpServer: sent op=" + std::string(op) + " id=" +
                 std::to_string(request_id) + " " + std::to_string(json.size()) + " bytes to " +
                 settings.send_host + ":" + send_port_str);
    return true;
}

void handleRequest(const std::string& json_text,
                   const sockaddr_storage& from,
                   const std::uint32_t from_len,
                   const Config::CompanionSettings& settings,
                   Logger& logger,
                   CompanionCommandQueue& commands,
                   const CompanionUdpServer::SnapshotProvider& snapshot_fn,
                   const CompanionUdpServer::FcLogsProvider& fc_logs_fn,
                   const int socket_fd) {
    CompanionDiagnostics::logUdpPeer(logger, "CompanionUdpServer: datagram", from, from_len);

    if (static_cast<int>(json_text.size()) > settings.max_request_bytes) {
        logger.warn("CompanionUdpServer: request too large (" + std::to_string(json_text.size()) +
                    " bytes, max " + std::to_string(settings.max_request_bytes) + "), dropping");
        return;
    }

    const auto req_opt = CompanionProtocol::parseRequest(json_text);
    if (!req_opt) {
        logger.warn("CompanionUdpServer: failed to parse JSON request (" +
                    std::to_string(json_text.size()) + " bytes): " +
                    (json_text.size() > 120 ? json_text.substr(0, 120) + "..." : json_text));
        return;
    }
    const auto& req = *req_opt;

    const bool is_mutating = (req.op == "archive.start" || req.op == "archive.cancel");
    if (is_mutating) {
        logger.info("CompanionUdpServer: request op=" + req.op + " id=" + std::to_string(req.id));
    } else {
        logger.debug("CompanionUdpServer: request op=" + req.op + " id=" + std::to_string(req.id));
    }

    if (is_mutating && !settings.token.empty() && req.token != settings.token) {
        logger.warn("CompanionUdpServer: op=" + req.op + " id=" + std::to_string(req.id) +
                    " rejected (bad_token)");
        const auto resp = CompanionProtocol::buildError(req.id, "bad_token");
        if (!sendToWfb(resp.json, settings, logger, socket_fd, req.id, req.op.c_str())) {
            logger.error("CompanionUdpServer: failed to send bad_token response for id=" +
                         std::to_string(req.id));
        }
        return;
    }

    const std::size_t max_bytes = static_cast<std::size_t>(settings.max_response_bytes);

    if (req.op == "status") {
        const ServiceSnapshot snap = snapshot_fn();
        const auto resp = CompanionProtocol::buildStatus(req.id, snap, max_bytes);
        if (resp.truncated) {
            logger.warn("CompanionUdpServer: status id=" + std::to_string(req.id) +
                        " truncated to fit " + std::to_string(max_bytes) + " byte budget");
        }
        if (!sendToWfb(resp.json, settings, logger, socket_fd, req.id, "status")) {
            logger.error("CompanionUdpServer: failed to send status response id=" +
                         std::to_string(req.id));
        }

    } else if (req.op == "fc.logs") {
        const int limit = std::min(req.limit, settings.max_fc_logs_per_response);
        logger.debug("CompanionUdpServer: fc.logs id=" + std::to_string(req.id) + " offset=" +
                     std::to_string(req.offset) + " limit=" + std::to_string(limit));
        const FcLogsPage page = fc_logs_fn(req.offset, limit);
        const auto resp = CompanionProtocol::buildFcLogs(req.id, page, max_bytes);
        if (resp.truncated) {
            logger.info("CompanionUdpServer: fc.logs id=" + std::to_string(req.id) +
                        " truncated (page reduced to fit budget)");
        }
        if (!sendToWfb(resp.json, settings, logger, socket_fd, req.id, "fc.logs")) {
            logger.error("CompanionUdpServer: failed to send fc.logs response id=" +
                         std::to_string(req.id));
        }

    } else if (req.op == "archive.start") {
        commands.push(StartArchiveCommand{});
        logger.info("CompanionUdpServer: queued archive.start id=" + std::to_string(req.id));
        const auto resp = CompanionProtocol::buildAck(req.id, true);
        if (!sendToWfb(resp.json, settings, logger, socket_fd, req.id, "archive.start")) {
            logger.error("CompanionUdpServer: failed to send archive.start ack id=" +
                         std::to_string(req.id));
        }

    } else if (req.op == "archive.cancel") {
        commands.push(CancelArchiveCommand{});
        logger.info("CompanionUdpServer: queued archive.cancel id=" + std::to_string(req.id));
        const auto resp = CompanionProtocol::buildAck(req.id, true);
        if (!sendToWfb(resp.json, settings, logger, socket_fd, req.id, "archive.cancel")) {
            logger.error("CompanionUdpServer: failed to send archive.cancel ack id=" +
                         std::to_string(req.id));
        }

    } else {
        logger.warn("CompanionUdpServer: unknown op=\"" + req.op + "\" id=" +
                    std::to_string(req.id));
        const auto resp = CompanionProtocol::buildError(req.id, "bad_request");
        if (!sendToWfb(resp.json, settings, logger, socket_fd, req.id, req.op.c_str())) {
            logger.error("CompanionUdpServer: failed to send bad_request response id=" +
                         std::to_string(req.id));
        }
    }
}

} // namespace

} // namespace mcls
