#include "mcls/CompanionUdpServer.hpp"

#include "mcls/CompanionDiagnostics.hpp"
#include "mcls/CompanionProtocol.hpp"
#include "SocketPlatform.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
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
                   const CompanionUdpServer::JobGate& job_gate,
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
                                       FcLogsProvider fc_logs_fn,
                                       JobGate job_gate)
    : settings_(settings),
      logger_(logger),
      commands_(commands),
      snapshot_fn_(std::move(snapshot_fn)),
      fc_logs_fn_(std::move(fc_logs_fn)),
      job_gate_(std::move(job_gate)) {
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

    if (settings_.udp_proxy_keepalive_ms > 0) {
        keepalive_thread_ = std::thread(&CompanionUdpServer::udpProxyKeepaliveLoop, this);
        logger_.info("CompanionUdpServer: udp_proxy keepalive enabled (every " +
                     std::to_string(settings_.udp_proxy_keepalive_ms) + " ms to " +
                     settings_.send_host + ":" + std::to_string(settings_.send_port) + ")");
    } else {
        logger_.info("CompanionUdpServer: udp_proxy keepalive disabled "
                     "(udp_proxy_keepalive_ms=0) — wfb-ng listen:// uplink will not "
                     "deliver until mcls sends first");
    }
    return true;
}

void CompanionUdpServer::stop() {
    if (!running_.load() && socket_fd_ == -1) {
        return;
    }

    logger_.info("CompanionUdpServer: stopping RX thread");
    running_.store(false);

    // Wake and join the keepalive thread before closing the socket so it never
    // calls sendto() on a closed fd.
    keepalive_cv_.notify_all();
    if (keepalive_thread_.joinable()) {
        keepalive_thread_.join();
    }

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
                      snapshot_fn_, fc_logs_fn_, job_gate_, socket_fd_);
    }

    logger_.info("CompanionUdpServer: RX thread exiting");
}

