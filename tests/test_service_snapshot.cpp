#include "mcls/ServiceSnapshot.hpp"

#include <gtest/gtest.h>

// ServiceSnapshot is a plain struct; these tests verify default construction
// and that field types are correct as a compile-time check.

TEST(ServiceSnapshot, DefaultConstruction) {
    const mcls::ServiceSnapshot s;
    EXPECT_TRUE(s.state.empty());
    EXPECT_TRUE(s.version.empty());
    EXPECT_FALSE(s.transport_connected);
    EXPECT_FALSE(s.heartbeat_fresh);
    EXPECT_FALSE(s.vehicle_detected);
    EXPECT_FALSE(s.vehicle_armed);
    EXPECT_EQ(s.fc_logs_count, 0);
    EXPECT_TRUE(s.fc_logs_stale);
    EXPECT_FALSE(s.archive_active);
    EXPECT_EQ(s.archive_current_log_id, 0);
    EXPECT_EQ(s.archive_progress_bytes, 0u);
    EXPECT_EQ(s.archive_progress_total_bytes, 0u);
    EXPECT_EQ(s.last_cycle_downloaded, 0);
    EXPECT_EQ(s.last_cycle_skipped, 0);
    EXPECT_EQ(s.last_cycle_failed, 0);
    EXPECT_EQ(s.last_cycle_cancelled, 0);
    EXPECT_FALSE(s.last_cycle_all_archived);
    EXPECT_EQ(s.storage_used_bytes, 0u);
    EXPECT_EQ(s.storage_limit_bytes, 0u);
    EXPECT_EQ(s.storage_archived_count, 0);
}

TEST(ServiceSnapshot, FcLogsPageDefaults) {
    const mcls::FcLogsPage page;
    EXPECT_EQ(page.count, 0);
    EXPECT_EQ(page.offset, 0);
    EXPECT_TRUE(page.entries.empty());
    EXPECT_FALSE(page.truncated);
    EXPECT_TRUE(page.stale);
}

TEST(ServiceSnapshot, FcLogEntryFields) {
    mcls::FcLogEntry e;
    e.id = 17;
    e.size = 4200000;
    EXPECT_EQ(e.id, 17);
    EXPECT_EQ(e.size, 4200000u);
}
