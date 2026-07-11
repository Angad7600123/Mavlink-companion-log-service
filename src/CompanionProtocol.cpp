#include "mcls/CompanionProtocol.hpp"

#include <nlohmann/json.hpp>

namespace mcls {
namespace CompanionProtocol {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Parse
// ---------------------------------------------------------------------------

std::optional<CompanionRequest> parseRequest(const std::string& json_text) {
    try {
        const auto obj = json::parse(json_text);

        CompanionRequest req;
        req.version = obj.value("v", 0);
        req.id = obj.value("id", 0);
        req.op = obj.value("op", std::string{});
        req.token = obj.value("token", std::string{});
        req.client = obj.value("client", std::string{});

        // fc.logs pagination
        req.offset = obj.value("offset", 0);
        req.limit = obj.value("limit", 8);

        // logs.download selection: "sel":{"ids":[..]} or "sel":{"all":true}
        if (obj.contains("sel") && obj["sel"].is_object()) {
            const auto& sel = obj["sel"];
            req.sel_all = sel.value("all", false);
            if (sel.contains("ids") && sel["ids"].is_array()) {
                for (const auto& v : sel["ids"]) {
                    if (v.is_number_integer() || v.is_number_unsigned()) {
                        const auto raw = v.get<std::int64_t>();
                        if (raw >= 0 && raw <= 0xFFFF) {
                            req.sel_ids.push_back(static_cast<std::uint16_t>(raw));
                        }
                    }
                }
            }
        }

        if (req.op.empty()) {
            return std::nullopt;
        }
        return req;
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/// Try to serialize obj; if result exceeds max_bytes return empty string.
std::string trySerialize(const json& obj, std::size_t max_bytes) {
    const std::string s = obj.dump(-1, ' ', false, json::error_handler_t::replace);
    if (s.size() > max_bytes) {
        return {};
    }
    return s;
}

json baseResponse(int request_id,
                  bool ok,
                  bool truncated = false,
                  const std::string& client = {}) {
    json r;
    r["v"] = 1;
    r["id"] = request_id;
    r["ok"] = ok;
    r["truncated"] = truncated;
    if (ok) {
        r["err"] = nullptr;
    }
    if (!client.empty()) {
        r["client"] = client;
    }
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// buildStatus (Tier 1 — fixed fields, must always fit)
// ---------------------------------------------------------------------------

SerializedResponse buildStatus(int request_id,
                               const ServiceSnapshot& snap,
                               std::size_t max_bytes,
                               const std::string& client) {
    json r = baseResponse(request_id, true, false, client);

    json data;
    data["service"]["state"] = snap.state;
    data["service"]["version"] = snap.version;

    data["link"]["transport_connected"] = snap.transport_connected;
    data["link"]["heartbeat_fresh"] = snap.heartbeat_fresh;

    data["vehicle"]["detected"] = snap.vehicle_detected;
    data["vehicle"]["armed"] = snap.vehicle_armed;

    data["fc_logs"]["count"] = snap.fc_logs_count;
    data["fc_logs"]["stale"] = snap.fc_logs_stale;

    data["archive"]["active"] = snap.archive_active;
    data["archive"]["current_log_id"] = snap.archive_current_log_id;
    data["archive"]["progress_bytes"] = snap.archive_progress_bytes;
    data["archive"]["progress_total_bytes"] = snap.archive_progress_total_bytes;
    data["archive"]["percent"] = snap.archive_percent;
    data["archive"]["bytes_per_sec"] = snap.archive_bytes_per_sec;

    data["archive"]["last_cycle"]["downloaded"] = snap.last_cycle_downloaded;
    data["archive"]["last_cycle"]["skipped"] = snap.last_cycle_skipped;
    data["archive"]["last_cycle"]["failed"] = snap.last_cycle_failed;
    data["archive"]["last_cycle"]["cancelled"] = snap.last_cycle_cancelled;
    data["archive"]["last_cycle"]["all_archived"] = snap.last_cycle_all_archived;

    // Manual/automatic job descriptor: null type when idle. The archive block
    // above carries the byte-level progress regardless of job type.
    data["job"]["active"] = !snap.job_type.empty();
    if (snap.job_type.empty()) {
        data["job"]["type"] = nullptr;
    } else {
        data["job"]["type"] = snap.job_type;
    }

    data["storage"]["used_bytes"] = snap.storage_used_bytes;
    data["storage"]["limit_bytes"] = snap.storage_limit_bytes;
    data["storage"]["archived_count"] = snap.storage_archived_count;

    // Onboard Pi video recording — fully independent of the archive/job state
    // above (see VideoRecorder).
    data["recording"]["enabled"] = snap.recording_enabled;
    data["recording"]["active"] = snap.recording_active;
    data["recording"]["duration_sec"] = snap.recording_duration_sec;
    data["recording"]["free_bytes"] = snap.recording_free_bytes;

    r["data"] = data;

    const std::string s = trySerialize(r, max_bytes);
    if (s.empty()) {
        // Status exceeded budget — drop optional fields in fixed order and flag truncated.
        // In practice this should not happen given the fixed Tier 1 field set; the unit
        // test enforces this invariant. Fall back to a minimal "truncated" response rather
        // than silently dropping the send.
        json minimal = baseResponse(request_id, true, /*truncated=*/true, client);
        json md;
        md["service"]["state"] = snap.state;
        md["archive"]["active"] = snap.archive_active;
        md["archive"]["progress_bytes"] = snap.archive_progress_bytes;
        md["archive"]["progress_total_bytes"] = snap.archive_progress_total_bytes;
        minimal["data"] = md;
        return {trySerialize(minimal, max_bytes), true};
    }
    return {s, false};
}

// ---------------------------------------------------------------------------
// buildFcLogs (Tier 2 — paginated, degradation ladder)
// ---------------------------------------------------------------------------

SerializedResponse buildFcLogs(int request_id,
                               const FcLogsPage& page,
                               std::size_t max_bytes,
                               const std::string& client) {
    auto build = [&](const FcLogsPage& p) {
        json r = baseResponse(request_id, true, p.truncated, client);
        json data;
        data["count"] = p.count;
        data["offset"] = p.offset;
        data["stale"] = p.stale;
        json entries = json::array();
        for (const auto& e : p.entries) {
            entries.push_back(
                {{"id", e.id}, {"size", e.size}, {"t", e.time_utc}, {"dl", e.downloaded}});
        }
        data["entries"] = entries;
        r["data"] = data;
        return r;
    };

    // Try with the full page first.
    std::string s = trySerialize(build(page), max_bytes);
    if (!s.empty()) {
        return {s, page.truncated};
    }

    // Degradation ladder: halve the entry list until it fits.
    FcLogsPage reduced = page;
    reduced.truncated = true;
    while (!reduced.entries.empty()) {
        reduced.entries.resize(reduced.entries.size() / 2);
        s = trySerialize(build(reduced), max_bytes);
        if (!s.empty()) {
            return {s, true};
        }
    }

    // Could not fit even an empty entry list — return an error.
    return {buildError(request_id, "internal", client).json, false};
}

// ---------------------------------------------------------------------------
// buildAck / buildJobAck / buildDownloadAck / buildCaps / buildError
// ---------------------------------------------------------------------------

SerializedResponse buildAck(int request_id,
                            bool ok,
                            const std::string& err,
                            const std::string& client) {
    json r = baseResponse(request_id, ok, false, client);
    if (!ok && !err.empty()) {
        r["err"] = err;
    }
    return {r.dump(), false};
}

SerializedResponse buildJobAck(int request_id, bool already_running, const std::string& client) {
    json r = baseResponse(request_id, true, false, client);
    r["data"]["accepted"] = true;
    r["data"]["already_running"] = already_running;
    return {r.dump(), false};
}

SerializedResponse buildDownloadAck(int request_id,
                                    int queued,
                                    const std::vector<std::uint16_t>& not_found,
                                    const std::string& client) {
    json r = baseResponse(request_id, true, false, client);
    r["data"]["accepted"] = true;
    r["data"]["queued"] = queued;
    r["data"]["not_found"] = not_found;
    return {r.dump(), false};
}

SerializedResponse buildRecAck(int request_id, bool active, const std::string& client) {
    json r = baseResponse(request_id, true, false, client);
    r["data"]["active"] = active;
    return {r.dump(), false};
}

SerializedResponse buildCaps(int request_id,
                             int max_request_bytes,
                             int max_response_bytes,
                             int max_fc_logs_per_response,
                             std::size_t max_bytes,
                             const std::string& client) {
    json r = baseResponse(request_id, true, false, client);
    json data;
    data["v"] = 1;
    data["ops"] = json::array({"status", "fc.logs", "caps", "archive.start", "archive.cancel",
                               "logs.refresh", "logs.download", "logs.erase",
                               "rec.start", "rec.stop"});
    data["limits"]["max_request_bytes"] = max_request_bytes;
    data["limits"]["max_response_bytes"] = max_response_bytes;
    data["limits"]["max_fc_logs_per_response"] = max_fc_logs_per_response;
    data["limits"]["max_ids_per_request"] = kMaxIdsPerRequest;
    r["data"] = data;

    const std::string s = trySerialize(r, max_bytes);
    if (s.empty()) {
        return {buildError(request_id, "internal", client).json, false};
    }
    return {s, false};
}

SerializedResponse buildError(int request_id, const std::string& err, const std::string& client) {
    json r;
    r["v"] = 1;
    r["id"] = request_id;
    r["ok"] = false;
    r["err"] = err;
    r["truncated"] = false;
    if (!client.empty()) {
        r["client"] = client;
    }
    return {r.dump(), false};
}


} // namespace CompanionProtocol
} // namespace mcls
