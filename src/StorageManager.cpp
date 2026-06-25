#include "mcls/StorageManager.hpp"
#include "mcls/Logger.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace mcls {

StorageManager::StorageManager(const Config::StorageSettings& settings, Logger& logger)
    : settings_(settings), logger_(logger) {
    root_ = settings_.directory;
    logs_dir_ = root_ / "logs";
    tmp_dir_ = root_ / "tmp";
}

void StorageManager::initialize() {
    std::filesystem::create_directories(logs_dir_);
    std::filesystem::create_directories(tmp_dir_);
    cleanupPartials();
    logger_.info("Storage initialized at " + root_.string());
}

void StorageManager::cleanupPartials() {
    if (!std::filesystem::exists(tmp_dir_)) {
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(tmp_dir_)) {
        if (entry.path().extension() == ".partial") {
            logger_.warn("Removing leftover partial file: " + entry.path().string());
            std::filesystem::remove(entry.path());
        }
    }
}

std::filesystem::path StorageManager::beginPartialFile() {
    const auto now = std::chrono::system_clock::now();
    const auto day = std::chrono::floor<std::chrono::days>(now);
    const std::time_t day_time = std::chrono::system_clock::to_time_t(day);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &day_time);
#else
    localtime_r(&day_time, &tm);
#endif
    std::ostringstream day_dir;
    day_dir << std::put_time(&tm, "%Y-%m-%d");

    const auto partial_name =
        timestampFilename() + "_part" + std::to_string(++partial_counter_) + ".bin.partial";
    return tmp_dir_ / partial_name;
}

std::optional<StorageManager::FinalizeResult> StorageManager::finalizePartial(
    const std::filesystem::path& partial_path,
    std::chrono::steady_clock::time_point download_start) {
    if (!std::filesystem::exists(partial_path)) {
        logger_.error("Partial file missing: " + partial_path.string());
        return std::nullopt;
    }

    const auto now = std::chrono::system_clock::now();
    const auto day = std::chrono::floor<std::chrono::days>(now);
    const std::time_t day_time = std::chrono::system_clock::to_time_t(day);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &day_time);
#else
    localtime_r(&day_time, &tm);
#endif
    std::ostringstream day_dir;
    day_dir << std::put_time(&tm, "%Y-%m-%d");

    const auto dest_dir = logs_dir_ / day_dir.str();
    std::filesystem::create_directories(dest_dir);

    const auto final_name = timestampFilename() + ".bin";
    const auto final_path = dest_dir / final_name;

    {
        std::ofstream flush_check(partial_path, std::ios::binary | std::ios::app);
        flush_check.flush();
    }

    if (!fsyncFile(partial_path)) {
        logger_.error("Failed to fsync partial file: " + partial_path.string());
        return std::nullopt;
    }

    std::error_code ec;
    std::filesystem::rename(partial_path, final_path, ec);
    if (ec) {
        logger_.error("Failed to rename partial to final: " + ec.message());
        return std::nullopt;
    }

    if (!fsyncParentDir(final_path)) {
        logger_.warn("Failed to fsync parent directory for: " + final_path.string());
    }

    if (!std::filesystem::exists(final_path)) {
        logger_.error("Final archive file missing after rename");
        return std::nullopt;
    }

    FinalizeResult result;
    result.final_path = final_path;
    result.archive_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - download_start)
                                     .count();
    return result;
}

void StorageManager::enforceStorageLimit() {
    const std::uintmax_t max_bytes =
        static_cast<std::uintmax_t>(settings_.max_size_gb) * 1024ULL * 1024ULL * 1024ULL;

    struct FileEntry {
        std::filesystem::path path;
        std::filesystem::file_time_type mtime;
        std::uintmax_t size;
    };

    std::vector<FileEntry> verified;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(logs_dir_)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != ".bin") {
            continue;
        }
        verified.push_back({entry.path(), entry.last_write_time(), entry.file_size()});
    }

    std::uintmax_t total = 0;
    for (const auto& f : verified) {
        total += f.size;
    }

    if (total <= max_bytes) {
        return;
    }

    std::sort(verified.begin(), verified.end(),
              [](const FileEntry& a, const FileEntry& b) { return a.mtime < b.mtime; });

    for (const auto& f : verified) {
        if (total <= max_bytes) {
            break;
        }
        logger_.info("Deleting oldest verified archive: " + f.path.string());
        total -= f.size;
        std::filesystem::remove(f.path);
    }
}

std::uintmax_t StorageManager::totalVerifiedBytes() const {
    std::uintmax_t total = 0;
    if (!std::filesystem::exists(logs_dir_)) {
        return 0;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(logs_dir_)) {
        if (entry.is_regular_file() && entry.path().extension() == ".bin") {
            total += entry.file_size();
        }
    }
    return total;
}

std::string StorageManager::timestampFilename() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S");
    return oss.str();
}

bool StorageManager::fsyncFile(const std::filesystem::path& path) {
#ifdef _WIN32
    int fd = _open(path.string().c_str(), _O_RDONLY);
    if (fd < 0) {
        return false;
    }
    const int rc = _commit(fd);
    _close(fd);
    return rc == 0;
#else
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }
    const int rc = fsync(fd);
    close(fd);
    return rc == 0;
#endif
}

bool StorageManager::fsyncParentDir(const std::filesystem::path& path) {
    const auto parent = path.parent_path();
#ifdef _WIN32
    int fd = _open(parent.string().c_str(), _O_RDONLY);
    if (fd < 0) {
        return false;
    }
    const int rc = _commit(fd);
    _close(fd);
    return rc == 0;
#else
    int fd = open(parent.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }
    const int rc = fsync(fd);
    close(fd);
    return rc == 0;
#endif
}

} // namespace mcls
