#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace mcls {

struct DataFlashValidationResult {
    bool ok = false;
    uint64_t bytes_scanned = 0;
    uint32_t resync_count = 0;
    uint32_t bad_message_count = 0;
    std::string error;
};

/// Validate a DataFlash binary file on disk. No MAVLink or service dependencies.
DataFlashValidationResult validateDataFlashFile(const std::filesystem::path& path);

/// Validate from a memory buffer (unit tests).
DataFlashValidationResult validateDataFlashBuffer(const uint8_t* data, std::size_t size);

bool dataFlashResyncRatioExceeded(const DataFlashValidationResult& result,
                                  uint32_t file_size,
                                  double max_ratio);

} // namespace mcls
