#include "mcls/DroneLogService.hpp"

#include "mcls/CompanionDiagnostics.hpp"

#include <ardupilotmega/mavlink.h>

#include <algorithm>
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

    CompanionDiagnostics::logEffectiveSettings(logger_, config_.companion, config_path_,
                                                config_.companion_table_present);

    if (config_.companion.enabled) {
        logger_.info("Companion API enabled — constructing UDP server");
        companion_server_ = std::make_unique<CompanionUdpServer>(
            config_.companion,
            logger_,
            companion_commands_,
            [this]() { return buildSnapshot(); },
            [this](int offset, int limit) { return buildFcLogsPage(offset, limit); },
            [this](CompanionJobKind kind, const std::vector<std::uint16_t>& ids, bool all) {
                return requestCompanionJob(kind, ids, all);
            });
    } else {
        logger_.info("Companion API disabled — UDP server not created");
    }
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

    logger_.info("Using configuration file: " + config_path_);

    if (companion_server_) {
        logger_.info("Starting companion UDP server (bind " + config_.companion.bind_host + ":" +
                     std::to_string(config_.companion.bind_port) + ")");
        if (!companion_server_->start()) {
            logger_.error("Companion UDP server failed to start — check bind port and journal "
                            "for errno; service continues without companion API");
            companion_server_.reset();
        } else if (!companion_server_->isListening()) {
            logger_.error("Companion UDP server start returned ok but isListening() is false");
        }
    } else if (config_.companion.enabled) {
        logger_.error("Companion enabled in config but UDP server object is missing");
    }

    logger_.info("MAVLink Companion Log Service starting");

    while (running_.load()) {
        drainCompanionCommands();
        processState();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Ensure any in-flight download stops promptly on shutdown.
    downloader_.requestCancel();
    client_.disconnect();
    if (companion_server_) {
        logger_.info("Shutting down companion UDP server");
        companion_server_->stop();
    }
    logStatistics();
    logger_.info("MAVLink Companion Log Service stopped");
}

void DroneLogService::stop() {
    running_.store(false);
    downloader_.requestCancel();
}

void DroneLogService::setState(State state) {
    state_.store(state);
}

bool DroneLogService::isArchiveBusy(State state) const {
    switch (state) {
    case State::Delay:
    case State::Enumerate:
    case State::Archive:
    case State::EraseAll:
    case State::ManualRefresh:
    case State::ManualDownload:
    case State::ManualErase:
        return true;
    default:
        return false;
    }
}

void DroneLogService::returnToIdleState() {
    setState(monitor_.isArmed() ? State::WaitDisarm : State::WaitArm);
}

