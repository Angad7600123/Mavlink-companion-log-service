#pragma once

#include <ardupilotmega/mavlink.h>

#include "mcls/Config.hpp"
#include "mcls/Database.hpp"
#include "mcls/FlightMonitor.hpp"
#include "mcls/LogDownloader.hpp"
#include "mcls/Logger.hpp"
#include "mcls/MavlinkClient.hpp"
#include "mcls/StorageManager.hpp"

#include <atomic>
#include <chrono>
#include <thread>

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
    State state_ = State::Boot;
    std::chrono::steady_clock::time_point disarm_time_{};

    void setState(State state);
    void processState();
    void onFlightEvent(FlightMonitor::Event event);
    void onMavlinkMessage(const mavlink_message_t& msg);
    void handleConnectionLost();
    bool reconnect();
    void logStatistics() const;
};

} // namespace mcls
