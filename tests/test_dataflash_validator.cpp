#include "mcls/DataFlashValidator.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace {

// ArduPilot log_Format on-disk layout (89 bytes total).
void appendFmtRecord(std::vector<uint8_t>& buf, uint8_t type_id, uint8_t length, const char name[4]) {
    buf.push_back(0xA3);
    buf.push_back(0x95);
    buf.push_back(0x80);
    buf.push_back(type_id);
    buf.push_back(length);
    buf.insert(buf.end(), name, name + 4);
    buf.insert(buf.end(), 16, 'b');
    buf.insert(buf.end(), 64, 'L');
}

void appendMessage(std::vector<uint8_t>& buf, uint8_t msg_type, uint8_t total_len) {
    ASSERT_GE(total_len, 3u);
    buf.push_back(0xA3);
    buf.push_back(0x95);
    buf.push_back(msg_type);
    buf.insert(buf.end(), static_cast<std::size_t>(total_len - 3), 0x00);
}

} // namespace

TEST(DataFlashValidatorTest, ParsesFmtTypeAndLengthAtCorrectOffsets) {
    std::vector<uint8_t> buf;
    appendFmtRecord(buf, 42, 37, {'T', 'E', 'S', 'T'});
    appendMessage(buf, 42, 37);

    const auto result = mcls::validateDataFlashBuffer(buf.data(), buf.size());
    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.resync_count, 0u);
    EXPECT_EQ(result.bad_message_count, 0u);
    EXPECT_EQ(result.bytes_scanned, buf.size());
}

TEST(DataFlashValidatorTest, ValidatesMinimalFmtBootstrap) {
    std::vector<uint8_t> buf;
    appendFmtRecord(buf, 0x01, 10, {'F', 'M', 'T', ' '});

    const auto result = mcls::validateDataFlashBuffer(buf.data(), buf.size());
    EXPECT_TRUE(result.ok);
}

TEST(DataFlashValidatorTest, ParsesFmtFromFile) {
    const auto path = std::filesystem::temp_directory_path() / "mcls_fmt_test.bin";
    std::vector<uint8_t> buf;
    appendFmtRecord(buf, 42, 37, {'T', 'E', 'S', 'T'});
    appendMessage(buf, 42, 37);

    {
        std::ofstream out(path, std::ios::binary);
        ASSERT_TRUE(out);
        out.write(reinterpret_cast<const char*>(buf.data()),
                  static_cast<std::streamsize>(buf.size()));
    }

    const auto result = mcls::validateDataFlashFile(path);
    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.bytes_scanned, buf.size());

    std::filesystem::remove(path);
}

TEST(DataFlashValidatorTest, RejectsGarbage) {
    const uint8_t garbage[] = {0, 1, 2, 3, 4, 5};
    const auto result = mcls::validateDataFlashBuffer(garbage, sizeof(garbage));
    EXPECT_FALSE(result.ok);
}

TEST(DataFlashValidatorTest, ResyncRatioThreshold) {
    mcls::DataFlashValidationResult result;
    result.resync_count = 4000;
    EXPECT_TRUE(mcls::dataFlashResyncRatioExceeded(result, 3'000'000, 0.001));
    EXPECT_FALSE(mcls::dataFlashResyncRatioExceeded(result, 3'000'000, 0.01));
}
