#pragma once

#include <cstddef>
#include <cstdint>

namespace mcls {

uint32_t crc32(const uint8_t* data, std::size_t len, uint32_t seed = 0xFFFFFFFFU);

} // namespace mcls
