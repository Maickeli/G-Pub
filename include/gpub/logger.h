#pragma once

#include <mutex>
#include <string_view>

namespace gpub {

enum class LogLevel {
    Error = 0,
    Warn,
    Info,
    Debug
};

class Logger {
public:
    static Logger& instance();

    void setLevel(LogLevel level);
    LogLevel level() const;

    void log(LogLevel level, std::string_view message);
    void error(std::string_view message);
    void warn(std::string_view message);
    void info(std::string_view message);
    void debug(std::string_view message);

    static LogLevel parseLevel(std::string_view raw);
    static const char* toString(LogLevel level);

private:
    Logger() = default;

    LogLevel level_{LogLevel::Info};
    mutable std::mutex lock_;
};

} // namespace gpub

