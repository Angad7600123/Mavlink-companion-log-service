#pragma once

#include "mcls/CompanionCommandQueue.hpp"
#include "mcls/Config.hpp"
#include "mcls/Logger.hpp"
#include "mcls/ServiceSnapshot.hpp"

#include <atomic>
#include <functional>
#include <thread>

namespace mcls {

/// Runs a background UDP thread that:
///  - binds to bind_host:bind_port (default 127.0.0.1:14541)
///  - sends responses to send_host:send_port (default 127.0.0.1:14540, owned by wfb-ng)
///  - parses JSON companion requests and enqueues commands for the main loop
///  - serves status snapshots and fc.logs pages built by the caller
///
/// Thread safety: snapshot and fc log cache are updated under their own mutex by
/// the main loop thread; the UDP thread reads them under the same mutex.
class CompanionUdpServer {
public:
    using SnapshotProvider = std::function<ServiceSnapshot()>;
    using FcLogsProvider = std::function<FcLogsPage(int offset, int limit)>;

    CompanionUdpServer(const Config::CompanionSettings& settings,
                       Logger& logger,
                       CompanionCommandQueue& commands,
                       SnapshotProvider snapshot_fn,
                       FcLogsProvider fc_logs_fn);
    ~CompanionUdpServer();

    CompanionUdpServer(const CompanionUdpServer&) = delete;
    CompanionUdpServer& operator=(const CompanionUdpServer&) = delete;

    void start();
    void stop();

private:
    void rxLoop();

    Config::CompanionSettings settings_;
    Logger& logger_;
    CompanionCommandQueue& commands_;
    SnapshotProvider snapshot_fn_;
    FcLogsProvider fc_logs_fn_;

    std::atomic<bool> running_{false};
    std::thread thread_;
    int socket_fd_ = -1;
};

} // namespace mcls
