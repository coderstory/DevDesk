#include "logging.h"
#include "time.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <system_error>

namespace core::log {

namespace {
    // 审计 L7：级别/控制台开关由设置页（UI 线程）写、代理 worker 线程读
    // （worker 在读 g_level 时还没取 g_mutex），原来用普通全局变量属数据竞争。
    // 改 atomic 只为消竞争，行为不变。
    std::atomic<Level> g_level{Level::Info};
    std::atomic<bool> g_console{false};
    std::ofstream g_file;
    std::mutex g_mutex;

    // 审计 M7：文本日志按大小轮转。原来 devdesk.log 以 app 追加打开、永不
    // 轮转（设置页可把级别切到 DEBUG），体积无限增长。这里在启动打开日志时
    // 滚动：超过阈值把当前文件改名 .1，旧的依次后移，最多保留 2 份。
    constexpr std::uintmax_t kRotateThresholdBytes = 8 * 1024 * 1024; // 8MB
    constexpr int kMaxRotatedFiles = 2;                               // .1 / .2

    std::string rotatedName(const std::filesystem::path& file, int index) {
        return file.string() + "." + std::to_string(index);
    }

    // 全用 error_code 版本：本仓库不抛异常，轮转失败静默跳过——日志轮转
    // 失败不应影响日志功能本身
    void rotateIfTooLarge(const std::filesystem::path& file) {
        std::error_code ec;
        auto size = std::filesystem::file_size(file, ec);
        if (ec || size <= kRotateThresholdBytes) return;

        std::filesystem::remove(rotatedName(file, kMaxRotatedFiles), ec); // 最老一份丢弃
        for (int i = kMaxRotatedFiles - 1; i >= 1; --i) {
            std::filesystem::rename(rotatedName(file, i), rotatedName(file, i + 1), ec);
        }
        std::filesystem::rename(file, rotatedName(file, 1), ec);
    }

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

void setLevel(Level level) { g_level.store(level); }
Level level() { return g_level.load(); }

void addFileOutput(const std::filesystem::path& file) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file.is_open()) g_file.close();
    rotateIfTooLarge(file); // 先关掉旧句柄再滚动（Windows 不允许改打开中的文件名）
    g_file.open(file, std::ios::app);
}

void addConsoleOutput() {
    g_console.store(true);
}

void logMessage(Level level, const std::string& message, const char* file, int line) {
    if (static_cast<int>(level) < static_cast<int>(g_level.load())) return;

    std::lock_guard<std::mutex> lock(g_mutex);

    std::ostringstream oss;
    oss << "[" << timestamp() << "] [" << levelToString(level) << "] "
        << message;
    if (line > 0 && file && *file) {
        oss << " (" << file << ":" << line << ")";
    }
    oss << "\n";

    std::string line_out = oss.str();

    if (g_console.load()) {
        std::cout << line_out;
        std::cout.flush();
    }
    if (g_file.is_open()) {
        g_file << line_out;
        g_file.flush();
    }
}

} // namespace core::log