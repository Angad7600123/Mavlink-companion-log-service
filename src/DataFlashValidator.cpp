#include "mcls/DataFlashValidator.hpp"

#include <array>
#include <cstdio>
#include <fstream>
#include <vector>

namespace mcls {

namespace {

constexpr uint8_t kHeadByte1 = 0xA3;
constexpr uint8_t kHeadByte2 = 0x95;
constexpr uint8_t kLogFormatType = 0x80;
constexpr int kMaxFormatTypes = 256;

struct FormatEntry {
    uint8_t length = 0;
    bool defined = false;
};

DataFlashValidationResult validateBytes(const uint8_t* data, const std::size_t size) {
    DataFlashValidationResult result;
    if (data == nullptr || size == 0) {
        result.error = "empty buffer";
        return result;
    }

    std::array<FormatEntry, kMaxFormatTypes> formats{};
    std::size_t pos = 0;
    bool have_fmt = false;

    while (pos + 3 <= size) {
        if (data[pos] != kHeadByte1 || data[pos + 1] != kHeadByte2) {
            ++pos;
            ++result.resync_count;
            continue;
        }

        const uint8_t msg_type = data[pos + 2];
        if (msg_type == kLogFormatType) {
            // ArduPilot log_Format: header(3) + type(1) + length(1) + name(4) + format(16) + labels(64)
            constexpr std::size_t kFmtSize = 3 + 1 + 1 + 4 + 16 + 64;
            if (pos + kFmtSize > size) {
                result.error = "truncated FMT message";
                result.bytes_scanned = pos;
                return result;
            }
            const uint8_t type_id = data[pos + 3];
            if (type_id < kMaxFormatTypes) {
                formats[type_id].length = data[pos + 4];
                formats[type_id].defined = true;
                have_fmt = true;
            }
            pos += kFmtSize;
            result.bytes_scanned = pos;
            continue;
        }

        if (!have_fmt) {
            ++pos;
            ++result.resync_count;
            continue;
        }

        if (msg_type >= kMaxFormatTypes || !formats[msg_type].defined) {
            ++pos;
            ++result.bad_message_count;
            ++result.resync_count;
            continue;
        }

        const uint8_t msg_len = formats[msg_type].length;
        if (msg_len < 3) {
            ++pos;
            ++result.bad_message_count;
            continue;
        }

        if (pos + msg_len > size) {
            result.error = "message extends past end of file";
            result.bytes_scanned = pos;
            return result;
        }

        pos += msg_len;
        result.bytes_scanned = pos;
    }

    if (!have_fmt) {
        result.error = "no FMT bootstrap found";
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace

bool dataFlashResyncRatioExceeded(const DataFlashValidationResult& result,
                                  const uint32_t file_size,
                                  const double max_ratio) {
    if (file_size == 0) {
        return false;
    }
    return static_cast<double>(result.resync_count) / static_cast<double>(file_size) > max_ratio;
}

DataFlashValidationResult validateDataFlashBuffer(const uint8_t* data, const std::size_t size) {
    return validateBytes(data, size);
}

DataFlashValidationResult validateDataFlashFile(const std::filesystem::path& path) {
    DataFlashValidationResult result;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.error = "failed to open file";
        return result;
    }

    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    if (end < 0) {
        result.error = "failed to stat file";
        return result;
    }

    const std::size_t size = static_cast<std::size_t>(end);
    in.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    if (size > 0 && !in.read(reinterpret_cast<char*>(buffer.data()),
                             static_cast<std::streamsize>(size))) {
        result.error = "failed to read file";
        return result;
    }

    return validateBytes(buffer.data(), size);
}

} // namespace mcls
