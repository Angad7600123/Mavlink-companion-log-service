#include "mcls/Database.hpp"

#include <sqlite3.h>

#include <chrono>
#include <stdexcept>

namespace mcls {

namespace {

void checkSqlite(int rc, sqlite3* db, const char* context) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        const char* err = db ? sqlite3_errmsg(db) : "unknown";
        throw std::runtime_error(std::string(context) + ": " + err);
    }
}

} // namespace

Database::Database(const std::filesystem::path& db_path) : db_path_(db_path) {}

Database::~Database() {
    if (db_) {
        sqlite3_close(db_);
    }
}

void Database::initialize() {
    if (db_path_.has_parent_path()) {
        std::filesystem::create_directories(db_path_.parent_path());
    }

    checkSqlite(sqlite3_open(db_path_.string().c_str(), &db_), db_, "sqlite3_open");
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA synchronous=FULL;");

    exec(R"(
        CREATE TABLE IF NOT EXISTS archived_logs (
            sha256 TEXT PRIMARY KEY,
            fc_log_id INTEGER NOT NULL,
            fc_log_size INTEGER NOT NULL,
            fc_log_time_utc INTEGER,
            probe_sha256 TEXT NOT NULL,
            probe_bytes INTEGER NOT NULL,
            archive_time INTEGER NOT NULL,
            archive_duration_ms INTEGER,
            filename TEXT NOT NULL,
            erased INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_fc_lookup ON archived_logs(fc_log_id, fc_log_size);
        CREATE TABLE IF NOT EXISTS statistics (
            key TEXT PRIMARY KEY,
            value INTEGER NOT NULL
        );
    )");
}

void Database::exec(const char* sql) const {
    char* err = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string message = err ? err : "sqlite exec failed";
        sqlite3_free(err);
        throw std::runtime_error(message);
    }
}

std::optional<ArchivedLogCandidate> Database::findCandidate(uint16_t fc_log_id,
                                                          uint32_t fc_log_size) const {
    const char* sql =
        "SELECT sha256, probe_sha256, probe_bytes, filename FROM archived_logs "
        "WHERE fc_log_id = ? AND fc_log_size = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    checkSqlite(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), db_, "prepare findCandidate");

    sqlite3_bind_int(stmt, 1, fc_log_id);
    sqlite3_bind_int64(stmt, 2, fc_log_size);

    std::optional<ArchivedLogCandidate> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ArchivedLogCandidate candidate;
        candidate.sha256 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        candidate.probe_sha256 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        candidate.probe_bytes = sqlite3_column_int(stmt, 2);
        candidate.filename = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        result = candidate;
    }
    sqlite3_finalize(stmt);
    return result;
}

bool Database::hasArchived(uint16_t fc_log_id, uint32_t fc_log_size) const {
    return findCandidate(fc_log_id, fc_log_size).has_value();
}

void Database::insertArchivedLog(const std::string& sha256,
                                 uint16_t fc_log_id,
                                 uint32_t fc_log_size,
                                 uint32_t fc_log_time_utc,
                                 const std::string& probe_sha256,
                                 int probe_bytes,
                                 int64_t archive_time,
                                 int64_t archive_duration_ms,
                                 const std::string& filename) {
    const char* sql =
        "INSERT OR REPLACE INTO archived_logs "
        "(sha256, fc_log_id, fc_log_size, fc_log_time_utc, probe_sha256, probe_bytes, "
        "archive_time, archive_duration_ms, filename, erased) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 0);";

    sqlite3_stmt* stmt = nullptr;
    checkSqlite(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), db_, "prepare insert");

    sqlite3_bind_text(stmt, 1, sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, fc_log_id);
    sqlite3_bind_int64(stmt, 3, fc_log_size);
    sqlite3_bind_int64(stmt, 4, fc_log_time_utc);
    sqlite3_bind_text(stmt, 5, probe_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, probe_bytes);
    sqlite3_bind_int64(stmt, 7, archive_time);
    sqlite3_bind_int64(stmt, 8, archive_duration_ms);
    sqlite3_bind_text(stmt, 9, filename.c_str(), -1, SQLITE_TRANSIENT);

    checkSqlite(sqlite3_step(stmt), db_, "insert archived log");
    sqlite3_finalize(stmt);
}

void Database::markAllErased() {
    exec("UPDATE archived_logs SET erased = 1;");
}

int64_t Database::incrementStat(const std::string& key, int64_t delta) {
    const int64_t current = getStat(key);
    const int64_t next = current + delta;
    setStat(key, next);
    return next;
}

int64_t Database::getStat(const std::string& key) const {
    const char* sql = "SELECT value FROM statistics WHERE key = ?;";
    sqlite3_stmt* stmt = nullptr;
    checkSqlite(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), db_, "prepare getStat");
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);

    int64_t value = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return value;
}

void Database::setStat(const std::string& key, int64_t value) {
    const char* sql = "INSERT OR REPLACE INTO statistics (key, value) VALUES (?, ?);";
    sqlite3_stmt* stmt = nullptr;
    checkSqlite(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), db_, "prepare setStat");
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, value);
    checkSqlite(sqlite3_step(stmt), db_, "setStat");
    sqlite3_finalize(stmt);
}

} // namespace mcls
