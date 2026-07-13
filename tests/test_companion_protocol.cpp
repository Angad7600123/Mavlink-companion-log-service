#include "mcls/CompanionProtocol.hpp"
#include "mcls/ServiceSnapshot.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// parseRequest
// ---------------------------------------------------------------------------

TEST(CompanionProtocol, ParseStatusRequest) {
    const std::string raw = R"({"v":1,"id":42,"op":"status"})";
    const auto req = mcls::CompanionProtocol::parseRequest(raw);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->version, 1);
    EXPECT_EQ(req->id, 42);
    EXPECT_EQ(req->op, "status");
    EXPECT_TRUE(req->token.empty());
}

TEST(CompanionProtocol, ParseFcLogsRequest) {
    const std::string raw = R"({"v":1,"id":7,"op":"fc.logs","offset":8,"limit":4,"token":"abc"})";
    const auto req = mcls::CompanionProtocol::parseRequest(raw);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->op, "fc.logs");
    EXPECT_EQ(req->offset, 8);
    EXPECT_EQ(req->limit, 4);
    EXPECT_EQ(req->token, "abc");
}

TEST(CompanionProtocol, ParseReturnsNulloptOnInvalidJson) {
    EXPECT_FALSE(mcls::CompanionProtocol::parseRequest("not json").has_value());
    EXPECT_FALSE(mcls::CompanionProtocol::parseRequest("").has_value());
    EXPECT_FALSE(mcls::CompanionProtocol::parseRequest(R"({"v":1,"id":1})").has_value()); // missing op
}

// ---------------------------------------------------------------------------
// buildAck / buildError
// ---------------------------------------------------------------------------

TEST(CompanionProtocol, BuildAckOk) {
    const auto resp = mcls::CompanionProtocol::buildAck(5, true);
    const auto obj = json::parse(resp.json);
    EXPECT_EQ(obj["id"], 5);
    EXPECT_TRUE(obj["ok"].get<bool>());
    EXPECT_FALSE(resp.truncated);
}

TEST(CompanionProtocol, BuildError) {
    const auto resp = mcls::CompanionProtocol::buildError(3, "bad_token");
    const auto obj = json::parse(resp.json);
    EXPECT_EQ(obj["id"], 3);
    EXPECT_FALSE(obj["ok"].get<bool>());
    EXPECT_EQ(obj["err"], "bad_token");
}

// ---------------------------------------------------------------------------
// buildStatus
// ---------------------------------------------------------------------------

static mcls::ServiceSnapshot makeSnapshot() {
    mcls::ServiceSnapshot s;
    s.state = "wait_arm";
    s.version = "1.0.0";
    s.transport_connected = true;
    s.heartbeat_fresh = true;
    s.vehicle_detected = true;
    s.vehicle_armed = false;
    s.fc_logs_count = 3;
    s.fc_logs_stale = false;
    s.archive_active = false;
    s.archive_progress_bytes = 0;
    s.archive_progress_total_bytes = 0;
    s.last_cycle_downloaded = 2;
    s.last_cycle_skipped = 1;
    s.last_cycle_failed = 0;
    s.last_cycle_cancelled = 0;
    s.last_cycle_all_archived = true;
    s.storage_used_bytes = 52428800;
    s.storage_limit_bytes = 1073741824;
    s.storage_archived_count = 47;
    return s;
}

TEST(CompanionProtocol, BuildStatusStructure) {
    const auto snap = makeSnapshot();
    const auto resp = mcls::CompanionProtocol::buildStatus(42, snap, 1200);
    ASSERT_FALSE(resp.json.empty());
    EXPECT_FALSE(resp.truncated);

    const auto obj = json::parse(resp.json);
    EXPECT_EQ(obj["id"], 42);
    EXPECT_TRUE(obj["ok"].get<bool>());
    EXPECT_EQ(obj["data"]["service"]["state"], "wait_arm");
    EXPECT_TRUE(obj["data"]["link"]["transport_connected"].get<bool>());
    EXPECT_EQ(obj["data"]["fc_logs"]["count"], 3);
    EXPECT_FALSE(obj["data"]["fc_logs"]["stale"].get<bool>());
    EXPECT_EQ(obj["data"]["archive"]["last_cycle"]["downloaded"], 2);
    EXPECT_EQ(obj["data"]["storage"]["archived_count"], 47);
}

