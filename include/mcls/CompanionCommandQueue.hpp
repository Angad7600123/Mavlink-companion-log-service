#pragma once

#include <mutex>
#include <queue>
#include <variant>

namespace mcls {

struct StartArchiveCommand {};
struct CancelArchiveCommand {};

using CompanionCommand = std::variant<StartArchiveCommand, CancelArchiveCommand>;

/// Outcome of an archive.start request, decided against live service state on
/// the companion UDP thread so the handler can return an accurate ack/err.
/// Makes archive.start idempotent by state: a retry (e.g. after a lost ack)
/// that arrives while a cycle is already in flight returns Busy instead of
/// queuing a second StartArchiveCommand.
enum class ArchiveStartResult {
    Accepted,      ///< preconditions met; a StartArchiveCommand was queued
    Busy,          ///< an archive cycle is already in progress
    Armed,         ///< vehicle is armed
    NotConnected,  ///< MAVLink transport not connected
};

/// Thread-safe queue for commands arriving from the companion UDP thread.
/// The main loop drains this queue at the top of each processState() iteration.
class CompanionCommandQueue {
public:
    void push(CompanionCommand cmd) {
        std::lock_guard lock(mutex_);
        queue_.push(std::move(cmd));
    }

    bool pop(CompanionCommand& out) {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    bool empty() const {
        std::lock_guard lock(mutex_);
        return queue_.empty();
    }

private:
    mutable std::mutex mutex_;
    std::queue<CompanionCommand> queue_;
};

} // namespace mcls
