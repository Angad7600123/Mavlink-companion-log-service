#pragma once

#include <cstdint>
#include <mutex>
#include <queue>
#include <variant>
#include <vector>

namespace mcls {

struct StartArchiveCommand {};
struct CancelArchiveCommand {};
struct RefreshLogsCommand {};
struct DownloadLogsCommand {
    std::vector<std::uint16_t> ids;  ///< validated against the cached enumeration
    bool all = false;
};
struct EraseAllLogsCommand {};

using CompanionCommand = std::variant<StartArchiveCommand,
                                      CancelArchiveCommand,
                                      RefreshLogsCommand,
                                      DownloadLogsCommand,
                                      EraseAllLogsCommand>;

/// Kind of manual job a companion request is asking for.
enum class CompanionJobKind {
    Archive,   ///< archive.start — full cycle (download all un-archived + erase)
    Refresh,   ///< logs.refresh — re-enumerate the FC log list
    Download,  ///< logs.download — archive a selection (or all), no erase
    EraseAll,  ///< logs.erase — unconditional full DataFlash wipe (super-delete)
};

/// Outcome of a manual-job request, decided against live service state on the
/// companion UDP thread so the handler can return an accurate ack/err.
/// Idempotent by state: a retry (e.g. after a lost ack) that arrives while a
/// job is already in flight yields AlreadyRunning — reported to the client as
/// success (accepted + already_running:true), never as an error, and never
/// queues a second job.
enum class JobStartResult {
    Accepted,        ///< preconditions met; a command was queued
    AlreadyRunning,  ///< a job is already in progress (idempotent success)
    Armed,           ///< vehicle is armed
    NotConnected,    ///< MAVLink transport not connected
    NotFound,        ///< logs.download: none of the requested ids exist
};

/// Full outcome of a manual-job request (result + download bookkeeping).
struct JobOutcome {
    JobStartResult result = JobStartResult::Accepted;
    int queued = 0;                        ///< logs.download: number of logs queued
    std::vector<std::uint16_t> not_found;  ///< logs.download: requested ids not on FC
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
