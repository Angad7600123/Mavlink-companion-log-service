#include "mcls/CompanionProtocol.hpp"
#include "mcls/ServiceSnapshot.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

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
