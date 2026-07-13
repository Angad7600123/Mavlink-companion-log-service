#include "mcls/VideoRecorder.hpp"

#include <cstring>
#include <ctime>
#include <filesystem>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <cerrno>

extern char** environ;
#endif

namespace mcls {

VideoRecorder::VideoRecorder(const Config::RecordingSettings& settings, Logger& logger)
    : settings_(settings), logger_(logger) {}

VideoRecorder::~VideoRecorder() {
    stop();
}

bool VideoRecorder::checkMediaWritable() const {
#ifdef _WIN32
    return false;
#else
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(settings_.mount_path, ec) || ec) {
        return false;
    }
    if (::access(settings_.mount_path.c_str(), W_OK) != 0) {
        return false;
    }

    // mount_path must be a real mount point, not just a writable directory
    // that happens to live on the root filesystem (e.g. the USB drive isn't
    // plugged in / mounted yet, but /mnt/usb still exists as a plain dir).
    // A directory's device id differs from its parent's iff something else
    // is mounted on top of it.
    struct stat self_st {};
    struct stat parent_st {};
    const fs::path parent = fs::path(settings_.mount_path).parent_path();
    if (::stat(settings_.mount_path.c_str(), &self_st) != 0 ||
        ::stat(parent.empty() ? "/" : parent.c_str(), &parent_st) != 0) {
        return false;
    }
    return self_st.st_dev != parent_st.st_dev;
#endif
}

#ifndef _WIN32
void VideoRecorder::reapIfExited() {
    if (pid_ <= 0) {
        return;
    }
    int status = 0;
    const pid_t rc = ::waitpid(pid_, &status, WNOHANG);
    if (rc == pid_ || (rc < 0 && errno == ECHILD)) {
        logger_.info("VideoRecorder: recorder process (pid=" + std::to_string(pid_) +
                     ") is no longer running");
        pid_ = -1;
        if (sync_active_) {
            *sync_active_ = false;
            sync_active_.reset();
        }
    }
}
#endif

VideoRecorder::StartResult VideoRecorder::start() {
#ifdef _WIN32
    return StartResult::Disabled;
#else
    std::lock_guard<std::mutex> lock(mutex_);

    if (!settings_.enabled) {
        return StartResult::Disabled;
    }

    reapIfExited();
    if (pid_ > 0) {
        return StartResult::AlreadyRecording;
    }

    if (!checkMediaWritable()) {
        logger_.warn("VideoRecorder: rec.start rejected — mount_path '" + settings_.mount_path +
                     "' is missing, not writable, or not an actual mount point");
        return StartResult::NoMedia;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&now_c, &tm_buf);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm_buf);

    const std::string filename = settings_.filename_prefix + "_" + stamp + ".ts";
    std::string path = settings_.mount_path;
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    path += filename;

    const std::string caps = "application/x-rtp,media=video,encoding-name=H264,payload=" +
        std::to_string(settings_.rtp_payload_type) + ",clock-rate=90000";

    // gst-launch-1.0 [-q] udpsrc port=<p> caps=<c> !
    //     rtph264depay ! h264parse ! mpegtsmux ! filesink location=<path>
    //
    // NO rtpjitterbuffer: the recording tap is a loopback (127.0.0.1) source
    // with zero jitter, zero loss, and in-order delivery. A standalone
    // rtpjitterbuffer here has nothing to smooth and instead wedges after a
    // handful of buffers on clock/latency negotiation — confirmed on a Pi 4:
    // with the jitterbuffer the .ts stayed 0 bytes; without it recording works.
    std::vector<std::string> args = {
        settings_.gst_launch_path,
        "-q",
        "udpsrc",
        "port=" + std::to_string(settings_.source_port),
        "caps=" + caps,
        "!",
        "rtph264depay",
        "!",
        "h264parse",
        "!",
        "mpegtsmux",
        "!",
        "filesink",
        "location=" + path,
    };
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    pid_t child = -1;
    // posix_spawn (not fork+exec): mcls is multi-threaded (companion RX +
    // keepalive + main threads), and fork() in a multi-threaded process only
    // duplicates the calling thread — a known hazard posix_spawn avoids.
    const int rc = ::posix_spawnp(&child, settings_.gst_launch_path.c_str(), &actions, nullptr,
                                  argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);

    if (rc != 0 || child <= 0) {
        logger_.error("VideoRecorder: posix_spawnp(" + settings_.gst_launch_path +
                      ") failed: " + std::strerror(rc));
        return StartResult::Failed;
    }

    pid_ = child;
    start_time_ = std::chrono::steady_clock::now();
    logger_.info("VideoRecorder: recording started, pid=" + std::to_string(pid_) + " file=" +
                 path + " (source udp port " + std::to_string(settings_.source_port) + ")");

    if (settings_.fsync_interval_sec > 0) {
        sync_active_ = std::make_shared<std::atomic<bool>>(true);
        auto active = sync_active_;
        const int interval_sec = settings_.fsync_interval_sec;
        // Global sync() rather than fsync() on the file: the file is written
        // by the gst-launch-1.0 subprocess, not this process, and periodic
        // sync() is simpler than reopening its path every tick while being
        // just as effective — it forces all dirty pages (and, on ext4, the
        // journaled inode metadata that records the file's current size) to
        // the physical device. Bounds the worst-case data lost on a hard
        // power cut to roughly this interval instead of "whatever the kernel
        // hadn't gotten around to flushing yet" (which is how a single-file
        // recording came back at 0 bytes after a field power-loss test).
        std::thread([active, interval_sec]() {
            while (active->load()) {
                for (int i = 0; i < interval_sec * 10 && active->load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (!active->load()) {
                    break;
                }
                ::sync();
            }
        }).detach();
    }

    return StartResult::Started;
#endif
}

