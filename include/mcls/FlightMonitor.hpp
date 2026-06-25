#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

namespace mcls {

class Logger;

/// Tracks vehicle identity and armed state from HEARTBEAT messages.
class FlightMonitor {
public:
    enum class Event {
        VehicleDetected,
        VehicleArmed,
        VehicleDisarmed,
        LinkLost,
        LinkRestored,
    };

    using EventHandler = std::function<void(Event)>;

    explicit FlightMonitor(Logger& logger);

    void onHeartbeat(uint8_t sysid,
                     uint8_t compid,
                     uint8_t type,
                     uint8_t autopilot,
                     bool armed);
    void onLinkTimeout();
    void onLinkRestored();

    bool isArmed() const { return armed_; }
    bool vehicleDetected() const { return vehicle_detected_; }
    uint8_t sysid() const { return sysid_; }
    uint8_t compid() const { return compid_; }

    void setEventHandler(EventHandler handler);

private:
    Logger& logger_;
    EventHandler handler_;
    std::atomic<bool> armed_{false};
    std::atomic<bool> vehicle_detected_{false};
    std::atomic<bool> link_lost_{false};
    uint8_t sysid_ = 0;
    uint8_t compid_ = 0;
    uint8_t vehicle_type_ = 0;
    uint8_t autopilot_ = 0;
    bool first_heartbeat_ = true;

    void emit(Event event);
};

} // namespace mcls
