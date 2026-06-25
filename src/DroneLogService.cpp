#include "mcls/DroneLogService.hpp"

#include <ardupilotmega/mavlink.h>

#include <csignal>
#include <thread>

namespace mcls {

namespace {
std::atomic<bool>* g_running = nullptr;

void signalHandler(int) {
    if (g_running) {
        g_running->store(false);
    }
}
} // namespace

DroneLogService::DroneLogService(Config config, std::string config_path)
    : config_(std::move(config)),
      config_path_(std::move(config_path)),
      logger_("mcls", config_.logging.verbose, config_.logging.file),
      storage_(config_.storage, logger_),
      database_(std::filesystem::path(config_.storage.directory) / "database.sqlite"),
      client_(config_.transport, logger_),
      monitor_(logger_),
      downloader_(client_, storage_, database_, config_.download, logger_) {
    monitor_.setEventHandler([this](FlightMonitor::Event event) { onFlightEvent(event); });
    client_.setMessageHandler([this](const mavlink_message_t& msg) { onMavlinkMessage(msg); });
}

DroneLogService::~DroneLogService() {
    stop();
}

void DroneLogService::run() {
    g_running = &running_;
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    storage_.initialize();
    database_.initialize();

    logger_.info("MAVLink Companion Log Service starting");

    while (running_.load()) {
        processState();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    client_.disconnect();
    logStatistics();
    logger_.info("MAVLink Companion Log Service stopped");
}

void DroneLogService::stop() {
    running_.store(false);
}

void DroneLogService::setState(State state) {
    state_ = state;
}

void DroneLogService::onFlightEvent(FlightMonitor::Event event) {
    switch (event) {
    case FlightMonitor::Event::VehicleArmed:
        if (state_ == State::WaitArm) {
            setState(State::WaitDisarm);
        }
        break;
    case FlightMonitor::Event::VehicleDisarmed:
        disarm_time_ = std::chrono::steady_clock::now();
        archive_requested_.store(true);
        if (state_ == State::WaitDisarm) {
            setState(State::Delay);
        }
        break;
    case FlightMonitor::Event::LinkLost:
        handleConnectionLost();
        break;
    case FlightMonitor::Event::LinkRestored:
        if (state_ == State::ConnectionLost) {
            setState(State::Connect);
        }
        break;
    default:
        break;
    }
}

void DroneLogService::onMavlinkMessage(const mavlink_message_t& msg) {
    downloader_.onMessage(msg);

    if (msg.msgid == MAVLINK_MSG_ID_HEARTBEAT) {
        mavlink_heartbeat_t hb{};
        mavlink_msg_heartbeat_decode(&msg, &hb);
        const bool armed = (hb.base_mode & MAV_MODE_FLAG_SAFETY_ARMED) != 0;
        monitor_.onHeartbeat(msg.sysid, msg.compid, hb.type, hb.autopilot, armed);
    }

    if (!client_.heartbeatFresh()) {
        monitor_.onLinkTimeout();
    }
}

void DroneLogService::handleConnectionLost() {
    if (state_ != State::ConnectionLost && state_ != State::Boot) {
        logger_.warn("Connection lost, will reconnect");
        client_.disconnect();
        setState(State::ConnectionLost);
    }
}

bool DroneLogService::reconnect() {
    logger_.info("Reconnecting to MAVLink transport...");
    if (!client_.connect()) {
        return false;
    }
    return client_.waitForHeartbeat(std::chrono::seconds(10));
}

void DroneLogService::logStatistics() const {
    logger_.info("Statistics: flights_archived=" +
                 std::to_string(database_.getStat("flights_archived")) +
                 ", bytes_downloaded=" + std::to_string(database_.getStat("bytes_downloaded")) +
                 ", archive_failures=" + std::to_string(database_.getStat("archive_failures")) +
                 ", retries=" + std::to_string(database_.getStat("retries")) +
                 ", storage_bytes=" + std::to_string(storage_.totalVerifiedBytes()) +
                 ", last_archive_time=" + std::to_string(database_.getStat("last_archive_time")));
}

void DroneLogService::processState() {
    switch (state_) {
    case State::Boot:
        setState(State::Connect);
        break;

    case State::Connect:
        if (reconnect()) {
            logger_.info("Waiting for vehicle heartbeat...");
            setState(State::WaitHeartbeat);
        } else {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        break;

    case State::WaitHeartbeat:
        if (monitor_.vehicleDetected()) {
            setState(State::WaitArm);
        } else if (!client_.heartbeatFresh()) {
            handleConnectionLost();
        }
        break;

    case State::WaitArm:
        if (!client_.heartbeatFresh()) {
            handleConnectionLost();
        }
        break;

    case State::WaitDisarm:
        if (!client_.heartbeatFresh()) {
            handleConnectionLost();
        }
        break;

    case State::Delay: {
        const auto elapsed =
            std::chrono::steady_clock::now() - disarm_time_;
        if (elapsed >= std::chrono::seconds(config_.download.delay_after_disarm_sec)) {
            logger_.info("Waiting " + std::to_string(config_.download.delay_after_disarm_sec) +
                         " seconds after disarm complete");
            setState(State::Enumerate);
        }
        break;
    }

    case State::Enumerate:
        setState(State::Archive);
        break;

    case State::Archive: {
        const auto logs = downloader_.enumerateLogs();
        const auto result = downloader_.archiveAll(logs);

        logger_.info("Archive cycle: downloaded=" + std::to_string(result.downloaded) +
                     ", skipped=" + std::to_string(result.skipped) +
                     ", failed=" + std::to_string(result.failed));

        if (result.all_archived && config_.download.erase_after_success && !logs.empty()) {
            setState(State::EraseAll);
        } else {
            if (!result.all_archived) {
                logger_.warn("Skipping FC erase because one or more logs failed to archive");
            }
            setState(State::Cleanup);
        }
        break;
    }

    case State::EraseAll:
        if (downloader_.eraseFlightControllerLogs()) {
            setState(State::Cleanup);
        } else {
            logger_.error("Failed to send LOG_ERASE");
            setState(State::Cleanup);
        }
        break;

    case State::Cleanup:
        storage_.enforceStorageLimit();
        database_.setStat("storage_bytes", static_cast<int64_t>(storage_.totalVerifiedBytes()));
        logStatistics();
        archive_requested_.store(false);
        if (monitor_.isArmed()) {
            setState(State::WaitDisarm);
        } else {
            setState(State::WaitArm);
        }
        break;

    case State::ConnectionLost:
        std::this_thread::sleep_for(std::chrono::seconds(2));
        setState(State::Connect);
        break;
    }
}

} // namespace mcls
