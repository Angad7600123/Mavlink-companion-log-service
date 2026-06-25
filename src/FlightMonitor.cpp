#include "mcls/FlightMonitor.hpp"
#include "mcls/Logger.hpp"

#include <ardupilotmega/mavlink.h>

namespace mcls {

FlightMonitor::FlightMonitor(Logger& logger) : logger_(logger) {}

void FlightMonitor::setEventHandler(EventHandler handler) {
    handler_ = std::move(handler);
}

void FlightMonitor::onHeartbeat(uint8_t sysid,
                                uint8_t compid,
                                uint8_t type,
                                uint8_t autopilot,
                                bool armed) {
    const bool identity_changed =
        vehicle_detected_ &&
        (sysid_ != sysid || compid_ != compid || vehicle_type_ != type || autopilot_ != autopilot);

    if (!vehicle_detected_ || identity_changed) {
        sysid_ = sysid;
        compid_ = compid;
        vehicle_type_ = type;
        autopilot_ = autopilot;
        vehicle_detected_ = true;
        first_heartbeat_ = true;
        logger_.info("Vehicle detected (sysid=" + std::to_string(sysid) +
                     ", compid=" + std::to_string(compid) + ")");
        emit(Event::VehicleDetected);
    }

    if (link_lost_) {
        link_lost_ = false;
        emit(Event::LinkRestored);
    }

    if (first_heartbeat_) {
        armed_ = armed;
        first_heartbeat_ = false;
        return;
    }

    if (armed && !armed_) {
        armed_ = true;
        logger_.info("Vehicle armed");
        emit(Event::VehicleArmed);
    } else if (!armed && armed_) {
        armed_ = false;
        logger_.info("Vehicle disarmed");
        emit(Event::VehicleDisarmed);
    }
}

void FlightMonitor::onLinkTimeout() {
    if (!link_lost_) {
        link_lost_ = true;
        logger_.warn("MAVLink link lost");
        emit(Event::LinkLost);
    }
}

void FlightMonitor::onLinkRestored() {
    if (link_lost_) {
        link_lost_ = false;
        logger_.info("MAVLink link restored");
        emit(Event::LinkRestored);
    }
}

void FlightMonitor::emit(Event event) {
    if (handler_) {
        handler_(event);
    }
}

} // namespace mcls
