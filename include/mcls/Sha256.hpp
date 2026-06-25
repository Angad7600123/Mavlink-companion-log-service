#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mcls {

/// Incremental SHA-256 with clone support for probe + full-file hashing.
class Sha256 {
public:
    Sha256();
    void update(const uint8_t* data, std::size_t len);
    void update(const std::vector<uint8_t>& data);
    Sha256 clone() const;
    std::string finalizeHex();
    static std::string hashHex(const uint8_t* data, std::size_t len);

private:
    void transform(const uint8_t block[64]);
    void padAndFinalize(std::array<uint8_t, 32>& out);

    std::array<uint32_t, 8> state_{};
    std::array<uint8_t, 64> buffer_{};
    std::uint64_t total_bytes_ = 0;
    std::size_t buffer_len_ = 0;
    bool finalized_ = false;
};

} // namespace mcls
