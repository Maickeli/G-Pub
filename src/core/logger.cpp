#include "gpub/logger.h"
#include "gpub/text_util.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace gpub {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::setLevel(LogLevel level) {
    std::lock_guard<std::mutex> guard(lock_);
    level_ = level;
}

LogLevel Logger::level() const {
    std::lock_guard<std::mutex> guard(lock_);
    return level_;
}

void Logger::log(LogLevel level, std::string_view message) {
    std::lock_guard<std::mutex> guard(lock_);
    if (static_cast<int>(level) > static_cast<int>(level_)) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const auto now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &now_time);
#else
    localtime_r(&now_time, &local_tm);
#endif

    std::ostringstream stamp;
    stamp << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S");

    std::cerr << "[" << stamp.str() << "] [" << toString(level) << "] " << message << '\n';
}

void Logger::error(std::string_view message) {
    log(LogLevel::Error, message);
}

void Logger::warn(std::string_view message) {
    log(LogLevel::Warn, message);
}

void Logger::info(std::string_view message) {
    log(LogLevel::Info, message);
}

void Logger::debug(std::string_view message) {
    log(LogLevel::Debug, message);
}

LogLevel Logger::parseLevel(std::string_view raw) {
    const std::string lowered = toLowerAscii(std::string(raw));
    if (lowered == "error") {
        return LogLevel::Error;
    }
    if (lowered == "warn" || lowered == "warning") {
        return LogLevel::Warn;
    }
    if (lowered == "debug") {
        return LogLevel::Debug;
    }
    return LogLevel::Info;
}

const char* Logger::toString(LogLevel level) {
    switch (level) {
    case LogLevel::Error:
        return "ERROR";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Debug:
        return "DEBUG";
    }
    return "INFO";
}

} // namespace gpub

