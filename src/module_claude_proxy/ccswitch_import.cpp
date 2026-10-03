// src/module_claude_proxy/ccswitch_import.cpp
// 从 cc-switch 导入 Profile：只读读侧 + 纯映射实现（头文件见字段映射表与
// 只读硬约束说明）
//
// 平台适配（DevDesk 铁律：业务代码零 #ifdef）：用户主目录探测与打开 flags
// 都在 quota/ccswitch_source 里（已交付、已被 test_claude_quota_ccswitch.cpp
// 守），本文件直接复用，**不重写一份**。
//   ⚠ Windows 待验证：cc-switch 在 Windows 上的库路径**未经真机核实**。当前
//   userHomeDir() 探测 USERPROFILE（+ HOMEDRIVE/HOMEPATH 兜底），推测落在
//   `%USERPROFILE%\.cc-switch\cc-switch.db` —— 但那只是推测，**未实测**。
//   TODO(跨平台)：在真 Windows 上装一次 cc-switch，确认实际路径与表结构后
//   把本注释改写成实测结论；若与推测不同，改 ccSwitchDbPath 一处即可
//   （本文件不含任何路径假设）。
#include "ccswitch_import.h"

#include "provider_config.h"
#include "quota/ccswitch_source.h"

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
using quota::kCcSwitchOpenFlags;

// 导入只取 Claude Code 侧两类：DevDesk 管的是 Claude Code 的 settings.json。
// 同库的 codex / gemini / grokbuild 是别的产品，凭据形态不同，导进来必然坏。
constexpr char kAppTypeClaude[] = "claude";
constexpr char kAppTypeClaudeDesktop[] = "claude-desktop";

constexpr int kBusyTimeoutMs = 2000;

// env 里被「抽成独立 Profile 字段」而**不进 extraEnvJson** 的四个键。
// 顺序无关（是集合），但逐条写出理由便于日后核对：
//   ANTHROPIC_BASE_URL     → baseUrl 列
//   ANTHROPIC_AUTH_TOKEN   → apiKey（与 API_KEY 二选一，TOKEN 优先）
//   ANTHROPIC_API_KEY      → apiKey（兜底）
//   ANTHROPIC_MODEL        → model 列
// 其余 env 键（含 CLAUDE_CODE_SUBAGENT_MODEL / ANTHROPIC_DEFAULT_*_MODEL 等）
// 全部落 extraEnvJson ——「导入全部的配置内容」靠的就是这一条。
constexpr char kEnvBaseUrl[] = "ANTHROPIC_BASE_URL";
constexpr char kEnvAuthToken[] = "ANTHROPIC_AUTH_TOKEN";
constexpr char kEnvApiKey[] = "ANTHROPIC_API_KEY";
constexpr char kEnvModel[] = "ANTHROPIC_MODEL";

bool isReservedEnvKey(const std::string& key) {
    return key == kEnvBaseUrl || key == kEnvAuthToken || key == kEnvApiKey ||
           key == kEnvModel;
}

// ---- sqlite RAII（与 quota/ccswitch_source.cpp 同款：确定性关闭，不抛异常）----
struct SqliteCloser {
    void operator()(sqlite3* db) const noexcept {
        if (db != nullptr) sqlite3_close(db);
    }
};
using DbHandle = std::unique_ptr<sqlite3, SqliteCloser>;

struct SqliteStmtCloser {
    void operator()(sqlite3_stmt* stmt) const noexcept {
        if (stmt != nullptr) sqlite3_finalize(stmt);
    }
};
using StmtHandle = std::unique_ptr<sqlite3_stmt, SqliteStmtCloser>;

std::string columnText(sqlite3_stmt* stmt, int index) {
    const unsigned char* text = sqlite3_column_text(stmt, index);
    if (text == nullptr) return {};
    return std::string(reinterpret_cast<const char*>(text));
}

std::vector<std::string> providerColumns(sqlite3* db) {
    std::vector<std::string> columns;
    const char* sql = "SELECT name FROM pragma_table_info('providers')";
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) return columns;
    const StmtHandle stmt(raw);
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        columns.push_back(columnText(stmt.get(), 0));
    }
    return columns;
}

bool hasColumn(const std::vector<std::string>& columns, const char* name) {
    return std::find(columns.begin(), columns.end(), name) != columns.end();
}

