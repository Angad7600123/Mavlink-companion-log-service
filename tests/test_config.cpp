#include "mcls/Config.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

TEST(ConfigTest, LoadsTransportTcp) {
    const auto path = std::filesystem::temp_directory_path() / "mcls_test_config.toml";
    std::ofstream out(path);
    out << R"(
[transport]
transport = "tcp"
host = "192.168.1.10"
port = 5762
heartbeat_timeout_sec = 8

[download]
delay_after_disarm = 3
probe_bytes = 4096

[storage]
directory = "/tmp/mcls-test"
max_size_gb = 2
)";
    out.close();

    const mcls::Config cfg = mcls::Config::loadFromFile(path.string());
    EXPECT_EQ(cfg.transport.transport, "tcp");
    EXPECT_EQ(cfg.transport.host, "192.168.1.10");
    EXPECT_EQ(cfg.transport.port, 5762);
    EXPECT_EQ(cfg.transport.heartbeat_timeout_sec, 8);
    EXPECT_EQ(cfg.download.delay_after_disarm_sec, 3);
    EXPECT_EQ(cfg.download.probe_bytes, 4096);
    EXPECT_EQ(cfg.storage.directory, "/tmp/mcls-test");
    EXPECT_EQ(cfg.storage.max_size_gb, 2);
    EXPECT_TRUE(cfg.download.verify_after_download);
    EXPECT_TRUE(cfg.download.erase_after_success);
    EXPECT_EQ(cfg.download.max_queued_log_data, 2048);
    EXPECT_EQ(cfg.download.gap_fill_idle_ms, 500);
    EXPECT_TRUE(cfg.download.verify_dataflash_parse);
    EXPECT_DOUBLE_EQ(cfg.download.verify_max_bad_header_ratio, 0.001);
    EXPECT_TRUE(cfg.download.detect_overlap_conflict);
    EXPECT_EQ(cfg.download.verify_fc_reread, "sample");
    EXPECT_EQ(cfg.download.verify_fc_reread_sample_count, 64);
    EXPECT_FALSE(cfg.download.benchmark_download);

    std::filesystem::remove(path);
}

TEST(ConfigTest, LoadsCompanionDefaults) {
    const auto path = std::filesystem::temp_directory_path() / "mcls_test_companion_defaults.toml";
    {
        std::ofstream out(path);
        out << R"(
[transport]
transport = "tcp"
host = "127.0.0.1"
port = 5760
)";
        out.close();
    }

    const mcls::Config cfg = mcls::Config::loadFromFile(path.string());
    EXPECT_FALSE(cfg.companion.enabled);
    EXPECT_EQ(cfg.companion.bind_host, "127.0.0.1");
    EXPECT_EQ(cfg.companion.bind_port, 14541);
    EXPECT_EQ(cfg.companion.send_host, "127.0.0.1");
    EXPECT_EQ(cfg.companion.send_port, 14540);
    EXPECT_TRUE(cfg.companion.token.empty());
    EXPECT_EQ(cfg.companion.max_request_bytes, 2048);
    EXPECT_EQ(cfg.companion.max_response_bytes, 1200);
    EXPECT_EQ(cfg.companion.max_fc_logs_per_response, 8);

    std::filesystem::remove(path);
}

TEST(ConfigTest, LoadsCompanionSection) {
    const auto path = std::filesystem::temp_directory_path() / "mcls_test_companion.toml";
    {
        std::ofstream out(path);
        out << R"(
[transport]
transport = "tcp"
host = "127.0.0.1"
port = 5760

[companion]
enabled = true
bind_host = "127.0.0.1"
bind_port = 14541
send_host = "127.0.0.1"
send_port = 14540
token = "secret"
max_request_bytes = 1024
max_response_bytes = 900
max_fc_logs_per_response = 4
)";
        out.close();
    }

    const mcls::Config cfg = mcls::Config::loadFromFile(path.string());
    EXPECT_TRUE(cfg.companion.enabled);
    EXPECT_EQ(cfg.companion.bind_port, 14541);
    EXPECT_EQ(cfg.companion.send_port, 14540);
    EXPECT_EQ(cfg.companion.token, "secret");
    EXPECT_EQ(cfg.companion.max_request_bytes, 1024);
    EXPECT_EQ(cfg.companion.max_response_bytes, 900);
    EXPECT_EQ(cfg.companion.max_fc_logs_per_response, 4);

    std::filesystem::remove(path);
}

TEST(ConfigTest, LoadsTransportUdpAndLegacyMavlinkSection) {
    const auto path = std::filesystem::temp_directory_path() / "mcls_test_udp_config.toml";
    {
        std::ofstream out(path);
        out << R"(
[transport]
transport = "udp"
host = "127.0.0.1"
port = 14550
bind_host = "0.0.0.0"
bind_port = 14551
)";
        out.close();

        const mcls::Config cfg = mcls::Config::loadFromFile(path.string());
        EXPECT_EQ(cfg.transport.transport, "udp");
        EXPECT_EQ(cfg.transport.bind_host, "0.0.0.0");
        EXPECT_EQ(cfg.transport.bind_port, 14551);
    }

    {
        std::ofstream out(path);
        out << R"(
[mavlink]
transport = "tcp"
host = "10.0.0.2"
port = 5760
)";
        out.close();

        const mcls::Config cfg = mcls::Config::loadFromFile(path.string());
        EXPECT_EQ(cfg.transport.transport, "tcp");
        EXPECT_EQ(cfg.transport.host, "10.0.0.2");
    }

    std::filesystem::remove(path);
}
