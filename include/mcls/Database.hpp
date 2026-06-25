#pragma once

#include "mcls/Types.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

struct sqlite3;

namespace mcls {

/// SQLite catalog of archived logs and persistent statistics.
class Database {
public:
    explicit Database(const std::filesystem::path& db_path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void initialize();
    std::optional<ArchivedLogCandidate> findCandidate(uint16_t fc_log_id, uint32_t fc_log_size) const;
    bool hasArchived(uint16_t fc_log_id, uint32_t fc_log_size) const;
    void insertArchivedLog(const std::string& sha256,
                           uint16_t fc_log_id,
                           uint32_t fc_log_size,
                           uint32_t fc_log_time_utc,
                           const std::string& probe_sha256,
                           int probe_bytes,
                           int64_t archive_time,
                           int64_t archive_duration_ms,
                           const std::string& filename);
    void markAllErased();
    int64_t incrementStat(const std::string& key, int64_t delta = 1);
    int64_t getStat(const std::string& key) const;
    void setStat(const std::string& key, int64_t value);

private:
    std::filesystem::path db_path_;
    sqlite3* db_ = nullptr;

    void exec(const char* sql) const;
};

} // namespace mcls
