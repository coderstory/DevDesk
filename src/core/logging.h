#pragma once

#include <filesystem>
#include <mutex>
#include <string>

namespace core::log {

enum class Level { Trace, Debug, Info, Warn, Error, Fatal };

void setLevel(Level level);
Level level();

void addFileOutput(const std::filesystem::path& file);

void addConsoleOutput();

void logMessage(Level level, const std::string& message,
                const char* file = "", int line = 0);

inline const char* levelToString(Level l) {
    switch (l) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO ";
        case Level::Warn:  return "WARN ";
        case Level::Error: return "ERROR";
        case Level::Fatal: return "FATAL";
    }
    return "?";
}

} // namespace core::log

#define LOG_TRACE(msg) ::core::log::logMessage(::core::log::Level::Trace, msg, __FILE__, __LINE__)
#define LOG_DEBUG(msg) ::core::log::logMessage(::core::log::Level::Debug, msg, __FILE__, __LINE__)
#define LOG_INFO(msg)  ::core::log::logMessage(::core::log::Level::Info,  msg, __FILE__, __LINE__)
#define LOG_WARN(msg)  ::core::log::logMessage(::core::log::Level::Warn,  msg, __FILE__, __LINE__)
#define LOG_ERROR(msg) ::core::log::logMessage(::core::log::Level::Error, msg, __FILE__, __LINE__)
#define LOG_FATAL(msg) ::core::log::logMessage(::core::log::Level::Fatal, msg, __FILE__, __LINE__)