#include "mcls/Sha256.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace mcls {

namespace {

constexpr std::array<uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

inline uint32_t sig0(uint32_t x) {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

inline uint32_t sig1(uint32_t x) {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

inline uint32_t ep0(uint32_t x) {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

inline uint32_t ep1(uint32_t x) {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

std::string bytesToHex(const std::array<uint8_t, 32>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t b : bytes) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

} // namespace

Sha256::Sha256() {
    state_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
}

void Sha256::update(const uint8_t* data, std::size_t len) {
    if (finalized_) {
        throw std::runtime_error("Sha256 context already finalized");
    }
    for (std::size_t i = 0; i < len; ++i) {
        buffer_[buffer_len_++] = data[i];
        ++total_bytes_;
        if (buffer_len_ == 64) {
            transform(buffer_.data());
            buffer_len_ = 0;
        }
    }
}

void Sha256::update(const std::vector<uint8_t>& data) {
    update(data.data(), data.size());
}

Sha256 Sha256::clone() const {
    Sha256 copy;
    copy.state_ = state_;
    copy.buffer_ = buffer_;
    copy.total_bytes_ = total_bytes_;
    copy.buffer_len_ = buffer_len_;
    copy.finalized_ = finalized_;
    return copy;
}

void Sha256::transform(const uint8_t block[64]) {
    std::array<uint32_t, 64> w{};
    for (int i = 0; i < 16; ++i) {
        w[i] = (block[i * 4] << 24) | (block[i * 4 + 1] << 16) | (block[i * 4 + 2] << 8) | block[i * 4 + 3];
    }
    for (int i = 16; i < 64; ++i) {
        w[i] = ep1(w[i - 2]) + w[i - 7] + ep0(w[i - 15]) + w[i - 16];
    }

    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];
    uint32_t f = state_[5];
    uint32_t g = state_[6];
    uint32_t h = state_[7];

    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + sig1(e) + ch(e, f, g) + kRoundConstants[i] + w[i];
        const uint32_t t2 = sig0(a) + maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::padAndFinalize(std::array<uint8_t, 32>& out) {
    const uint64_t bit_len = total_bytes_ * 8;
    uint8_t pad_byte = 0x80;
    update(&pad_byte, 1);

    while (buffer_len_ != 56) {
        pad_byte = 0;
        update(&pad_byte, 1);
    }

    for (int i = 7; i >= 0; --i) {
        const uint8_t len_byte = static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFF);
        update(&len_byte, 1);
    }

    for (int i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<uint8_t>((state_[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<uint8_t>((state_[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<uint8_t>((state_[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<uint8_t>(state_[i] & 0xFF);
    }
}

std::string Sha256::finalizeHex() {
    if (finalized_) {
        throw std::runtime_error("Sha256 context already finalized");
    }
    std::array<uint8_t, 32> digest{};
    padAndFinalize(digest);
    finalized_ = true;
    return bytesToHex(digest);
}

std::string Sha256::hashHex(const uint8_t* data, std::size_t len) {
    Sha256 ctx;
    ctx.update(data, len);
    return ctx.finalizeHex();
}

} // namespace mcls
