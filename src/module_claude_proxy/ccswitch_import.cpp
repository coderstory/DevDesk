// src/module_claude_proxy/ccswitch_import.cpp
// cc-switch 导入实现（字段映射表见头文件）。Windows 的 cc-switch 库路径尚未实测。
#include "ccswitch_import.h"

#include "provider_config.h"
#include "quota/ccswitch_source.h"
#include "quota/ccswitch_sqlite.h"  // 与 quota/ccswitch_source.cpp 共用的读取脚手架

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include "core/logging.h"
#include "platform/json.h"

namespace module::claude {

namespace {

using quota::ccSwitchDbPath;
using quota::ccswitch_sqlite::columnText;
using quota::ccswitch_sqlite::DbHandle;
using quota::ccswitch_sqlite::hasColumn;
using quota::ccswitch_sqlite::OpenStatus;
using quota::ccswitch_sqlite::openReadOnly;
using quota::ccswitch_sqlite::providerColumns;
using quota::ccswitch_sqlite::sqliteError;
using quota::ccswitch_sqlite::StmtHandle;

// wrapper：模板参数 ListT 只在返回类型里，无法推导。
CcSwitchImportRows unavailable(CcSwitchImportStatus status, std::string reason) {
    return quota::ccswitch_sqlite::makeUnavailable<CcSwitchImportRows>(
        status, std::move(reason));
}

// 导入只取 Claude Code 侧两类：同库的 codex / gemini 等凭据形态不同，导进来必然坏。
constexpr char kAppTypeClaude[] = "claude";
constexpr char kAppTypeClaudeDesktop[] = "claude-desktop";

// env 里抽成独立 Profile 字段的四个键（BASE_URL→baseUrl；AUTH_TOKEN / API_KEY→
// apiKey，TOKEN 优先；MODEL→model），其余 env 键全落 extraEnvJson。
constexpr char kEnvBaseUrl[] = "ANTHROPIC_BASE_URL";
constexpr char kEnvAuthToken[] = "ANTHROPIC_AUTH_TOKEN";
constexpr char kEnvApiKey[] = "ANTHROPIC_API_KEY";
constexpr char kEnvModel[] = "ANTHROPIC_MODEL";

bool isReservedEnvKey(const std::string& key) {
    return key == kEnvBaseUrl || key == kEnvAuthToken || key == kEnvApiKey ||
           key == kEnvModel;
}

// ---- 读侧（sqlite 骨架来自 quota/ccswitch_sqlite.h）----

CcSwitchImportRows readAll() {
    const std::string path = ccSwitchDbPath();
    DbHandle db;
    std::string reason;
    const OpenStatus opened = openReadOnly(db, &reason, path, "数据库");
    if (opened == OpenStatus::NotFound) {
        return unavailable(CcSwitchImportStatus::NotFound, std::move(reason));
    }
    if (opened == OpenStatus::Unreadable) {
        return unavailable(CcSwitchImportStatus::Unreadable, std::move(reason));
    }

    const std::vector<std::string> columns = providerColumns(db.get());
    if (columns.empty()) {
        return unavailable(CcSwitchImportStatus::Schema,
                           "cc-switch 数据库里没有 providers 表（库结构可能已变）");
    }
    // 导入要四列全在，缺任一列即 Schema 错误。
    for (const char* required : {"id", "app_type", "name", "settings_config"}) {
        if (!hasColumn(columns, required)) {
            return unavailable(
                CcSwitchImportStatus::Schema,
                std::string("cc-switch 的 providers 表缺少 ") + required + " 列");
        }
    }

    const std::string sql =
        "SELECT id, app_type, name, settings_config FROM providers "
        "WHERE app_type IN (?, ?)";
    sqlite3_stmt* rowStmt = nullptr;
    if (sqlite3_prepare_v2(db.get(), sql.c_str(), -1, &rowStmt, nullptr) != SQLITE_OK) {
        return unavailable(CcSwitchImportStatus::Schema,
                           "读 cc-switch 的 providers 表失败：" +
                               sqliteError(db.get(), SQLITE_ERROR));
    }
    const StmtHandle stmt(rowStmt);
    sqlite3_bind_text(stmt.get(), 1, kAppTypeClaude, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, kAppTypeClaudeDesktop, -1, SQLITE_TRANSIENT);

    CcSwitchImportRows out;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        CcSwitchImportRow row;
        row.id = columnText(stmt.get(), 0);
        if (row.id.empty()) continue;  // 没有身份 = 没法去重/报错，跳过
        row.appType = columnText(stmt.get(), 1);
        row.name = columnText(stmt.get(), 2);
        row.settingsConfig = columnText(stmt.get(), 3);
        out.rows.push_back(std::move(row));
    }
    if (step != SQLITE_DONE) {
        return unavailable(CcSwitchImportStatus::Unreadable,
                           "查询 cc-switch 的 providers 表失败：" +
                               sqliteError(db.get(), step));
    }

