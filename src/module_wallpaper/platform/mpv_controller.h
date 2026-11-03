// src/module_wallpaper/platform/mpv_controller.h
// mpv 进程 + 命名管道 JSON IPC 控制器（接口与纯逻辑在头文件，跨平台单测可达）。
//
// 线程模型：
//   - 唯一控制线程拥有进程/管道/Job 句柄；命令经队列投递，调用点不阻塞（stop 除外）。
//   - start() 异步派发 spawn + 管道连接；失败写内部错误槽，经 takeError() 取出。
//   - stop() 同步完成 quit → kill → wait，返回时进程已退出（有界，幂等）。
//   - time-pos 由读线程写原子毫秒值；positionSec() 任意线程可读。
//
// 路径解析（需求 §6.2）：
//   DEVDESK_MPV_PATH → <exeDir>/assets/mpv/mpv.exe → 开发兜底 D:/software/mpv/mpv.exe
//   开发兜底只在非 Release 构建启用，且日志明确标注；找不到返回空串 + 中文原因。
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "module_wallpaper/core/model.h"

namespace module::wallpaper {

inline constexpr const char* kMpvPathEnvVar = "DEVDESK_MPV_PATH";
inline constexpr const char* kMpvDevFallbackPath = "D:/software/mpv/mpv.exe";
inline constexpr const char* kMpvPipeNamePrefix = "\\\\.\\pipe\\devdesk-wallpaper-mpv-";

// mpv 命名管道名：\\.\pipe\devdesk-wallpaper-mpv-<pid>-<counter>
inline std::string makeMpvPipeName(std::uint32_t processId, std::uint64_t counter) {
    return std::string(kMpvPipeNamePrefix) + std::to_string(processId) + "-" +
           std::to_string(counter);
}

// 解析可执行文件路径；返回空串 = 未找到（原因由调用方组织中文文案）。
// devFallbackPath 显式传入（生产传 kMpvDevFallbackPath），保证纯函数可测。
inline std::string resolveMpvExecutable(const std::string& envValue,
                                        const std::string& executableDir,
                                        const std::string& devFallbackPath,
                                        bool allowDevFallback,
                                        bool* usedDevFallback) {
    if (usedDevFallback != nullptr) *usedDevFallback = false;
    std::error_code error;

    if (!envValue.empty()) {
        const std::filesystem::path candidate(envValue);
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return envValue;
        }
    }
    if (!executableDir.empty()) {
        const std::filesystem::path candidate =
            std::filesystem::path(executableDir) / "assets" / "mpv" / "mpv.exe";
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return candidate.string();
        }
    }
    if (allowDevFallback && !devFallbackPath.empty()) {
        const std::filesystem::path candidate(devFallbackPath);
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            if (usedDevFallback != nullptr) *usedDevFallback = true;
            return devFallbackPath;
        }
    }
    return std::string();
}

// §6.2 的 argv（不含 argv[0]）。wid 用十进制。
inline std::vector<std::string> buildMpvArguments(std::uintptr_t windowId,
                                                  const std::string& pipeName) {
    return {
        "--no-config",
        "--idle=yes",
        "--wid=" + std::to_string(windowId),
        "--input-ipc-server=" + pipeName,
        "--loop-file=inf",
        // 静态图片常驻：mpv 默认 image-display-duration=1s 会自行切走；壁纸换片只由轮换驱动。
        "--image-display-duration=inf",
        "--hwdec=auto-safe",
        // ── 内存/缓存收紧（壁纸场景专用）──
        // 壁纸只循环播本地文件：不需要网络式缓冲，也不需要长途 seek 的读前量。
        // mpv 默认为通用播放器调参（demuxer 读前缓冲按秒数×码率估，高码率 4K 可达
        // 数百 MB），对「一直播着不换片」的壁纸是纯浪费。
        //   demuxer-max-bytes：前向缓冲上限，32MiB 足够 4K 高码率数秒预读；
        //   demuxer-max-back-bytes：回退缓冲，循环播放几乎不用 seek，压到 8MiB；
        //   demuxer-readahead-secs：本地文件 5s 预读充足（默认 20s 对本地盘无意义）。
        // 刻意**不**加 --cache=no：它管的是流式缓存，对本地文件无意义；而且不同 mpv
        // 版本对该选项的语义有变动，未知/弃用选项会让 mpv 启动即失败、整个壁纸不可用
        //（本仓无法在 macOS 上验证 Windows 侧 mpv 版本行为）。上面三个 demuxer 选项
        // 自 mpv 0.29 起稳定存在。
        // 注：唯一效果是降低 mpv 常驻内存；不改变播放行为（循环/静音/铺满/暂停语义不变）。
        "--demuxer-max-bytes=32MiB",
        "--demuxer-max-back-bytes=8MiB",
        "--demuxer-readahead-secs=5",
        "--no-osc",
        "--no-input-default-bindings",
        "--no-osd-bar",
        "--osd-level=0",
        "--force-window=yes",
        "--vo=gpu",
        "--panscan=1.0",
        "--audio-pitch-correction=yes",
        "--no-terminal",
        "--keep-open=no",
        "--pause=yes",
        "--sub-auto=no",
    };
}

// JSON 字符串转义（mpv IPC 一行一条 JSON；Windows 路径的反斜杠必须转义）。
inline std::string mpvJsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char raw : value) {
        const unsigned char byte = static_cast<unsigned char>(raw);
        switch (byte) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (byte < 0x20) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", byte);
                    out += escaped;
                } else {
                    out += raw;
                }
                break;
        }
    }
    return out;
}

inline std::string mpvNumber(double value) {
    if (!std::isfinite(value)) return "0";
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    return std::string(buffer);
}

