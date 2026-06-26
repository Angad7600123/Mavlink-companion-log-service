#include "mcls/Crc32.hpp"

namespace mcls {

uint32_t crc32(const uint8_t* data, const std::size_t len, uint32_t crc) {
    crc = ~crc;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = -(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

} // namespace mcls
