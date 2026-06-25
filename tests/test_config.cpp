#include "mcls/Config.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

TEST(ConfigTest, LoadsDefaultsFromFile) {
    const auto path = std::filesystem::temp_directory_path() / "mcls_test_config.toml";
    std::ofstream out(path);
    out << R"(
[mavlink]
host = "192.168.1.10"
port = 5762

[download]
delay_after_disarm = 3
probe_bytes = 4096

[storage]
directory = "/tmp/mcls-test"
max_size_gb = 2
)";
    out.close();

    const mcls::Config cfg = mcls::Config::loadFromFile(path.string());
    EXPECT_EQ(cfg.mavlink.host, "192.168.1.10");
    EXPECT_EQ(cfg.mavlink.port, 5762);
    EXPECT_EQ(cfg.download.delay_after_disarm_sec, 3);
    EXPECT_EQ(cfg.download.probe_bytes, 4096);
    EXPECT_EQ(cfg.storage.directory, "/tmp/mcls-test");
    EXPECT_EQ(cfg.storage.max_size_gb, 2);
    EXPECT_TRUE(cfg.download.verify_after_download);
    EXPECT_TRUE(cfg.download.erase_after_success);

    std::filesystem::remove(path);
}