inline std::string mpvLoadFileCommand(const std::string& path) {
    return "{\"command\":[\"loadfile\",\"" + mpvJsonEscape(path) + "\",\"replace\"]}";
}

inline std::string mpvSetPropertyCommand(const std::string& name, bool value) {
    return "{\"command\":[\"set_property\",\"" + mpvJsonEscape(name) + "\"," +
           (value ? "true" : "false") + "]}";
}

inline std::string mpvSetPropertyCommand(const std::string& name, double value) {
    return "{\"command\":[\"set_property\",\"" + mpvJsonEscape(name) + "\"," + mpvNumber(value) +
           "]}";
}

inline std::string mpvSeekCommand(double seconds) {
    return "{\"command\":[\"seek\"," + mpvNumber(seconds) + ",\"absolute+exact\"]}";
}

inline std::string mpvObservePropertyCommand(int id, const std::string& name) {
    return "{\"command\":[\"observe_property\"," + std::to_string(id) + ",\"" +
           mpvJsonEscape(name) + "\"]}";
}

inline std::string mpvQuitCommand() { return "{\"command\":[\"quit\"]}"; }

// 解析 mpv 的 time-pos property-change 事件；data 为 null/非数字时返回 false。
// 容忍空格：先去掉空白再做紧凑子串匹配，数字交给 strtod。
inline bool parseMpvTimePosEvent(const std::string& line, double* seconds) {
    if (line.find("time-pos") == std::string::npos) return false;
    std::string compact;
    compact.reserve(line.size());
    for (const char raw : line) {
        if (raw == ' ' || raw == '\t' || raw == '\r' || raw == '\n') continue;
        compact += raw;
    }
    if (compact.find("\"event\":\"property-change\"") == std::string::npos) return false;
    if (compact.find("\"name\":\"time-pos\"") == std::string::npos) return false;
    const std::size_t dataAt = compact.find("\"data\":");
    if (dataAt == std::string::npos) return false;
    const char* begin = compact.c_str() + dataAt + 7;
    char* end = nullptr;
    const double value = std::strtod(begin, &end);
    if (end == begin || !std::isfinite(value)) return false;
    if (seconds != nullptr) *seconds = value;
    return true;
}

// Windows CreateProcess 命令行参数转义（MS 规则：反斜杠只在引号前翻倍）。
inline std::string quoteWindowsCommandLineArg(const std::string& argument) {
    if (argument.empty()) return "\"\"";
    if (argument.find_first_of(" \t\n\v\"") == std::string::npos) return argument;
    std::string out;
    out.push_back('"');
    std::size_t backslashes = 0;
    for (const char byte : argument) {
        if (byte == '\\') {
            ++backslashes;
            continue;
        }
        if (byte == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, '\\');
        backslashes = 0;
        out.push_back(byte);
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

inline std::string buildWindowsCommandLine(const std::string& executable,
                                           const std::vector<std::string>& arguments) {
    std::string line = quoteWindowsCommandLineArg(executable);
    for (const std::string& argument : arguments) {
        line.push_back(' ');
        line += quoteWindowsCommandLineArg(argument);
    }
    return line;
}

class MpvController {
public:
    MpvController();
    ~MpvController();
    MpvController(const MpvController&) = delete;
    MpvController& operator=(const MpvController&) = delete;

    // 异步：spawn + 管道连接 + 属性观察都在控制线程执行。失败经 takeError() 取中文原因。
    void start(std::uintptr_t windowId);
    // 同步：quit + kill + wait，返回时进程已退出；幂等。
    void stop();
    // 以下命令异步投递；未运行（未 start / 已 stop）时被忽略。
    void load(const std::string& path, float seekSeconds = -1.0f);
    void pause(bool paused);
    void seek(float seconds);
    void setRate(float rate);
    void setVolume(float volume);  // 0..1，内部换算为 mpv 的 0..100
    void setMute(bool muted);
    // 图片铺法。mpv 路线只实现「填充」（--panscan=1.0，其余三种铺法是 deferred
    // 决策，调用即 no-op）；macOS 原生路线四种铺法全部自己画。
    void setImageFit(ImageFit fit);

    float positionSec() const;
    bool processAlive() const;
    std::uint32_t processId() const;
    // 取走最近一次错误（空串 = 无错误）。
    std::string takeError();

private:
#if defined(_WIN32)
    friend struct MpvControllerTestAccess;  // 见文件尾注入缝契约
#endif
    struct Impl;
    // shared 所有权：异步块按值捕 impl，块在飞时 Impl 不会被析构（2026-11-04
    // 审计 F6：图片后台解码晚到 / 析构竞态的根治）。macOS 实现专用；Windows 的
    // Impl 生命期由 mpv 进程 IPC 管线自管，仍可继续用 unique_ptr 语义（本成员
    // 两平台同一声明，shared_ptr 对 Windows 无额外成本）。
    std::shared_ptr<Impl> impl_;
};

#if defined(_WIN32)
// 仅测试注入缝（tests/test_wallpaper_platform.cpp 的 IPC 死锁回归网用，生产代码
// 不得调用）：把测试自建的管道客户端句柄与伪进程句柄（未复位的事件即可——
// TerminateProcess 对其失败被忽略、wait 永不信号，使 stop 梯子走满有界路径而
// 不引入真子进程）注入控制器，模拟「spawn 成功 + 对端连上后拒绝读」。
// 注入即所有权移交：句柄由控制器 shutdown 路径统一关闭。
struct MpvControllerTestAccess {
    static void attachIpcForTest(MpvController& controller, void* pipeClient,
                                 void* fakeProcess);
};
#endif

} // namespace module::wallpaper