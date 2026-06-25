#include "mcls/MavlinkClient.hpp"
#include "mcls/Logger.hpp"
#include "mcls/Transport.hpp"

#include <ardupilotmega/mavlink.h>

#include <chrono>
#include <thread>

namespace mcls {

MavlinkClient::MavlinkClient(const Config::TransportSettings& transport_settings, Logger& logger)
    : transport_settings_(transport_settings),
      logger_(logger),
      transport_(createTransport(transport_settings_, logger_)) {
    last_heartbeat_.store(std::chrono::steady_clock::now());
}

MavlinkClient::~MavlinkClient() {
    disconnect();
}

bool MavlinkClient::connect() {
    disconnect();

    if (!transport_->connect()) {
        logger_.error("Failed to connect MAVLink transport (" + transport_->describe() + ")");
        return false;
    }

    connected_.store(true);
    running_.store(true);
    rx_thread_ = std::thread(&MavlinkClient::rxLoop, this);
    logger_.info("MAVLink transport connected (" + transport_->describe() + ")");
    return true;
}

void MavlinkClient::disconnect() {
    running_.store(false);
    connected_.store(false);

    if (transport_) {
        transport_->disconnect();
    }

    if (rx_thread_.joinable()) {
        rx_thread_.join();
    }
}

bool MavlinkClient::isConnected() const {
    return connected_.load() && transport_ && transport_->isConnected();
}

bool MavlinkClient::sendMessage(const mavlink_message_t& msg) {
    if (!isConnected()) {
        return false;
    }

    uint8_t buffer[MAVLINK_MAX_PACKET_LEN];
    const uint16_t len = mavlink_msg_to_send_buffer(buffer, const_cast<mavlink_message_t*>(&msg));

    std::lock_guard lock(send_mutex_);
    return transport_->send(buffer, len);
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
    return age <= std::chrono::seconds(transport_settings_.heartbeat_timeout_sec);
}

void MavlinkClient::rxLoop() {
    mavlink_message_t msg{};
    mavlink_status_t status{};

    while (running_.load()) {
        uint8_t byte = 0;
        if (!transport_->readByte(byte)) {
            if (running_.load()) {
                logger_.warn("MAVLink transport closed (" + transport_->describe() + ")");
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
