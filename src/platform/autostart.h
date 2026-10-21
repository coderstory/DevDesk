// src/platform/autostart.h
// 开机自启动平台抽象（UAT6 设置-通用「开机自启动」开关）：头文件平台无关——
// 注入缝接口 + 纯逻辑全部 inline 在头里，实现按 *_win/_macos/_posix 拆（与
// tray_icon / file_open 同一模式，业务代码零平台宏）。
// 注册内容统一带 --minimized 旗标：开机拉起后 main 侧扫 argv 命中即主窗保持
// 托盘态（捕获见 app_assembly::captureMinimizedFlagFromArgv）。
// 系统触达（注册表 / LaunchAgents / 进程命令行 / exe 路径解析）收在各平台实现；
// 单测经 AutostartOps 构造注入假 writer 断言写入内容与删除调用，headless 不碰系统。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/result.h"

namespace platform::autostart {

// ── 注入缝：注册 / 查询 / 删除三动作抽象 ──
// 生产实现：Windows 写 HKCU Software\Microsoft\Windows\CurrentVersion\Run 的
// "DevDesk" 值；macOS 写 ~/Library/LaunchAgents/com.coderstory.devdesk.plist。
// 单测注入假实现捕获 writeEntry 内容 / removeEntry 调用次数，不触系统。
struct AutostartOps {
    virtual ~AutostartOps() = default;
    virtual bool isRegistered() = 0;  // 注册表有值 / plist 文件存在 = 开
    virtual core::Result<void> writeEntry(const std::string& content) = 0;
    virtual core::Result<void> removeEntry() = 0;
};

// 平台能力：Windows/macOS 支持；其它 POSIX 兜底 false（设置卡据此整行不建）
bool supported();

// 平台生产 ops（无状态，每次调用新建，调用方各自独立持有）
std::unique_ptr<AutostartOps> systemOps();

// 进程自身 exe 绝对路径（UTF-8，运行时解析不落死值）。失败返回空串（写入方按失败处理）。
std::string executablePath();

// 进程完整命令行（UTF-8，argv[0]=exe）。Windows GetCommandLineW + CommandLineToArgvW、
// macOS _NSGetArgv；POSIX 空表。——file_open::openFileFromArgv 只取 argv[1]，不够本场景
std::vector<std::string> processArgsUtf8();

// ── 纯逻辑（inline，跨平台直测）──

// macOS LaunchAgent 标识（plist 文件名 = Label + ".plist"）
inline constexpr const char* kMacosLabel = "com.coderstory.devdesk";

// Windows Run 项值："<exe>" --minimized（exe 加双引号：路径带空格不会被拆参）
inline std::string buildCommandValue(const std::string& exePath) {
    return "\"" + exePath + "\" --minimized";
}

// macOS plist 最小字段集（Label / ProgramArguments[exe, --minimized] / RunAtLoad）
inline std::string buildPlistContent(const std::string& exePath) {
    // XML 文本转义：路径可能含 & < >（plist 是 XML，裸写会毁文档）
    const auto esc = [](const std::string& s) {
        std::string out;
        for (const char c : s) {
            switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c;
            }
        }
        return out;
    };
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                       "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                       "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                       "<plist version=\"1.0\">\n<dict>\n"
                       "\t<key>Label</key>\n"
                       "\t<string>") +
           kMacosLabel +
           "</string>\n"
           "\t<key>ProgramArguments</key>\n"
           "\t<array>\n"
           "\t\t<string>" +
           esc(exePath) +
           "</string>\n"
           "\t\t<string>--minimized</string>\n"
           "\t</array>\n"
           "\t<key>RunAtLoad</key>\n"
           "\t<true/>\n"
           "</dict>\n</plist>\n";
}

// --minimized 旗标扫描（纯函数）：argv[0]=exe 跳过；精确匹配，不认同族前缀。
// 单测与生产共用（main 的「解析抽成可测纯函数」落点，先例照 selectOpenFileArg）
inline bool parseMinimizedFlag(const std::vector<std::string>& args) {
    if (args.empty()) return false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--minimized") return true;
    }
    return false;
}

// 平台注册内容（win: buildCommandValue；macos: buildPlistContent；posix: 空串）
// ——控制器 setRegistered(true) 的取材分派点，单测也按平台对它断言格式。
std::string buildEntryContent(const std::string& exePath);

// ── 状态机：构造注入 ops，注册 / 注销 / 回读三动作 ──
// 设置页三处共用：打开时回读真值（防漂移）、拨开关写入、写后回读纠正显示。
class AutostartController {
public:
    explicit AutostartController(std::unique_ptr<AutostartOps> ops)
        : ops_(std::move(ops)) {}

    // 回读真值（注册表有项 = 开）；ops 未装配恒 false
    bool isRegistered() const { return ops_ != nullptr && ops_->isRegistered(); }

    // enable=true → 写入注册项（内容 = buildEntryContent(executablePath())）；
    // false → 删除。ops 为空 / exe 路径解析失败 / 系统报错均如实上抛。
    core::Result<void> setRegistered(bool enable) {
        if (ops_ == nullptr) {
            return core::Result<void>::fail(
                core::ErrorCode::Internal, "自启动 ops 未装配（构造注入传空）");
        }
        if (!enable) {
            return ops_->removeEntry();
        }
        const std::string exe = executablePath();
        if (exe.empty()) {
            return core::Result<void>::fail(
                core::ErrorCode::Internal,
                "未能解析本进程 exe 路径，开机自启动项未写入");
        }
        return ops_->writeEntry(buildEntryContent(exe));
    }

private:
    std::unique_ptr<AutostartOps> ops_;
};

} // namespace platform::autostart
