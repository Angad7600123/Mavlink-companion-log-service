#pragma once

#include <memory>
#include <string>

namespace spdlog {
class logger;
}

namespace mcls {

/// Thin wrapper around spdlog with optional journal and file sinks.
class Logger {
public:
    explicit Logger(const std::string& name, bool verbose, const std::string& file_path = {});

    void info(const std::string& message) const;
    void warn(const std::string& message) const;
    void error(const std::string& message) const;
    void debug(const std::string& message) const;

    std::shared_ptr<spdlog::logger> underlying() const { return logger_; }

private:
    std::shared_ptr<spdlog::logger> logger_;
};

} // namespace mcls