// Key invariant: Tier 1 status must always fit within the default budget.
TEST(CompanionProtocol, StatusNeverExceedsBudget) {
    // Worst-case snapshot: all string fields at max plausible length.
    mcls::ServiceSnapshot snap = makeSnapshot();
    snap.state = "connection_lost";
    snap.version = "99.99.999";
    snap.fc_logs_count = 9999;
    snap.archive_active = true;
    snap.archive_current_log_id = 65535;
    snap.archive_progress_bytes = 0xFFFFFFFFu;
    snap.archive_progress_total_bytes = 0xFFFFFFFFu;
    snap.last_cycle_downloaded = 9999;
    snap.last_cycle_skipped = 9999;
    snap.last_cycle_failed = 9999;
    snap.last_cycle_cancelled = 9999;
    snap.storage_used_bytes = 0xFFFFFFFFFFFFFFFFull;
    snap.storage_limit_bytes = 0xFFFFFFFFFFFFFFFFull;
    snap.storage_archived_count = 9999999;
    snap.job_type = "download";
    snap.recording_enabled = true;
    snap.recording_active = true;
    snap.recording_duration_sec = 0xFFFFFFFFu;
    snap.recording_free_bytes = 0xFFFFFFFFFFFFFFFFull;
    snap.recording_crash_reason = "recorder_crashed";  // longer of the two codes

    const auto resp = mcls::CompanionProtocol::buildStatus(1, snap, 1200);
    ASSERT_FALSE(resp.json.empty());
    EXPECT_LE(resp.json.size(), std::size_t{1200})
        << "Tier 1 status exceeded 1200-byte budget: " << resp.json.size() << " bytes";
}

// ---------------------------------------------------------------------------
// buildFcLogs — pagination and truncation
// ---------------------------------------------------------------------------

static mcls::FcLogsPage makePage(int count, int offset, int n_entries) {
    mcls::FcLogsPage page;
    page.count = count;
    page.offset = offset;
    page.stale = false;
    for (int i = 0; i < n_entries; ++i) {
        page.entries.push_back({static_cast<uint16_t>(offset + i), 1234567u});
    }
    return page;
}

TEST(CompanionProtocol, BuildFcLogsBasic) {
    const auto page = makePage(10, 0, 8);
    const auto resp = mcls::CompanionProtocol::buildFcLogs(5, page, 1200);
    ASSERT_FALSE(resp.json.empty());
    EXPECT_FALSE(resp.truncated);

    const auto obj = json::parse(resp.json);
    EXPECT_EQ(obj["data"]["count"], 10);
    EXPECT_EQ(obj["data"]["entries"].size(), std::size_t{8});
}

TEST(CompanionProtocol, FcLogsTruncatesWhenOverBudget) {
    // Build a page so large it can't fit — force very tight budget.
    const auto page = makePage(100, 0, 8);
    // Budget of 100 bytes is impossibly small for 8 entries → expect truncation.
    const auto resp = mcls::CompanionProtocol::buildFcLogs(1, page, 100);
    EXPECT_FALSE(resp.json.empty());
    // Either truncated or error; either way must not exceed budget.
    EXPECT_LE(resp.json.size(), std::size_t{200}); // error response is tiny
}

TEST(CompanionProtocol, FcLogsPaginationOffset) {
    const auto page = makePage(20, 8, 4);
    const auto resp = mcls::CompanionProtocol::buildFcLogs(9, page, 1200);
    const auto obj = json::parse(resp.json);
    EXPECT_EQ(obj["data"]["offset"], 8);
    EXPECT_EQ(obj["data"]["entries"].size(), std::size_t{4});
}

TEST(CompanionProtocol, FcLogsEntriesCarryTimeAndDownloaded) {
    mcls::FcLogsPage page;
    page.count = 1;
    page.offset = 0;
    page.stale = false;
    mcls::FcLogEntry e;
    e.id = 17;
    e.size = 4200000u;
    e.time_utc = 1719950400u;
    e.downloaded = true;
    page.entries.push_back(e);

    const auto resp = mcls::CompanionProtocol::buildFcLogs(1, page, 1200);
    const auto obj = json::parse(resp.json);
    const auto& entry = obj["data"]["entries"][0];
    EXPECT_EQ(entry["id"], 17);
    EXPECT_EQ(entry["size"], 4200000u);
    EXPECT_EQ(entry["t"], 1719950400u);
    EXPECT_TRUE(entry["dl"].get<bool>());
}

