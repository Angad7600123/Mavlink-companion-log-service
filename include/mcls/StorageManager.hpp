#pragma once

#include "mcls/Config.hpp"
#include "mcls/Types.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mcls {

class Logger;

/// Manages durable log file storage under /var/lib/mcls.
class StorageManager {
public:
    StorageManager(const Config::StorageSettings& settings, Logger& logger);

    void initialize();
    void cleanupPartials();
    std::filesystem::path beginPartialFile();
    struct FinalizeResult {
        std::filesystem::path final_path;
        int64_t archive_duration_ms = 0;
    };
    std::optional<FinalizeResult> finalizePartial(
        const std::filesystem::path& partial_path,
        std::chrono::steady_clock::time_point download_start);
    void enforceStorageLimit();
    std::uintmax_t totalVerifiedBytes() const;

    const std::filesystem::path& root() const { return root_; }
    const std::filesystem::path& logsDir() const { return logs_dir_; }
    const std::filesystem::path& tmpDir() const { return tmp_dir_; }

private:
    Config::StorageSettings settings_;
    Logger& logger_;
    std::filesystem::path root_;
    std::filesystem::path logs_dir_;
    std::filesystem::path tmp_dir_;
    int partial_counter_ = 0;

    static std::string timestampFilename();
    static bool fsyncFile(const std::filesystem::path& path);
    static bool fsyncParentDir(const std::filesystem::path& path);
};

} // namespace mcls
