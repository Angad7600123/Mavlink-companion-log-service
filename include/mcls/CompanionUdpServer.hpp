#pragma once

#include "mcls/CompanionCommandQueue.hpp"
#include "mcls/Config.hpp"
#include "mcls/Logger.hpp"
#include "mcls/ServiceSnapshot.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace mcls {

/// Runs a background UDP thread that:
///  - binds to bind_host:bind_port (default 127.0.0.1:14541)
///  - sends responses to send_host:send_port (default 127.0.0.1:14540, owned by wfb-ng)
///  - parses JSON companion requests and enqueues commands for the main loop
///  - serves status snapshots and fc.logs pages built by the caller
///  - emits a periodic opaque keepalive to send_host:send_port to keep
///    wfb-ng's listen:// reply address registered (see udpProxyKeepaliveLoop)
///
/// Thread safety: snapshot and fc log cache are updated under their own mutex by
/// the main loop thread; the UDP thread reads them under the same mutex.
class CompanionUdpServer {
public:
    using SnapshotProvider = std::function<ServiceSnapshot()>;
    using FcLogsProvider = std::function<FcLogsPage(int offset, int limit)>;
    /// Evaluates archive.start preconditions against live service state and, if
    /// met, queues the cycle — returning the outcome so the handler can ack/err
    /// accurately. Called on the UDP thread; must be thread-safe.
    using ArchiveStartGate = std::function<ArchiveStartResult()>;

    CompanionUdpServer(const Config::CompanionSettings& settings,
                       Logger& logger,
                       CompanionCommandQueue& commands,
                       SnapshotProvider snapshot_fn,
                       FcLogsProvider fc_logs_fn,
                       ArchiveStartGate archive_start_gate);
    ~CompanionUdpServer();

    CompanionUdpServer(const CompanionUdpServer&) = delete;
    CompanionUdpServer& operator=(const CompanionUdpServer&) = delete;

    bool start();
    /// True after a successful bind and while the RX thread is running.
    bool isListening() const { return running_.load() && socket_fd_ != -1; }
    void stop();

private:
    void rxLoop();
    /// Periodically sends a 1-byte opaque datagram to send_host:send_port.
    /// In wfb-ng's `listen://` udp_proxy the reply address is unknown until a
    /// local process sends first; mcls is otherwise purely reactive, so without
    /// this the GS's first uplink request is dropped and the link deadlocks.
    /// This keepalive primes (and, after a wfb-ng restart, re-primes) that reply
    /// path. It is purely a transport detail — the payload is opaque and is NOT
    /// a companion-protocol message, so the GS never has to parse it.
    void udpProxyKeepaliveLoop();

    Config::CompanionSettings settings_;
    Logger& logger_;
    CompanionCommandQueue& commands_;
    SnapshotProvider snapshot_fn_;
    FcLogsProvider fc_logs_fn_;
    ArchiveStartGate archive_start_gate_;

    std::atomic<bool> running_{false};
    std::thread thread_;
    std::thread keepalive_thread_;
    std::mutex keepalive_mutex_;
    std::condition_variable keepalive_cv_;
    int socket_fd_ = -1;
};

} // namespace mcls
