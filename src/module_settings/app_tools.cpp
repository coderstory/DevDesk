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
//   3) 存储异常不静默：load() 对「读不到」只 LOG_ERROR + 回落默认值——工具
//      照抄就会把「读不到」当成「用户就这么配的」回喂给 LLM。故改用
//      loadChecked()：库未就绪 / 表不存在 / 键值损坏一律走 isError。
#include "app_tools.h"

#include "settings_config.h"

#include "core/app_config.h"
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

// 存储可读性探测（2026-10-03 批次 E2：随配置落点从文件改为 app_config 表，
// 原「探测配置文件能否打开」的一支随之删除）。
// 返回空串 = 无异常；非空 = 中文错误说明（调用方转 isError）。
//
// 语义与 SettingsStore::loadChecked() 严格对齐（这是它存在的理由）：
//   · 一个键都没配（首次启动）→ 空串，loadChecked 返回全默认值，不算错误；
//   · 库未就绪 / 表不存在 / 值损坏 → 报错。load() 在同样情形只 LOG_ERROR +
//     回落默认值，agent 视角会变成「设置就是这样」——静默假答案比显式失败
//     更糟（六阶段口径「静默失败面」）。
std::string probeConfigReadable(core::data::SqliteDb* db) {
    if (db == nullptr) {
        return "应用数据库未就绪（core::appDatabase() 为空），读不到应用配置";
    }
    auto probe = core::appconfig::listWithPrefix(
        *db, module::settings::SettingsStore::keyNamespace());
    if (probe.isFail()) {
        return "读取应用配置失败（app_config 表不存在或 SQL 失败）: " +
               probe.errorMessage();
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
// 不含任何凭据类字段：API Key 在 claude_profiles.api_key_encrypted 列
// （2026-10-02 E1 存储统一），本工具的字段白名单里没有它，导出/复制链路
// 也不会经由这里拿到密钥。
class AppReadSettingsTool : public core::ITool {
public:
    explicit AppReadSettingsTool(core::data::SqliteDb* db) : db_(db) {}

    std::string name() const override { return "app.read_settings"; }

    std::string schemaJson() const override {
        return R"({"name":"app.read_settings","description":"读取应用设置（主题模式 / 深浅档 / 字号缩放 / 编辑器字号 / 日志保留 / 代理端口 / 窗口效果 / 侧边栏折叠 / 已启用模块），只读","input_schema":{"type":"object","properties":{}}})";
    }

    // 只读工具：不覆写 isDestructive（ITool 默认 false）。本工具不产生任何
    // 不可逆副作用，agent 在 allowDestructive=0 时也应能调用。
    Result<ToolResult> execute(const ToolArgs& args) override {
        (void)args;   // 无参数（input_schema = 空对象）

        const std::string unreadable = probeConfigReadable(db_);
        if (!unreadable.empty()) {
            return toolError(unreadable);
        }

        // 自持 store 实例（不摸模块成员）：agent 工作线程与 UI 线程并发安全。
        // loadChecked 内部已 sanitized。
        auto loaded = SettingsStore(db_).loadChecked();
        if (loaded.isFail()) {
            return toolError("读取应用配置失败: " + loaded.errorMessage());
        }
        const SettingsConfig cfg = loaded.value();

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
    core::data::SqliteDb* db_ = nullptr;
};

} // namespace

std::shared_ptr<core::ITool> makeAppReadSettingsTool(core::data::SqliteDb* db) {
    return std::make_shared<AppReadSettingsTool>(db);
}

} // namespace module::settings
