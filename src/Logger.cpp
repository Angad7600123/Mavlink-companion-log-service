#include "mcls/Logger.hpp"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#ifdef MCLS_HAS_SYSTEMD
#include <spdlog/sinks/systemd_sink.h>
#endif

#include <vector>

namespace mcls {

Logger::Logger(const std::string& name, bool verbose, const std::string& file_path) {
    std::vector<spdlog::sink_ptr> sinks;

#ifdef MCLS_HAS_SYSTEMD
    sinks.push_back(std::make_shared<spdlog::sinks::systemd_sink_mt>(name));
#else
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
#endif

    if (!file_path.empty()) {
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(file_path, true));
    }

    logger_ = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
    logger_->set_level(verbose ? spdlog::level::debug : spdlog::level::info);
    logger_->flush_on(spdlog::level::info);
    spdlog::register_logger(logger_);
}

void Logger::info(const std::string& message) const {
    logger_->info(message);
}

void Logger::warn(const std::string& message) const {
    logger_->warn(message);
}

void Logger::error(const std::string& message) const {
    logger_->error(message);
}

void Logger::debug(const std::string& message) const {
    logger_->debug(message);
}

} // namespace mcls