void DroneLogService::onFlightEvent(FlightMonitor::Event event) {
    const State state = currentState();
    switch (event) {
    case FlightMonitor::Event::VehicleArmed:
        if (state == State::WaitArm) {
            setState(State::WaitDisarm);
        } else if (isArchiveBusy(state)) {
            logger_.warn("Vehicle armed during archive; cancelling current cycle");
            downloader_.requestCancel();
        }
        break;
    case FlightMonitor::Event::VehicleDisarmed:
        disarm_time_ = std::chrono::steady_clock::now();
        archive_requested_.store(true);
        if (state == State::WaitDisarm) {
            setState(State::Delay);
        } else if (isArchiveBusy(state)) {
            logger_.warn("Disarm during archive; cancelling and scheduling a fresh cycle");
            downloader_.requestCancel();
        }
        break;
    case FlightMonitor::Event::LinkLost:
        handleConnectionLost();
        break;
    case FlightMonitor::Event::LinkRestored:
        if (state == State::ConnectionLost) {
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
    // Do not tear down the transport mid-archive; download can take many minutes
    // and is handled by the download/abort logic plus post-cycle reconnect policy.
    if (isArchiveBusy(currentState())) {
        return;
    }
    switch (currentState()) {
    case State::ConnectionLost:
    case State::Boot:
        return;
    default:
        break;
    }
    logger_.warn("Connection lost, will reconnect");
    client_.disconnect();
    setState(State::ConnectionLost);
}

bool DroneLogService::reconnect() {
    logger_.info("Reconnecting to MAVLink transport...");
    if (!client_.connect()) {
        return false;
    }
    return client_.waitForHeartbeat(std::chrono::seconds(10));
}

void DroneLogService::evaluateArchiveOutcome(const ArchiveCycleResult& result) {
    // Cancellation is an operational event (re-arm/disarm/shutdown), not a fault.
    if (result.cancelled > 0 && result.failed == 0) {
        logger_.info("Archive cycle cancelled; transport left intact");
        return;
    }

    if (result.all_archived && result.failed == 0) {
        consecutive_archive_failures_ = 0;
        return;
    }

    ++consecutive_archive_failures_;

    const bool transport_class = isTransportFailure(result.last_failure_reason);
    bool do_reconnect = false;
    std::string why;

    if (config_.download.reconnect_on_transport_failure && transport_class) {
        do_reconnect = true;
        why = "transport-class failure (reason=" +
              std::string(toString(result.last_failure_reason)) + ")";
    } else if (config_.download.reconnect_after_consecutive_failures > 0 &&
               consecutive_archive_failures_ >=
                   config_.download.reconnect_after_consecutive_failures) {
        do_reconnect = true;
        why = std::to_string(consecutive_archive_failures_) + " consecutive archive failures";
    }

    if (!do_reconnect) {
        logger_.info("Skipping transport reconnect (reason=" +
                     std::string(toString(result.last_failure_reason)) +
                     ", consecutive_failures=" + std::to_string(consecutive_archive_failures_) +
                     "/" +
                     std::to_string(config_.download.reconnect_after_consecutive_failures) + ")");
        return;
    }

    logger_.warn("Reconnecting MAVLink transport after " + why);
    client_.disconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if (client_.connect()) {
        client_.waitForHeartbeat(std::chrono::seconds(5));
        consecutive_archive_failures_ = 0;
    } else {
        logger_.warn("Transport reconnect failed; idle loop will retry");
    }
}

void DroneLogService::logStatistics() const {
    logger_.info("Statistics: flights_archived=" +
                 std::to_string(database_.getStat("flights_archived")) +
                 ", bytes_downloaded=" + std::to_string(database_.getStat("bytes_downloaded")) +
                 ", archive_failures=" + std::to_string(database_.getStat("archive_failures")) +
                 ", archive_cancellations=" +
                 std::to_string(database_.getStat("archive_cancellations")) +
                 ", retries=" + std::to_string(database_.getStat("retries")) +
                 ", storage_bytes=" + std::to_string(storage_.totalVerifiedBytes()) +
                 ", last_archive_time=" + std::to_string(database_.getStat("last_archive_time")));
}

void DroneLogService::processState() {
    switch (currentState()) {
    case State::Boot:
        setState(State::Connect);
        break;

    case State::Connect:
        if (reconnect()) {
            {
                std::lock_guard lock(snapshot_mutex_);
                fc_logs_stale_ = true;
            }
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
        const auto elapsed = std::chrono::steady_clock::now() - disarm_time_;
        if (elapsed >= std::chrono::seconds(config_.download.delay_after_disarm_sec)) {
            // Consume the pending request and start a clean cycle.
            archive_requested_.store(false);
            downloader_.resetCancel();
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
        cacheEnumerationResult(logs);
        const auto result = downloader_.archiveAll(logs);
        last_cycle_result_ = result;

        logger_.info("Archive cycle: downloaded=" + std::to_string(result.downloaded) +
                     ", skipped=" + std::to_string(result.skipped) +
                     ", failed=" + std::to_string(result.failed) +
                     ", cancelled=" + std::to_string(result.cancelled) +
                     ", reason=" + toString(result.last_failure_reason));

        const bool cancelled = result.cancelled > 0;
        if (!cancelled && result.all_archived && config_.download.erase_after_success &&
            !logs.empty()) {
            setState(State::EraseAll);
        } else {
            if (cancelled) {
                logger_.warn("Skipping FC erase because the cycle was cancelled");
            } else if (!result.all_archived) {
                logger_.warn("Skipping FC erase because one or more logs failed to archive");
            }
            setState(State::Cleanup);
        }
        break;
    }

    case State::EraseAll:
        if (downloader_.cancelled()) {
            logger_.warn("Skipping FC erase because the cycle was cancelled");
            setState(State::Cleanup);
            break;
        }
        if (downloader_.eraseFlightControllerLogs()) {
            setState(State::Cleanup);
        } else {
            logger_.error("Failed to send LOG_ERASE");
            setState(State::Cleanup);
        }
        break;

    case State::ManualRefresh: {
        logger_.info("Manual refresh: enumerating FC logs");
        const auto logs = downloader_.enumerateLogs();
        cacheEnumerationResult(logs);
        logger_.info("Manual refresh: " + std::to_string(logs.size()) + " logs cached");
        returnToIdleState();
        break;
    }

    case State::ManualDownload: {
        // Fresh enumeration so we archive against live FC state, then filter to
        // the validated selection. Reuses the exact same download/verify/persist
        // pipeline as the automatic cycle — but never erases the FC.
        const auto logs = downloader_.enumerateLogs();
        cacheEnumerationResult(logs);

        std::vector<LogEntry> selected;
        if (manual_download_all_) {
            selected = logs;
        } else {
            for (const auto& e : logs) {
                if (std::find(manual_download_ids_.begin(), manual_download_ids_.end(), e.id) !=
                    manual_download_ids_.end()) {
                    selected.push_back(e);
                }
            }
        }
        logger_.info("Manual download: " + std::to_string(selected.size()) + " of " +
                     std::to_string(logs.size()) + " logs selected");

        const auto result = downloader_.archiveAll(selected);
        last_cycle_result_ = result;
        logger_.info("Manual download: downloaded=" + std::to_string(result.downloaded) +
                     ", skipped=" + std::to_string(result.skipped) +
                     ", failed=" + std::to_string(result.failed) +
                     ", cancelled=" + std::to_string(result.cancelled));

        manual_download_ids_.clear();
        manual_download_all_ = false;
        // No FC erase for manual downloads. Cleanup handles storage limits,
        // stats, and the reconnect policy.
        setState(State::Cleanup);
        break;
    }

    case State::ManualErase: {
        logger_.warn("Manual erase: wiping FC DataFlash (super-delete)");
        downloader_.resetCancel();
        if (!downloader_.eraseFlightControllerLogs()) {
            logger_.error("Manual erase: failed to send LOG_ERASE");
        }
        // The DataFlash is now empty; publish an empty list directly rather than
        // re-enumerating (which would burn the full retry budget against a FC
        // that legitimately has no logs, and may still be mid-erase).
        cacheEnumerationResult({});
        returnToIdleState();
        break;
    }

    case State::Cleanup:
        storage_.enforceStorageLimit();
        database_.setStat("storage_bytes", static_cast<int64_t>(storage_.totalVerifiedBytes()));
        logStatistics();

        // Conditional transport reconnect based on the cycle outcome.
        evaluateArchiveOutcome(last_cycle_result_);

        if (archive_requested_.load()) {
            // A disarm arrived during the previous cycle: run a fresh one.
            logger_.info("Pending disarm detected; scheduling another archive cycle");
            setState(State::Delay);
        } else if (monitor_.isArmed()) {
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

// ---------------------------------------------------------------------------
// Companion API helpers
// ---------------------------------------------------------------------------

std::string DroneLogService::stateToString(State state) {
    switch (state) {
    case State::Boot:           return "boot";
    case State::Connect:        return "connect";
    case State::WaitHeartbeat:  return "wait_heartbeat";
    case State::WaitArm:        return "wait_arm";
    case State::WaitDisarm:     return "wait_disarm";
    case State::Delay:          return "delay";
    case State::Enumerate:      return "enumerate";
    case State::Archive:        return "archive";
    case State::EraseAll:       return "erase_all";
    case State::Cleanup:        return "cleanup";
    case State::ConnectionLost: return "connection_lost";
    case State::ManualRefresh:  return "manual_refresh";
    case State::ManualDownload: return "manual_download";
    case State::ManualErase:    return "manual_erase";
    }
    return "unknown";
}

void DroneLogService::cacheEnumerationResult(const std::vector<LogEntry>& entries) {
    std::lock_guard lock(snapshot_mutex_);
    cached_fc_logs_ = entries;
    fc_logs_stale_ = false;
}

ServiceSnapshot DroneLogService::buildSnapshot() const {
    std::lock_guard lock(snapshot_mutex_);

    ServiceSnapshot s;
    const State state = currentState();
    s.state = stateToString(state);
    s.version = "1.0.0";

    // Job descriptor for the phone UI: derived from the state machine so no
    // extra bookkeeping can drift out of sync.
    switch (state) {
    case State::Delay:
    case State::Enumerate:
    case State::Archive:
    case State::EraseAll:
        s.job_type = "archive";
        break;
    case State::ManualRefresh:
        s.job_type = "refresh";
        break;
    case State::ManualDownload:
        s.job_type = "download";
        break;
    case State::ManualErase:
        s.job_type = "erase";
        break;
    default:
        break;  // idle — job_type stays empty
    }
    s.transport_connected = client_.isConnected();
    s.heartbeat_fresh = client_.heartbeatFresh();
    s.vehicle_detected = monitor_.vehicleDetected();
    s.vehicle_armed = monitor_.isArmed();

    s.fc_logs_count = static_cast<int>(cached_fc_logs_.size());
    s.fc_logs_stale = fc_logs_stale_;

    const auto prog = downloader_.activeProgress();
    s.archive_active = prog.active;
    s.archive_current_log_id = prog.log_id;
    s.archive_progress_bytes = prog.bytes_received;
    s.archive_progress_total_bytes = prog.total_bytes;
    s.archive_bytes_per_sec = prog.bytes_per_sec;
    s.archive_percent =
        prog.total_bytes > 0
            ? static_cast<int>((static_cast<uint64_t>(prog.bytes_received) * 100) / prog.total_bytes)
            : 0;

    s.last_cycle_downloaded = last_cycle_result_.downloaded;
    s.last_cycle_skipped = last_cycle_result_.skipped;
    s.last_cycle_failed = last_cycle_result_.failed;
    s.last_cycle_cancelled = last_cycle_result_.cancelled;
    s.last_cycle_all_archived = last_cycle_result_.all_archived;

    s.storage_used_bytes = storage_.totalVerifiedBytes();
    s.storage_limit_bytes =
        static_cast<uint64_t>(config_.storage.max_size_gb) * 1024ULL * 1024ULL * 1024ULL;
    s.storage_archived_count = database_.getStat("flights_archived");

    return s;
}

FcLogsPage DroneLogService::buildFcLogsPage(int offset, int limit) const {
    std::lock_guard lock(snapshot_mutex_);

    FcLogsPage page;
    page.count = static_cast<int>(cached_fc_logs_.size());
    page.offset = offset;
    page.stale = fc_logs_stale_;

    const int start = std::max(0, offset);
    const int stop = std::min(static_cast<int>(cached_fc_logs_.size()), start + limit);
    for (int i = start; i < stop; ++i) {
        const LogEntry& e = cached_fc_logs_[static_cast<std::size_t>(i)];
        FcLogEntry out;
        out.id = e.id;
        out.size = e.size;
        out.time_utc = e.time_utc;
        out.downloaded = database_.hasArchived(e.id, e.size);
        page.entries.push_back(out);
    }
    return page;
}

JobOutcome DroneLogService::requestCompanionJob(CompanionJobKind kind,
                                                const std::vector<std::uint16_t>& ids,
                                                bool all) {
    // Runs on the companion UDP thread. Reads the same live state the main loop
    // uses, so the ack the client receives matches what will actually happen.
    // Queues a command only when accepted; a retry that lands while a job is
    // already in flight returns AlreadyRunning (idempotent success) rather than
    // queuing a second command.
    JobOutcome out;
    const State state = currentState();
    const bool busy = isArchiveBusy(state);

    if (kind == CompanionJobKind::EraseAll) {
        // Super-delete override: only physically impossible when the transport
        // is down. Cancels any in-flight job, then wipes the FC DataFlash.
        if (!client_.isConnected()) {
            out.result = JobStartResult::NotConnected;
            return out;
        }
        if (busy) {
            logger_.warn("Companion: logs.erase overriding in-flight job (state=" +
                         stateToString(state) + ")");
            downloader_.requestCancel();
        }
        companion_commands_.push(EraseAllLogsCommand{});
        logger_.info("Companion: logs.erase accepted, queued (state=" + stateToString(state) + ")");
        out.result = JobStartResult::Accepted;
        return out;
    }

    if (busy) {
        out.result = JobStartResult::AlreadyRunning;
        return out;
    }
    if (monitor_.isArmed()) {
        out.result = JobStartResult::Armed;
        return out;
    }
    if (!client_.isConnected()) {
        out.result = JobStartResult::NotConnected;
        return out;
    }

    switch (kind) {
    case CompanionJobKind::Archive:
        companion_commands_.push(StartArchiveCommand{});
        logger_.info("Companion: archive.start accepted, queued (state=" + stateToString(state) +
                     ")");
        break;

    case CompanionJobKind::Refresh:
        companion_commands_.push(RefreshLogsCommand{});
        logger_.info("Companion: logs.refresh accepted, queued");
        break;

    case CompanionJobKind::Download: {
        // Validate the selection against the cached enumeration so the client
        // learns immediately which ids are stale; the job re-enumerates anyway.
        std::vector<std::uint16_t> valid;
        {
            std::lock_guard lock(snapshot_mutex_);
            if (all) {
                out.queued = static_cast<int>(cached_fc_logs_.size());
            } else {
                for (const auto id : ids) {
                    const bool exists =
                        std::any_of(cached_fc_logs_.begin(), cached_fc_logs_.end(),
                                    [id](const LogEntry& e) { return e.id == id; });
                    if (exists) {
                        valid.push_back(id);
                    } else {
                        out.not_found.push_back(id);
                    }
                }
                out.queued = static_cast<int>(valid.size());
            }
        }
        if (!all && valid.empty()) {
            out.result = JobStartResult::NotFound;
            return out;
        }
        companion_commands_.push(DownloadLogsCommand{std::move(valid), all});
        logger_.info("Companion: logs.download accepted, queued=" + std::to_string(out.queued) +
                     " not_found=" + std::to_string(out.not_found.size()) +
                     (all ? " (all)" : ""));
        break;
    }

    case CompanionJobKind::EraseAll:
        break;  // handled above
    }

    out.result = JobStartResult::Accepted;
    return out;
}

void DroneLogService::drainCompanionCommands() {
    if (companion_commands_.empty()) {
        return;
    }

    logger_.debug("Companion: draining command queue");

    CompanionCommand cmd;
    while (companion_commands_.pop(cmd)) {
        std::visit(
            [this](auto&& c) {
                using T = std::decay_t<decltype(c)>;
                if constexpr (std::is_same_v<T, StartArchiveCommand>) {
                    const State state = currentState();
                    logger_.info("Companion: processing archive.start (service_state=" +
                                 stateToString(state) + " armed=" +
                                 (monitor_.isArmed() ? "true" : "false") + " transport_connected=" +
                                 (client_.isConnected() ? "true" : "false") + ")");
                    if (isArchiveBusy(state)) {
                        logger_.warn("Companion: archive.start rejected — archive already running "
                                     "(state=" +
                                     stateToString(state) + ")");
                    } else if (monitor_.isArmed()) {
                        logger_.warn("Companion: archive.start rejected — vehicle is armed");
                    } else if (!client_.isConnected()) {
                        logger_.warn("Companion: archive.start rejected — MAVLink transport not "
                                     "connected");
                    } else {
                        logger_.info("Companion: archive.start accepted — transitioning to "
                                     "Enumerate");
                        archive_requested_.store(false);
                        downloader_.resetCancel();
                        setState(State::Enumerate);
                    }
                } else if constexpr (std::is_same_v<T, CancelArchiveCommand>) {
                    logger_.info("Companion: processing archive.cancel (service_state=" +
                                 stateToString(currentState()) + ")");
                    downloader_.requestCancel();
                } else if constexpr (std::is_same_v<T, RefreshLogsCommand>) {
                    logger_.info("Companion: processing logs.refresh");
                    downloader_.resetCancel();
                    setState(State::ManualRefresh);
                } else if constexpr (std::is_same_v<T, DownloadLogsCommand>) {
                    logger_.info("Companion: processing logs.download (ids=" +
                                 std::to_string(c.ids.size()) +
                                 (c.all ? " all=true" : "") + ")");
                    manual_download_ids_ = c.ids;
                    manual_download_all_ = c.all;
                    downloader_.resetCancel();
                    setState(State::ManualDownload);
                } else if constexpr (std::is_same_v<T, EraseAllLogsCommand>) {
                    logger_.info("Companion: processing logs.erase (super-delete)");
                    downloader_.resetCancel();
                    setState(State::ManualErase);
                }
            },
            cmd);
    }
}

} // namespace mcls
