// src/module_settings/app_tools.cpp
// app.read_settings 实现：工厂 + 匿名命名空间小类（同 app_stats_tool.cpp 形态）。
// 返回字段：themeMode / darkTheme / fontScalePercent / editorFontSize /
// logRetentionDays / proxyPort / windowEffect / sidebarCollapsed / enabledModules。
//
// 设计：只读非破坏（不覆写 isDestructive，不新增写盘路径）；字段白名单取自
// SettingsStore::load() 的 sanitized 结果；存储异常经 loadChecked() 报 isError，
// 不把「读不到」静默当默认值回喂 LLM。
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

// 存储可读性探测：空串 = 无异常；非空 = 中文错误说明（调用方转 isError）。
// 与 SettingsStore::loadChecked() 对齐：一个键都没配（首次启动）= 空串不算错；
// 库未就绪 / 表不存在 / 值损坏 = 报错（load() 会静默回落默认值，不可取）。
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

// 回喂字段白名单（新增字段须同步 app_tools.h 与测试）：
//   themeMode 主题模式 / darkTheme 当前深浅档 / fontScalePercent[85,130] /
//   editorFontSize / logRetentionDays（0 = 永久） / proxyPort /
//   windowEffect / sidebarCollapsed / enabledModules（未知 ID 已过滤、恒启用项已补齐）。
// 不含凭据类字段：API Key 在 claude_profiles.api_key_encrypted 列。
class AppReadSettingsTool : public core::ITool {
public:
    explicit AppReadSettingsTool(core::data::SqliteDb* db) : db_(db) {}

    std::string name() const override { return "app.read_settings"; }

    std::string schemaJson() const override {
        return R"({"name":"app.read_settings","description":"读取应用设置（主题模式 / 深浅档 / 字号缩放 / 编辑器字号 / 日志保留 / 代理端口 / 窗口效果 / 侧边栏折叠 / 已启用模块），只读","input_schema":{"type":"object","properties":{}}})";
    }

    // 只读工具：不覆写 isDestructive（默认 false），allowDestructive=0 时也可调用。
    Result<ToolResult> execute(const ToolArgs& args) override {
        (void)args;   // 无参数（input_schema = 空对象）

        const std::string unreadable = probeConfigReadable(db_);
        if (!unreadable.empty()) {
            return toolError(unreadable);
        }

        // 自持 store（不摸模块成员）：agent 工作线程与 UI 线程并发安全；已 sanitized。
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
