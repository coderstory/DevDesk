#include "logging.h"
#include "time.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace core::log {

namespace {
    Level g_level = Level::Info;
    bool g_console = false;
    std::ofstream g_file;
    std::mutex g_mutex;

    std::string timestamp() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count() % 1000;
        std::tm tm;
        core::time::localTime(time, &tm);
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3) << ms;
        return oss.str();
    }
}

void setLevel(Level level) { g_level = level; }
Level level() { return g_level; }

void addFileOutput(const std::filesystem::path& file) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file.is_open()) g_file.close();
    g_file.open(file, std::ios::app);
}

void addConsoleOutput() {
    g_console = true;
}

void logMessage(Level level, const std::string& message, const char* file, int line) {
    if (static_cast<int>(level) < static_cast<int>(g_level)) return;

    std::lock_guard<std::mutex> lock(g_mutex);

    std::ostringstream oss;
    oss << "[" << timestamp() << "] [" << levelToString(level) << "] "
        << message;
    if (line > 0 && file && *file) {
        oss << " (" << file << ":" << line << ")";
    }
    oss << "\n";

    std::string line_out = oss.str();

    if (g_console) {
        std::cout << line_out;
        std::cout.flush();
    }
    if (g_file.is_open()) {
        g_file << line_out;
        g_file.flush();
    }
}

} // namespace core::log