void CompanionUdpServer::udpProxyKeepaliveLoop() {
    // Opaque 1-byte datagram. Its ONLY job is to make wfb-ng's `listen://`
    // udp_proxy learn mcls's address (it delivers GS uplink to the last local
    // sender). The content is meaningless on purpose — deliberately NOT a
    // companion-protocol message — so the GS never has to understand it; it can
    // treat anything that isn't a valid id-matched response as noise and drop it.
    static constexpr std::uint8_t kKeepalive = 0x00;

    // Resolve the send target once; this fires on a timer.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    const std::string send_port_str = std::to_string(settings_.send_port);
    addrinfo* result = nullptr;
    const int gai_rc =
        getaddrinfo(settings_.send_host.c_str(), send_port_str.c_str(), &hints, &result);
    if (gai_rc != 0) {
        logger_.error("CompanionUdpServer: keepalive getaddrinfo(" + settings_.send_host + ":" +
                      send_port_str + ") failed: " + std::string(gai_strerror(gai_rc)) +
                      " — udp_proxy priming disabled");
        return;
    }
    sockaddr_storage send_addr{};
    std::memcpy(&send_addr, result->ai_addr, result->ai_addrlen);
    const auto send_addr_len = result->ai_addrlen;
    freeaddrinfo(result);

    logger_.info("CompanionUdpServer: udp_proxy keepalive thread started (priming " +
                 settings_.send_host + ":" + send_port_str + ")");

    bool logged_first = false;
    std::uint64_t consecutive_failures = 0;
    while (running_.load()) {
        const int sent = ::sendto(static_cast<native_socket_t>(socket_fd_),
                                  reinterpret_cast<const char*>(&kKeepalive),
                                  1,
                                  0,
                                  reinterpret_cast<const sockaddr*>(&send_addr),
                                  static_cast<int>(send_addr_len));
        if (sent < 0) {
            // Rate-limit noise (e.g. wfb-ng not up yet): log first, then sparsely.
            if (running_.load() && (consecutive_failures++ % 30 == 0)) {
                logger_.warn("CompanionUdpServer: keepalive sendto(" + settings_.send_host + ":" +
                             send_port_str + ") failed: " +
                             CompanionDiagnostics::socketErrorMessage());
            }
        } else {
            consecutive_failures = 0;
            if (!logged_first) {
                logged_first = true;
                logger_.info("CompanionUdpServer: udp_proxy reply path primed (" +
                             settings_.send_host + ":" + send_port_str + ")");
            }
        }

        std::unique_lock<std::mutex> lock(keepalive_mutex_);
        keepalive_cv_.wait_for(lock,
                               std::chrono::milliseconds(settings_.udp_proxy_keepalive_ms),
                               [this] { return !running_.load(); });
    }

    logger_.info("CompanionUdpServer: udp_proxy keepalive thread exiting");
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

/// Sends the ack/err for a job-style op (archive.start, logs.refresh,
/// logs.erase, logs.download) given the gate's outcome.
void respondToJobOutcome(const CompanionRequest& req,
                         const char* op,
                         const JobOutcome& outcome,
                         const Config::CompanionSettings& settings,
                         Logger& logger,
                         const int socket_fd) {
    SerializedResponse resp;
    switch (outcome.result) {
    case JobStartResult::Accepted:
        logger.info("CompanionUdpServer: " + std::string(op) + " id=" + std::to_string(req.id) +
                    " accepted (queued=" + std::to_string(outcome.queued) + ")");
        if (req.op == "logs.download") {
            resp = CompanionProtocol::buildDownloadAck(req.id, outcome.queued, outcome.not_found,
                                                       req.client);
        } else {
            resp = CompanionProtocol::buildJobAck(req.id, /*already_running=*/false, req.client);
        }
        break;
    case JobStartResult::AlreadyRunning:
        // Idempotent success: the job the client asked for is already running,
        // so a retry after a lost ack must read as success, never an error.
        logger.info("CompanionUdpServer: " + std::string(op) + " id=" + std::to_string(req.id) +
                    " already running (idempotent ack)");
        resp = CompanionProtocol::buildJobAck(req.id, /*already_running=*/true, req.client);
        break;
    case JobStartResult::Armed:
        logger.info("CompanionUdpServer: " + std::string(op) + " id=" + std::to_string(req.id) +
                    " rejected (armed)");
        resp = CompanionProtocol::buildError(req.id, "armed", req.client);
        break;
    case JobStartResult::NotConnected:
        logger.info("CompanionUdpServer: " + std::string(op) + " id=" + std::to_string(req.id) +
                    " rejected (not_connected)");
        resp = CompanionProtocol::buildError(req.id, "not_connected", req.client);
        break;
    case JobStartResult::NotFound:
        logger.info("CompanionUdpServer: " + std::string(op) + " id=" + std::to_string(req.id) +
                    " rejected (not_found)");
        resp = CompanionProtocol::buildError(req.id, "not_found", req.client);
        break;
    }
    if (!sendToWfb(resp.json, settings, logger, socket_fd, req.id, op)) {
        logger.error("CompanionUdpServer: failed to send " + std::string(op) + " response id=" +
                     std::to_string(req.id));
    }
}

void handleRequest(const std::string& json_text,
                   const sockaddr_storage& from,
                   const std::uint32_t from_len,
                   const Config::CompanionSettings& settings,
                   Logger& logger,
                   CompanionCommandQueue& commands,
                   const CompanionUdpServer::SnapshotProvider& snapshot_fn,
                   const CompanionUdpServer::FcLogsProvider& fc_logs_fn,
                   const CompanionUdpServer::JobGate& job_gate,
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

    const bool is_mutating = (req.op == "archive.start" || req.op == "archive.cancel" ||
                              req.op == "logs.refresh" || req.op == "logs.download" ||
                              req.op == "logs.erase");
    if (is_mutating) {
        logger.info("CompanionUdpServer: request op=" + req.op + " id=" + std::to_string(req.id));
    } else {
        logger.debug("CompanionUdpServer: request op=" + req.op + " id=" + std::to_string(req.id));
    }

    if (is_mutating && !settings.token.empty() && req.token != settings.token) {
        logger.warn("CompanionUdpServer: op=" + req.op + " id=" + std::to_string(req.id) +
                    " rejected (bad_token)");
        const auto resp = CompanionProtocol::buildError(req.id, "bad_token", req.client);
        if (!sendToWfb(resp.json, settings, logger, socket_fd, req.id, req.op.c_str())) {
            logger.error("CompanionUdpServer: failed to send bad_token response for id=" +
                         std::to_string(req.id));
        }
        return;
    }

    const std::size_t max_bytes = static_cast<std::size_t>(settings.max_response_bytes);

    if (req.op == "status") {
        const ServiceSnapshot snap = snapshot_fn();
        const auto resp = CompanionProtocol::buildStatus(req.id, snap, max_bytes, req.client);
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
        const auto resp = CompanionProtocol::buildFcLogs(req.id, page, max_bytes, req.client);
        if (resp.truncated) {
            logger.info("CompanionUdpServer: fc.logs id=" + std::to_string(req.id) +
                        " truncated (page reduced to fit budget)");
        }
        if (!sendToWfb(resp.json, settings, logger, socket_fd, req.id, "fc.logs")) {
            logger.error("CompanionUdpServer: failed to send fc.logs response id=" +
                         std::to_string(req.id));
        }

    } else if (req.op == "caps") {
        const auto resp = CompanionProtocol::buildCaps(
            req.id, settings.max_request_bytes, settings.max_response_bytes,
            settings.max_fc_logs_per_response, max_bytes, req.client);
        if (!sendToWfb(resp.json, settings, logger, socket_fd, req.id, "caps")) {
            logger.error("CompanionUdpServer: failed to send caps response id=" +
                         std::to_string(req.id));
        }

    } else if (req.op == "archive.start") {
        respondToJobOutcome(req, "archive.start",
                            job_gate(CompanionJobKind::Archive, {}, false), settings, logger,
                            socket_fd);

    } else if (req.op == "logs.refresh") {
        respondToJobOutcome(req, "logs.refresh",
                            job_gate(CompanionJobKind::Refresh, {}, false), settings, logger,
                            socket_fd);

    } else if (req.op == "logs.download") {
        if (!req.sel_all && req.sel_ids.empty()) {
            logger.warn("CompanionUdpServer: logs.download id=" + std::to_string(req.id) +
                        " rejected (empty selection)");
            const auto resp = CompanionProtocol::buildError(req.id, "bad_request", req.client);
            sendToWfb(resp.json, settings, logger, socket_fd, req.id, "logs.download");
            return;
        }
        if (static_cast<int>(req.sel_ids.size()) > CompanionProtocol::kMaxIdsPerRequest) {
            logger.warn("CompanionUdpServer: logs.download id=" + std::to_string(req.id) +
                        " rejected (too many ids: " + std::to_string(req.sel_ids.size()) + ")");
            const auto resp = CompanionProtocol::buildError(req.id, "bad_request", req.client);
            sendToWfb(resp.json, settings, logger, socket_fd, req.id, "logs.download");
            return;
        }
        respondToJobOutcome(req, "logs.download",
                            job_gate(CompanionJobKind::Download, req.sel_ids, req.sel_all),
                            settings, logger, socket_fd);

    } else if (req.op == "logs.erase") {
        // Super-delete: unconditional full FC DataFlash wipe. The UI carries the
        // confirmation dialog; the server intentionally has no extra gate here
        // beyond transport connectivity (cannot transmit otherwise).
        respondToJobOutcome(req, "logs.erase",
                            job_gate(CompanionJobKind::EraseAll, {}, false), settings, logger,
                            socket_fd);

    } else if (req.op == "archive.cancel") {
        commands.push(CancelArchiveCommand{});
        logger.info("CompanionUdpServer: queued archive.cancel id=" + std::to_string(req.id));
        const auto resp = CompanionProtocol::buildAck(req.id, true, {}, req.client);
        if (!sendToWfb(resp.json, settings, logger, socket_fd, req.id, "archive.cancel")) {
            logger.error("CompanionUdpServer: failed to send archive.cancel ack id=" +
                         std::to_string(req.id));
        }

    } else {
        logger.warn("CompanionUdpServer: unknown op=\"" + req.op + "\" id=" +
                    std::to_string(req.id));
        const auto resp = CompanionProtocol::buildError(req.id, "bad_request", req.client);
        if (!sendToWfb(resp.json, settings, logger, socket_fd, req.id, req.op.c_str())) {
            logger.error("CompanionUdpServer: failed to send bad_request response id=" +
                         std::to_string(req.id));
        }
    }
}

} // namespace

} // namespace mcls