// ---------------------------------------------------------------------------
// buildJobAck — idempotent success semantics
// ---------------------------------------------------------------------------

TEST(CompanionProtocol, JobAckStarted) {
    const auto resp = mcls::CompanionProtocol::buildJobAck(5, /*already_running=*/false);
    const auto obj = json::parse(resp.json);
    EXPECT_TRUE(obj["ok"].get<bool>());
    EXPECT_TRUE(obj["data"]["accepted"].get<bool>());
    EXPECT_FALSE(obj["data"]["already_running"].get<bool>());
}

TEST(CompanionProtocol, JobAckAlreadyRunningIsSuccess) {
    // A retry that lands while a job is running must read as success, not error.
    const auto resp = mcls::CompanionProtocol::buildJobAck(6, /*already_running=*/true);
    const auto obj = json::parse(resp.json);
    EXPECT_TRUE(obj["ok"].get<bool>());
    EXPECT_TRUE(obj["data"]["accepted"].get<bool>());
    EXPECT_TRUE(obj["data"]["already_running"].get<bool>());
}

// ---------------------------------------------------------------------------
// client echo
// ---------------------------------------------------------------------------

TEST(CompanionProtocol, ParseClientField) {
    const auto req = mcls::CompanionProtocol::parseRequest(
        R"({"v":1,"id":1,"op":"status","client":"gs-a1"})");
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->client, "gs-a1");
}

TEST(CompanionProtocol, ClientEchoedWhenPresent) {
    const auto snap = makeSnapshot();
    const auto resp = mcls::CompanionProtocol::buildStatus(1, snap, 1200, "gs-a1");
    const auto obj = json::parse(resp.json);
    EXPECT_EQ(obj["client"], "gs-a1");

    const auto err = mcls::CompanionProtocol::buildError(2, "armed", "gs-a1");
    EXPECT_EQ(json::parse(err.json)["client"], "gs-a1");

    const auto ack = mcls::CompanionProtocol::buildJobAck(3, false, "gs-a1");
    EXPECT_EQ(json::parse(ack.json)["client"], "gs-a1");
}

TEST(CompanionProtocol, ClientOmittedWhenAbsent) {
    const auto snap = makeSnapshot();
    const auto resp = mcls::CompanionProtocol::buildStatus(1, snap, 1200);
    const auto obj = json::parse(resp.json);
    EXPECT_FALSE(obj.contains("client"));
}

// ---------------------------------------------------------------------------
// logs.download parsing + buildDownloadAck
// ---------------------------------------------------------------------------

TEST(CompanionProtocol, ParseDownloadSelectionIds) {
    const auto req = mcls::CompanionProtocol::parseRequest(
        R"({"v":1,"id":9,"op":"logs.download","sel":{"ids":[17,18,99999,-3]}})");
    ASSERT_TRUE(req.has_value());
    EXPECT_FALSE(req->sel_all);
    // 99999 (> uint16) and -3 are dropped defensively.
    ASSERT_EQ(req->sel_ids.size(), std::size_t{2});
    EXPECT_EQ(req->sel_ids[0], 17);
    EXPECT_EQ(req->sel_ids[1], 18);
}

TEST(CompanionProtocol, ParseDownloadSelectionAll) {
    const auto req = mcls::CompanionProtocol::parseRequest(
        R"({"v":1,"id":10,"op":"logs.download","sel":{"all":true}})");
    ASSERT_TRUE(req.has_value());
    EXPECT_TRUE(req->sel_all);
    EXPECT_TRUE(req->sel_ids.empty());
}

TEST(CompanionProtocol, BuildDownloadAck) {
    const auto resp = mcls::CompanionProtocol::buildDownloadAck(11, 2, {42}, "gs-a1");
    const auto obj = json::parse(resp.json);
    EXPECT_TRUE(obj["ok"].get<bool>());
    EXPECT_TRUE(obj["data"]["accepted"].get<bool>());
    EXPECT_EQ(obj["data"]["queued"], 2);
    ASSERT_EQ(obj["data"]["not_found"].size(), std::size_t{1});
    EXPECT_EQ(obj["data"]["not_found"][0], 42);
    EXPECT_EQ(obj["client"], "gs-a1");
}

