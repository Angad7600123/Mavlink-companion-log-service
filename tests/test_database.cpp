#include "mcls/Database.hpp"

#include <gtest/gtest.h>

#include <filesystem>

TEST(DatabaseTest, CandidateLookupAndStats) {
    const auto dir = std::filesystem::temp_directory_path() / "mcls_db_test";
    std::filesystem::create_directories(dir);
    const auto db_path = dir / "database.sqlite";

    mcls::Database db(db_path);
    db.initialize();

    db.insertArchivedLog("fullhash", 17, 4200000, 1719000000, "probehash", 51200,
                         1719000100, 3200, "/var/lib/mcls/logs/2026-06-25/test.bin");

    const auto candidate = db.findCandidate(17, 4200000);
    ASSERT_TRUE(candidate.has_value());
    EXPECT_EQ(candidate->probe_sha256, "probehash");
    EXPECT_EQ(candidate->sha256, "fullhash");

    EXPECT_FALSE(db.findCandidate(18, 1000).has_value());

    db.incrementStat("flights_archived");
    db.incrementStat("bytes_downloaded", 4200000);
    EXPECT_EQ(db.getStat("flights_archived"), 1);
    EXPECT_EQ(db.getStat("bytes_downloaded"), 4200000);

    std::filesystem::remove_all(dir);
}
