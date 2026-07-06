#pragma once

#include <ardupilotmega/mavlink.h>

#include "mcls/CompanionCommandQueue.hpp"
#include "mcls/CompanionUdpServer.hpp"
#include "mcls/Config.hpp"
#include "mcls/Database.hpp"
#include "mcls/FlightMonitor.hpp"
#include "mcls/LogDownloader.hpp"
#include "mcls/Logger.hpp"
#include "mcls/MavlinkClient.hpp"
#include "mcls/ServiceSnapshot.hpp"
#include "mcls/StorageManager.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace mcls {

/// Main service state machine coordinating flight detection and log archival.
class DroneLogService {
public:
    DroneLogService(Config config, std::string config_path);
    ~DroneLogService();

    void run();
    void stop();

private:
    enum class State {
        Boot,
        Connect,
        WaitHeartbeat,
        WaitArm,
        WaitDisarm,
        Delay,
        Enumerate,
        Archive,
        EraseAll,
        Cleanup,
        ConnectionLost,
    };

    Config config_;
    std::string config_path_;
    Logger logger_;
    StorageManager storage_;
    Database database_;
    MavlinkClient client_;
    FlightMonitor monitor_;
    LogDownloader downloader_;

    std::atomic<bool> running_{true};
    std::atomic<bool> archive_requested_{false};
    std::atomic<State> state_{State::Boot};
    std::chrono::steady_clock::time_point disarm_time_{};
    ArchiveCycleResult last_cycle_result_{};
    int consecutive_archive_failures_ = 0;

    void setState(State state);
    State currentState() const { return state_.load(); }
    bool isArchiveBusy(State state) const;
    void processState();
    void onFlightEvent(FlightMonitor::Event event);
    void onMavlinkMessage(const mavlink_message_t& msg);
    void handleConnectionLost();
    bool reconnect();
    void evaluateArchiveOutcome(const ArchiveCycleResult& result);
    void logStatistics() const;

    // Companion API helpers
    void drainCompanionCommands();
    /// Evaluates archive.start preconditions against live state and queues a
    /// cycle iff accepted. Called on the companion UDP thread. Idempotent by
    /// state: returns Busy (not a second queued cycle) while one is in flight.
    ArchiveStartResult requestManualArchive();
    ServiceSnapshot buildSnapshot() const;
    FcLogsPage buildFcLogsPage(int offset, int limit) const;
    void cacheEnumerationResult(const std::vector<LogEntry>& entries);
    static std::string stateToString(State state);

    // Companion API members
    mutable std::mutex snapshot_mutex_;
    std::vector<LogEntry> cached_fc_logs_;
    bool fc_logs_stale_ = true;
    CompanionCommandQueue companion_commands_;
    std::unique_ptr<CompanionUdpServer> companion_server_;
};

} // namespace mcls