std::string sqliteError(sqlite3* db, int code) {
    const char* message = db != nullptr ? sqlite3_errmsg(db) : nullptr;
    std::string text = message != nullptr ? message : sqlite3_errstr(code);
    if (text.empty()) text = "sqlite 错误 " + std::to_string(code);
    return text;
}

CcSwitchImportRows unavailable(CcSwitchImportStatus status, std::string reason) {
    CcSwitchImportRows out;
    out.status = status;
    out.unavailableReason = std::move(reason);
    return out;
}

// ---- 读侧 ----

CcSwitchImportRows readAll() {
    const std::string path = ccSwitchDbPath();
    if (path.empty()) {
        return unavailable(CcSwitchImportStatus::NotFound,
                           "找不到用户主目录，无法定位 cc-switch 配置库");
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return unavailable(CcSwitchImportStatus::NotFound,
                           "未找到 cc-switch 数据库（" + path + "）");
    }

    // 只读打开（flags 见 quota/ccswitch_source：恒 READONLY，恒不含 CREATE）
    sqlite3* raw = nullptr;
    const int openCode =
        sqlite3_open_v2(path.c_str(), &raw, kCcSwitchOpenFlags, nullptr);
    const DbHandle db(raw);
    if (openCode != SQLITE_OK) {
        return unavailable(CcSwitchImportStatus::Unreadable,
                           "打不开 cc-switch 数据库（只读）：" +
                               sqliteError(db.get(), openCode));
    }
    sqlite3_busy_timeout(db.get(), kBusyTimeoutMs);

    const std::vector<std::string> columns = providerColumns(db.get());
    if (columns.empty()) {
        return unavailable(CcSwitchImportStatus::Schema,
                           "cc-switch 数据库里没有 providers 表（库结构可能已变）");
    }
    // 导入要四列全在：id（身份）/ app_type（筛产品）/ name（Profile 名）/
    // settings_config（映射的全部来源）。缺任一列这条导入路径就没法工作——
    // 不像 quota 那侧能逐字段退化（它只要 base_url + usage_script）。
    for (const char* required : {"id", "app_type", "name", "settings_config"}) {
        if (!hasColumn(columns, required)) {
            return unavailable(
                CcSwitchImportStatus::Schema,
                std::string("cc-switch 的 providers 表缺少 ") + required + " 列");
        }
    }

    // app_type IN (...)：SQL 里做产品筛选，不用读出来再判。
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

// env 对象里取一个字符串键（不存在 / 非字符串 / 空串一律空串）。
// 空串按「没有」处理，与 quota/ccswitch_source 的 first_non_empty 同语义：
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

    // 同名冲突：DevDesk 已有同名 profile → 跳过，**不覆盖**。
    // 放在最前面判（解析之后、读字段之前）：撞名的那条根本不该被看第二眼。
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

    // authField 三态（值域同 claude_service.h / provider_config.h）：
    // 两个键都非空 → both；只有 TOKEN → bearer；只有 API_KEY → api_key；
    // 都无 → both（无证据时的既有默认值）。
    std::string authField = "both";
    if (!authToken.empty() && !apiKeyEnv.empty()) {
        authField = "both";
    } else if (!authToken.empty()) {
        authField = "bearer";
    } else if (!apiKeyEnv.empty()) {
        authField = "api_key";
    }

    // extraEnvJson：env 里四个保留键之外的全部键值。走既有 serializeExtraEnv
    // （与表单写 extra_env 同一序列化器，键自动排序、值恒字符串），保证写进去
    // 的形态与 UI 手填路径**逐字同款**（读侧 parseExtraEnv 因此不会退化）。
    // 非字符串值的 env 键**丢弃**：extra_env 的契约是 string→string
    // （parseExtraEnv 对非字符串值整体 fail），硬塞进去等于把整段配置读成空。
    std::map<std::string, std::string> extraEnv;
    for (const std::string& key : env.keys()) {
        if (isReservedEnvKey(key)) continue;
        const std::string value = envString(env, key.c_str());
        if (value.empty()) continue;
        extraEnv[key] = value;
    }
    std::string extraEnvJson;
    if (!extraEnv.empty()) {
        auto serialized = serializeExtraEnv(extraEnv);
        // 全字符串 map 序列化，失败理论不可达；空串兜底（读侧容错为空表）
        if (serialized.isOk()) extraEnvJson = serialized.value();
    }

    // extraSettingsJson：settings_config 去掉 env 块后的全部顶层属性。
    // 走既有落库口径（pretty，见 advanced-json-editor §7.3 D-6）。
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