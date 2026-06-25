#pragma once

#include <ardupilotmega/mavlink.h>

#include "mcls/Config.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace mcls {

class Logger;
class Transport;

/// MAVLink client with background RX parsing over a pluggable transport.
class MavlinkClient {
public:
    using MessageHandler = std::function<void(const mavlink_message_t&)>;

    MavlinkClient(const Config::TransportSettings& transport_settings, Logger& logger);
    ~MavlinkClient();

    MavlinkClient(const MavlinkClient&) = delete;
    MavlinkClient& operator=(const MavlinkClient&) = delete;

    bool connect();
    void disconnect();
    bool isConnected() const;
    bool sendMessage(const mavlink_message_t& msg);
    void setMessageHandler(MessageHandler handler);
    bool waitForHeartbeat(std::chrono::milliseconds timeout);
    bool heartbeatFresh() const;

    uint8_t targetSystem() const { return target_system_; }
    uint8_t targetComponent() const { return target_component_; }

private:
    Config::TransportSettings transport_settings_;
    Logger& logger_;
    std::unique_ptr<Transport> transport_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::chrono::steady_clock::time_point> last_heartbeat_{};

    uint8_t target_system_ = 1;
    uint8_t target_component_ = 1;

    std::thread rx_thread_;
    MessageHandler handler_;
    std::mutex handler_mutex_;
    std::mutex send_mutex_;

    void rxLoop();
    void handleParsedMessage(const mavlink_message_t& msg);
};

} // namespace mcls
