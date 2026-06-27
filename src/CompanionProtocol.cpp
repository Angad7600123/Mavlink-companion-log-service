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

        // fc.logs pagination
        req.offset = obj.value("offset", 0);
        req.limit = obj.value("limit", 8);

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

json baseResponse(int request_id, bool ok, bool truncated = false) {
    json r;
    r["v"] = 1;
    r["id"] = request_id;
    r["ok"] = ok;
    r["truncated"] = truncated;
    if (ok) {
        r["err"] = nullptr;
    }
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// buildStatus (Tier 1 — fixed fields, must always fit)
// ---------------------------------------------------------------------------

SerializedResponse buildStatus(int request_id,
                               const ServiceSnapshot& snap,
                               std::size_t max_bytes) {
    json r = baseResponse(request_id, true);

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

    data["archive"]["last_cycle"]["downloaded"] = snap.last_cycle_downloaded;
    data["archive"]["last_cycle"]["skipped"] = snap.last_cycle_skipped;
    data["archive"]["last_cycle"]["failed"] = snap.last_cycle_failed;
    data["archive"]["last_cycle"]["cancelled"] = snap.last_cycle_cancelled;
    data["archive"]["last_cycle"]["all_archived"] = snap.last_cycle_all_archived;

    data["storage"]["used_bytes"] = snap.storage_used_bytes;
    data["storage"]["limit_bytes"] = snap.storage_limit_bytes;
    data["storage"]["archived_count"] = snap.storage_archived_count;

    r["data"] = data;

    const std::string s = trySerialize(r, max_bytes);
    if (s.empty()) {
        // Status exceeded budget — drop optional fields in fixed order and flag truncated.
        // In practice this should not happen given the fixed Tier 1 field set; the unit
        // test enforces this invariant. Fall back to a minimal "truncated" response rather
        // than silently dropping the send.
        json minimal = baseResponse(request_id, true, /*truncated=*/true);
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
                               std::size_t max_bytes) {
    auto build = [&](const FcLogsPage& p) {
        json r = baseResponse(request_id, true, p.truncated);
        json data;
        data["count"] = p.count;
        data["offset"] = p.offset;
        data["stale"] = p.stale;
        json entries = json::array();
        for (const auto& e : p.entries) {
            entries.push_back({{"id", e.id}, {"size", e.size}});
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
    return {buildError(request_id, "internal").json, false};
}

// ---------------------------------------------------------------------------
// buildAck / buildError
// ---------------------------------------------------------------------------

SerializedResponse buildAck(int request_id, bool ok, const std::string& err) {
    json r = baseResponse(request_id, ok);
    if (!ok && !err.empty()) {
        r["err"] = err;
    }
    return {r.dump(), false};
}

SerializedResponse buildError(int request_id, const std::string& err) {
    json r;
    r["v"] = 1;
    r["id"] = request_id;
    r["ok"] = false;
    r["err"] = err;
    r["truncated"] = false;
    return {r.dump(), false};
}


} // namespace CompanionProtocol
} // namespace mcls
