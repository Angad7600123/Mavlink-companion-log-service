#include "mcls/DataFlashValidator.hpp"

#include <gtest/gtest.h>
#include <vector>

TEST(DataFlashValidatorTest, ValidatesMinimalFmtBootstrap) {
    std::vector<uint8_t> buf;
    buf.push_back(0xA3);
    buf.push_back(0x95);
    buf.push_back(0x80);
    buf.insert(buf.end(), {'F', 'M', 'T', ' '});
    buf.push_back(0x01);
    buf.push_back(10);
    buf.insert(buf.end(), 16, 'b');
    buf.insert(buf.end(), 64, 'L');

    const auto result = mcls::validateDataFlashBuffer(buf.data(), buf.size());
    EXPECT_TRUE(result.ok);
}

TEST(DataFlashValidatorTest, RejectsGarbage) {
    const uint8_t garbage[] = {0, 1, 2, 3, 4, 5};
    const auto result = mcls::validateDataFlashBuffer(garbage, sizeof(garbage));
    EXPECT_FALSE(result.ok);
}