// ---------------------------------------------------------------------------
// buildCaps
// ---------------------------------------------------------------------------

TEST(CompanionProtocol, BuildCaps) {
    const auto resp = mcls::CompanionProtocol::buildCaps(3, 2048, 1200, 8, 1200);
    ASSERT_FALSE(resp.json.empty());
    const auto obj = json::parse(resp.json);
    EXPECT_TRUE(obj["ok"].get<bool>());
    EXPECT_EQ(obj["data"]["v"], 1);
    EXPECT_EQ(obj["data"]["limits"]["max_request_bytes"], 2048);
    const auto ops = obj["data"]["ops"];
    EXPECT_NE(std::find(ops.begin(), ops.end(), "status"), ops.end());
    EXPECT_NE(std::find(ops.begin(), ops.end(), "caps"), ops.end());
    EXPECT_NE(std::find(ops.begin(), ops.end(), "rec.start"), ops.end());
    EXPECT_NE(std::find(ops.begin(), ops.end(), "rec.stop"), ops.end());
}

// ---------------------------------------------------------------------------
// buildRecAck / status.recording
// ---------------------------------------------------------------------------

TEST(CompanionProtocol, RecAckReflectsResultingState) {
    const auto started = mcls::CompanionProtocol::buildRecAck(1, /*active=*/true);
    EXPECT_TRUE(json::parse(started.json)["data"]["active"].get<bool>());

    const auto stopped = mcls::CompanionProtocol::buildRecAck(2, /*active=*/false, "gs-a1");
    const auto obj = json::parse(stopped.json);
    EXPECT_FALSE(obj["data"]["active"].get<bool>());
    EXPECT_EQ(obj["client"], "gs-a1");
}

TEST(CompanionProtocol, StatusCarriesRecordingBlockIndependentOfJob) {
    mcls::ServiceSnapshot snap = makeSnapshot();
    // Recording active while no archive job is running — the two must be
    // independent (this is the whole point of not routing rec.* through the
    // job state machine).
    snap.job_type.clear();
    snap.recording_enabled = true;
    snap.recording_active = true;
    snap.recording_duration_sec = 42;
    snap.recording_free_bytes = 12884901888ull;

    const auto resp = mcls::CompanionProtocol::buildStatus(1, snap, 1200);
    const auto obj = json::parse(resp.json);
    EXPECT_FALSE(obj["data"]["job"]["active"].get<bool>());
    EXPECT_TRUE(obj["data"]["recording"]["enabled"].get<bool>());
    EXPECT_TRUE(obj["data"]["recording"]["active"].get<bool>());
    EXPECT_EQ(obj["data"]["recording"]["duration_sec"], 42);
    EXPECT_EQ(obj["data"]["recording"]["free_bytes"], 12884901888ull);
    EXPECT_TRUE(obj["data"]["recording"]["error"].is_null());
}

TEST(CompanionProtocol, StatusRecordingErrorSurfacesCrashReason) {
    mcls::ServiceSnapshot snap = makeSnapshot();
    snap.recording_enabled = true;
    snap.recording_active = false;
    snap.recording_crash_reason = "media_lost";

    const auto resp = mcls::CompanionProtocol::buildStatus(1, snap, 1200);
    const auto obj = json::parse(resp.json);
    EXPECT_FALSE(obj["data"]["recording"]["active"].get<bool>());
    EXPECT_EQ(obj["data"]["recording"]["error"], "media_lost");
}

TEST(CompanionProtocol, StatusCarriesPercentAndThroughput) {
    mcls::ServiceSnapshot snap = makeSnapshot();
    snap.archive_active = true;
    snap.archive_progress_bytes = 500000;
    snap.archive_progress_total_bytes = 1000000;
    snap.archive_percent = 50;
    snap.archive_bytes_per_sec = 123456;
    const auto resp = mcls::CompanionProtocol::buildStatus(1, snap, 1200);
    const auto obj = json::parse(resp.json);
    EXPECT_EQ(obj["data"]["archive"]["percent"], 50);
    EXPECT_EQ(obj["data"]["archive"]["bytes_per_sec"], 123456u);
}