bool VideoRecorder::stop() {
#ifdef _WIN32
    return false;
#else
    std::unique_lock<std::mutex> lock(mutex_);
    reapIfExited();
    if (pid_ <= 0) {
        return false;
    }

    const pid_t child = pid_;
    logger_.info("VideoRecorder: stopping recorder pid=" + std::to_string(child));
    ::kill(child, SIGINT);  // gst-launch-1.0's graceful-stop signal

    if (sync_active_) {
        *sync_active_ = false;
        sync_active_.reset();
    }

    // From the caller's perspective recording is stopped now: the request has
    // been issued and the file is already safe (MPEG-TS tolerates a truncated
    // tail) even if the process takes a moment to actually exit. Confirming
    // and force-killing on a timeout happens in a background thread so this
    // call never blocks the UDP thread — the same responsiveness lesson as
    // the archive.cancel fix.
    pid_ = -1;
    lock.unlock();

    // Deliberately captures only the plain pid, nothing from `this`: this
    // thread is detached and must stay valid even if VideoRecorder (or mcls
    // itself) is destroyed/restarted before the grace period elapses.
    std::thread([child]() {
        for (int i = 0; i < 20; ++i) {  // ~2s
            int status = 0;
            if (::waitpid(child, &status, WNOHANG) == child) {
                ::sync();  // flush the just-finalized file to disk promptly
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        ::kill(child, SIGKILL);
        int status = 0;
        ::waitpid(child, &status, 0);
        ::sync();
    }).detach();

    return true;
#endif
}

VideoRecorder::Snapshot VideoRecorder::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot s;
    s.enabled = settings_.enabled;
#ifndef _WIN32
    reapIfExited();
    s.active = pid_ > 0;
    if (s.active) {
        s.duration_sec = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                              start_time_)
                .count());
    }
    struct statvfs st {};
    if (::statvfs(settings_.mount_path.c_str(), &st) == 0) {
        s.free_bytes = static_cast<std::uint64_t>(st.f_bavail) * st.f_frsize;
    }
#endif
    return s;
}

} // namespace mcls
