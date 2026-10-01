// src/module_settings/app_tools.cpp
// AppTools M2 Task 9 实现：app.read_settings 单工具（同文件存放，照
// module_agent 侧 app_stats_tool.cpp 的「工厂 + 匿名命名空间小类」形态）。
//
//   app.read_settings  {} → {"themeMode":…,"darkTheme":bool,
//                            "fontScalePercent":N,"editorFontSize":N,
//                            "logRetentionDays":N,"proxyPort":N,
//                            "windowEffect":"…","sidebarCollapsed":bool,
//                            "enabledModules":[…]}
//
// 三个设计决定（对应 Task 9 的三条约束）：
//   1) 只读、非破坏：execute 只做 load()（SettingsStore::load 是 const 纯读，
//      连 .bak 备份都只在解析失败的分支才写——本工具不新增任何写盘路径），
//      故 isDestructive() 保持 ITool 默认 false（agent_loop 不会因此终止 run）。
//   2) 钳制 / 白名单：字段逐个人工挑（kAppReadSettingsFields 注释表），
//      SettingsStore::load() 返回值已 sanitized，越界值不会以原文外泄。
//   3) 磁盘异常不静默：load() 对「打不开的配置文件」只 LOG_WARN + 回落默认值
//      （settings_config.cpp:336）——工具照抄就会把「读不到」当成「用户就这么
//      配的」回喂给 LLM。故读前显式探一次可读性，异常走 isError。
#include "app_tools.h"

#include "settings_config.h"

#include "platform/json.h"

#include <fstream>
#include <string>
#include <utility>

namespace module::settings {

namespace {

using core::Result;
using core::ToolArgs;
using core::ToolResult;
namespace pj = platform::json;

// ---- 结果组装辅助（照 markdown_tools.cpp:30-41 / app_stats_tool.cpp:24-34）----

// 工具级错误：Result ok + isError + {"error":"<中文>"}（错误回喂 LLM）
Result<ToolResult> toolError(const std::string& message) {
    pj::JsonObject err;
    err.set("error", pj::makeString(message));
    auto text = pj::stringify(pj::JsonValue(std::move(err)));
    if (text.isFail()) {
        // 自有字面量 + 一个字符串字段的序列化失败 = 编程不变量破坏
        return Result<ToolResult>::fail(core::ErrorCode::Internal,
                                        "工具错误消息序列化失败: " +
                                            text.errorMessage());
    }
    return Result<ToolResult>::ok(ToolResult{true, text.value()});
}

Result<ToolResult> toolOk(const std::string& contentJson) {
    return Result<ToolResult>::ok(ToolResult{false, contentJson});
}

// 配置可读性探测（非破坏：只 exists / is_directory / 只读 open，不写不改）。
// 返回空串 = 无异常；非空 = 中文错误说明（调用方转 isError）。
//
// 语义与 SettingsStore::load() 对齐但不重合：
//   · 文件不存在 → 空串：首次启动正常路径，load() 返回全默认值，不算错误；
//   · 存在但打不开 / 是目录 → 报错：load() 在同样情形静默回落默认值
//     （settings_config.cpp:336 只 LOG_WARN），agent 视角会变成「设置就是这样」
//     ——静默假答案比显式失败更糟（六阶段口径「静默失败面」）。
// 目录一支按 is_directory 显式识别：std::ifstream 对目录的成败各平台 / 各
// 标准库不一致（libc++ 打开目录可成功、读才失败），不能靠 !probe 兜。
std::string probeConfigReadable(const std::filesystem::path& path) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        return "配置路径不可访问: " + path.string() + "（" + ec.message() + "）";
    }
    if (!exists) return std::string();   // 首次启动：无配置文件 = 正常
    if (std::filesystem::is_directory(path, ec) && !ec) {
        return "配置路径是目录而非文件: " + path.string();
    }
    std::ifstream probe(path);
    if (!probe) {
        return "配置文件无法读取: " + path.string();
    }
    return std::string();
}

// 回喂字段白名单（钉在此处，新增字段必须同步 app_tools.h 的头注与测试）：
//   themeMode        主题模式 auto/light/dark（用户意图）
//   darkTheme        当前解析出的深浅档
//   fontScalePercent 全局字号缩放百分比（钳制 [85,130]）
//   editorFontSize   Markdown 编辑器字号 px（钳制区间见 sanitized）
//   logRetentionDays 调用日志保留天数（0 = 永久）
//   proxyPort        本地代理端口（越界回落默认端口）
//   windowEffect     窗口效果 off/translucent/frosted
//   sidebarCollapsed 侧边栏是否折叠
//   enabledModules   启用的模块 ID 列表（未知 ID 已过滤、恒启用项已补齐）
// 不含任何凭据类字段：API Key 在 platform::keychain，与 config.json 无关。
class AppReadSettingsTool : public core::ITool {
public:
    explicit AppReadSettingsTool(std::filesystem::path configPath)
        : configPath_(std::move(configPath)) {}

    std::string name() const override { return "app.read_settings"; }

    std::string schemaJson() const override {
        return R"({"name":"app.read_settings","description":"读取应用设置（主题模式 / 深浅档 / 字号缩放 / 编辑器字号 / 日志保留 / 代理端口 / 窗口效果 / 侧边栏折叠 / 已启用模块），只读","input_schema":{"type":"object","properties":{}}})";
    }

    // 只读工具：不覆写 isDestructive（ITool 默认 false）。本工具不产生任何
    // 不可逆副作用，agent 在 allowDestructive=0 时也应能调用。
    Result<ToolResult> execute(const ToolArgs& args) override {
        (void)args;   // 无参数（input_schema = 空对象）

        const std::string unreadable = probeConfigReadable(configPath_);
        if (!unreadable.empty()) {
            return toolError(unreadable);
        }

        // 自持 store 实例（不摸模块成员）：agent 工作线程与 UI 线程并发安全。
        // load() 内部已 sanitized（settings_config.cpp:425）。
        const SettingsConfig cfg = SettingsStore(configPath_).load();

        pj::JsonObject o;
        o.set("themeMode", pj::makeString(cfg.themeMode));
        o.set("darkTheme", pj::makeBool(cfg.darkTheme));
        o.set("fontScalePercent", pj::makeInt(cfg.fontScalePercent));
        o.set("editorFontSize", pj::makeInt(cfg.editorFontSize));
        o.set("logRetentionDays", pj::makeInt(cfg.logRetentionDays));
        o.set("proxyPort", pj::makeInt(cfg.proxyPort));
        o.set("windowEffect", pj::makeString(cfg.windowEffect));
        o.set("sidebarCollapsed", pj::makeBool(cfg.sidebarCollapsed));
        o.set("enabledModules", pj::makeStringArray(cfg.enabledModules));

        auto text = pj::stringify(pj::JsonValue(std::move(o)));
        if (text.isFail()) {
            return Result<ToolResult>::fail(core::ErrorCode::Internal,
                                            "设置结果序列化失败: " +
                                                text.errorMessage());
        }
        return toolOk(text.value());
    }

private:
    std::filesystem::path configPath_;
};

} // namespace

std::shared_ptr<core::ITool> makeAppReadSettingsTool(
    const std::filesystem::path& configPath) {
    return std::make_shared<AppReadSettingsTool>(configPath);
}

} // namespace module::settings
