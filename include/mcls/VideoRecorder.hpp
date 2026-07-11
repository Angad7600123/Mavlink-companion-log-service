#pragma once

#include "mcls/Config.hpp"
#include "mcls/Logger.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#ifndef _WIN32
#include <sys/types.h>  // pid_t
#endif

namespace mcls {

/// Controls an independent recorder subprocess that reads the local RTP/H264
/// recording tap (see Config::RecordingSettings / docs/companion-wfb.md) and
/// writes an MPEG-TS file to removable media (chosen specifically because a
/// truncated .ts — a crash, power loss, or a killed process — still plays
/// everything up to the cut, unlike a plain MP4 whose index is written last).
///
/// Deliberately independent of DroneLogService's state machine and
/// CompanionCommandQueue: recording must start/stop regardless of whatever
/// archive job may be running. See the archive.cancel fix (CompanionUdpServer
/// CancelFn) for why routing a "must take effect now" control through the
/// command queue is the wrong shape — start()/stop() here follow the same
/// direct-call, thread-safe pattern instead.
class VideoRecorder {
public:
    enum class StartResult {
        Started,
        AlreadyRecording,  ///< idempotent success, not an error
        Disabled,          ///< [recording] enabled = false
        NoMedia,           ///< mount_path missing or not writable
        Failed,            ///< subprocess spawn failed
    };

    struct Snapshot {
        bool enabled = false;
        bool active = false;
        std::uint32_t duration_sec = 0;
        std::uint64_t free_bytes = 0;
    };

    VideoRecorder(const Config::RecordingSettings& settings, Logger& logger);
    ~VideoRecorder();

    VideoRecorder(const VideoRecorder&) = delete;
    VideoRecorder& operator=(const VideoRecorder&) = delete;

    /// Thread-safe; call from any thread, at any time. Returns quickly
    /// (spawns a subprocess, does not wait on it).
    StartResult start();

    /// Thread-safe; call from any thread, at any time. Returns almost
    /// immediately — signals the subprocess and hands off graceful-then-forced
    /// shutdown to a detached watcher rather than blocking the caller (a
    /// blocking stop() here would reproduce the same "control coupled to
    /// something slow" bug archive.cancel had). Idempotent: returns false if
    /// nothing was recording.
    bool stop();

    /// Thread-safe. Opportunistically detects & clears state if the
    /// subprocess exited on its own (e.g. USB pulled mid-flight).
    Snapshot snapshot();

private:
    bool checkMediaWritable() const;
    void reapIfExited();  // caller must hold mutex_

    Config::RecordingSettings settings_;
    Logger& logger_;

    mutable std::mutex mutex_;
#ifndef _WIN32
    pid_t pid_ = -1;
#endif
    std::chrono::steady_clock::time_point start_time_{};
};

} // namespace mcls