    // 日志只记**条数**与库路径 —— 绝不记 settings_config 原文（含明文密钥）。
    LOG_DEBUG("cc-switch 导入源：读到 " + std::to_string(out.rows.size()) +
              " 条可导入配置（" + path + "）");
    return out;
}

// ---- 映射 ----

CcSwitchImportMapping skipped(std::string reason) {
    CcSwitchImportMapping out;
    out.ok = false;
    out.skipReason = std::move(reason);
    return out;
}

// env 对象里取一个字符串键（不存在/非字符串一律空串）。空串按「没有」处理：
// cc-switch 预设会把某些键种成空串占位。
std::string envString(const platform::json::JsonObject& env, const char* key) {
    const platform::json::JsonValue value = env.get(key);
    return value.isString() ? value.asString() : std::string();
}

}  // namespace

CcSwitchImportRows listCcSwitchImportRows() { return readAll(); }

CcSwitchImportMapping mapCcSwitchProviderToProfile(
    const std::string& ccName, const std::string& settingsConfigJson,
    const std::vector<std::string>& existingNames) {
    auto parsed = platform::json::parse(settingsConfigJson);
    if (parsed.isFail()) {
        return skipped("settings_config 不是合法 JSON");
    }
    if (!parsed.value().isObject()) {
        return skipped("settings_config 顶层不是 JSON 对象");
    }
    if (ccName.empty()) {
        return skipped("cc-switch 里这条配置没有名称");
    }

    // 同名冲突 → 跳过不覆盖。
    if (std::find(existingNames.begin(), existingNames.end(), ccName) !=
        existingNames.end()) {
        return skipped("DevDesk 已有同名 Profile，已跳过");
    }

    const platform::json::JsonObject root = parsed.value().asObject();

    // env 缺失 / 非对象 → 当空对象（逐字段退化，不是整条丢）
    const platform::json::JsonValue envValue = root.get("env");
    const platform::json::JsonObject env = envValue.isObject()
                                              ? envValue.asObject()
                                              : platform::json::JsonObject();

    const std::string baseUrl = envString(env, kEnvBaseUrl);
    if (baseUrl.empty()) {
        return skipped("缺 base_url（env.ANTHROPIC_BASE_URL 为空或不存在）");
    }

    // apiKey：TOKEN 优先，缺（空串也算缺）则 API_KEY。
    const std::string authToken = envString(env, kEnvAuthToken);
    const std::string apiKeyEnv = envString(env, kEnvApiKey);
    const std::string apiKey =
        !authToken.empty() ? authToken : apiKeyEnv;

    // 缺密钥 → 确定性跳过（源缺必填字段，重试也不会好）。
    if (apiKey.empty()) {
        return skipped("缺密钥（env 无 ANTHROPIC_AUTH_TOKEN / ANTHROPIC_API_KEY）");
    }

    // authField 三态：两键都非空 → both；只有 TOKEN → bearer；只有 API_KEY → api_key。
    std::string authField = "both";
    if (!authToken.empty() && !apiKeyEnv.empty()) {
        authField = "both";
    } else if (!authToken.empty()) {
        authField = "bearer";
    } else if (!apiKeyEnv.empty()) {
        authField = "api_key";
    }

    // extraEnvJson：保留键之外的全部键值，走 serializeExtraEnv。非字符串值丢弃：
    // extra_env 契约是 string→string，硬塞进去等于把整段配置读成空。
    std::map<std::string, std::string> extraEnv;
    for (const std::string& key : env.keys()) {
        if (isReservedEnvKey(key)) continue;
        const std::string value = envString(env, key.c_str());
        if (value.empty()) continue;
        extraEnv[key] = value;
    }
    // 双 key 并存：未用作 apiKey 的 API_KEY 保留进 extraEnvJson（TOKEN 非空时
    // apiKey 已取 TOKEN，故此处只可能是 API_KEY）；运行期安全由既有守卫保证。
    if (!authToken.empty() && !apiKeyEnv.empty()) {
        extraEnv[kEnvApiKey] = apiKeyEnv;
    }
    std::string extraEnvJson;
    if (!extraEnv.empty()) {
        auto serialized = serializeExtraEnv(extraEnv);
        // 全字符串 map 序列化，失败理论不可达；空串兜底（读侧容错为空表）
        if (serialized.isOk()) extraEnvJson = serialized.value();
    }

    // extraSettingsJson：settings_config 去掉 env 块后的全部顶层属性（pretty 落库口径）。
    platform::json::JsonObject settings;
    for (const std::string& key : root.keys()) {
        if (key == "env") continue;
        settings.set(key, root.get(key));
    }
    std::string extraSettingsJson;
    if (!settings.keys().empty()) {
        auto pretty = platform::json::stringify(
            platform::json::JsonValue(std::move(settings)), /*pretty=*/true);
        if (pretty.isOk()) extraSettingsJson = pretty.value();
    }

    CcSwitchImportMapping out;
    out.ok = true;
    out.name = ccName;
    out.baseUrl = baseUrl;
    out.apiKey = apiKey;
    out.model = envString(env, kEnvModel);
    out.authField = authField;
    out.extraEnvJson = extraEnvJson;
    out.extraSettingsJson = extraSettingsJson;
    return out;
}

}  // namespace module::claude