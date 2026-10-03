// src/module_claude_proxy/quota/ccswitch_source.cpp
// H：从 cc-switch **只读**读取供应商配置（不复制密钥）
//
// 字段语义逐条抄自 cc-switch（rev bfaaba16），本文件是那份抄写的落点：
//   src-tauri/src/provider.rs:145  resolve_usage_credentials() 的
//                                `AppType::Claude` 分支（settings_config.env）
//   src-tauri/src/provider.rs:268  UsageScript（usage_script 里的 JSON 键名）
//   src-tauri/src/provider.rs:444  ProviderMeta（meta 里 usage_script 的键名）
//   src-tauri/src/database/        providers 表（id/app_type/name/settings_config/meta）
//
// 两条与 cc-switch 的**刻意分歧**（都是往「更安全」方向改，理由见头文件）：
//   1. 打开用 SQLITE_OPEN_READONLY（cc-switch 自己是读写库）；DevDesk 只借不还。
//   2. base_url 匹配用 host **完全相等** + path 相等（cc-switch 是裸 `contains`），
//      `api.deepseek.com.evil.com` 不命中。假阳性的代价（把别家的余量挂到当前
//      profile 上）远大于假阴性的代价（G6 里让用户手选一下）。
//
// 平台适配：用户主目录靠探测 HOME / USERPROFILE / HOMEDRIVE+HOMEPATH 拿到，
// **不写 #ifdef _WIN32**（DevDesk CLAUDE.md 硬约束 2：业务代码零平台宏）。
// 要按平台拆就得往 core::paths 加 userHomeDir()，那是共享层改动、超出 H 范围；
// 探测这三个名字本来就是可移植写法，见 userHomeDir() 处的注释。
#include "ccswitch_source.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/logging.h"
#include "platform/json.h"

namespace module::claude::quota {
namespace {

constexpr char kDbPathEnv[] = "CCSWITCH_DB_PATH";
constexpr char kCcSwitchDir[] = ".cc-switch";
constexpr char kDbFileName[] = "cc-switch.db";
// cc-switch 的 app_type 取值（provider.rs 的 AppType 枚举序列化成小写 snake）。
// 只取 claude 一类：DevDesk 管的是 Claude Code 的 settings.json；同库里的
// claude-desktop / codex / gemini / grokbuild 是别的产品，凭据形态也不同。
constexpr char kClaudeAppType[] = "claude";

// cc-switch 应用正在写自己的库；只读连接撞上写锁时短暂等一下，别一上来就报失败。
constexpr int kBusyTimeoutMs = 2000;

// env 里取推理 key 的顺序 —— 逐条对齐 resolve_usage_credentials 的 Claude 分支
// 里 first_non_empty(env, [...])。**顺序有语义**：OpenCode Go 实测把 key 存在
// ANTHROPIC_API_KEY（不是 AUTH_TOKEN），而预设会把 AUTH_TOKEN 种成空串占位。
constexpr const char* const kApiKeyEnvOrder[] = {
    "ANTHROPIC_AUTH_TOKEN",
    "ANTHROPIC_API_KEY",
    "OPENROUTER_API_KEY",
    "GOOGLE_API_KEY",
};

std::string toLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

// 用户主目录：探测三个环境变量而不是写平台宏（头文件注释已说明理由）。
std::string userHomeDir() {
    for (const char* name : {"HOME", "USERPROFILE"}) {
        const char* value = std::getenv(name);
        if (value != nullptr && *value != '\0') return value;
    }
    // Windows 上 profile 落在别处时（漫游配置重定向）只剩这一对
    const char* drive = std::getenv("HOMEDRIVE");
    const char* rest = std::getenv("HOMEPATH");
    if (drive != nullptr && *drive != '\0' && rest != nullptr && *rest != '\0') {
        return std::string(drive) + rest;
    }
    return {};
}

// URL → (host, path)，小写化 + 去 userinfo / 端口 / query / 尾部 '/'。
// 与 opencode_go_provider.cpp:126 的 splitUrl 同款（两处各自持有，将来要共用时
// 再提到一个头里——G0 头文件没有 URL 工具，不宜为两份抄本就动它）。
struct UrlParts {
    std::string host;
    std::string path;
};

UrlParts splitUrl(const std::string& rawUrl) {
    UrlParts parts;
    std::string rest = toLower(rawUrl);
    const std::size_t scheme = rest.find("://");
    if (scheme != std::string::npos) rest.erase(0, scheme + 3);

    const std::size_t authorityEnd = rest.find_first_of("/?#");
    std::string authority = rest.substr(0, authorityEnd);
    parts.path =
        authorityEnd == std::string::npos ? std::string() : rest.substr(authorityEnd);

    const std::size_t at = authority.rfind('@');
    if (at != std::string::npos) authority.erase(0, at + 1);
    const std::size_t colon = authority.find(':');
    if (colon != std::string::npos) authority.erase(colon);

    const std::size_t query = parts.path.find_first_of("?#");
    if (query != std::string::npos) parts.path.erase(query);
    while (!parts.path.empty() && parts.path.back() == '/') parts.path.pop_back();

    parts.host = authority;
    return parts;
}

// sqlite3* 的 RAII 收口（项目铁律：不抛异常，全部走确定性关闭）。
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

// providers 表的列名清单。表不存在时查询失败 → 空清单（调用方按 Schema 处理）。
// 这是「schema 演进防护」的前半段：不写死 SELECT *，而是先看清楚有哪些列。
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

// 缺列时用 NULL 占位（而不是整条 SELECT 失败）：cc-switch 哪天给 providers 换 /
// 拆列，我们仍然能把认得的字段读出来，而不是一颗雷「来源坏了」。
std::string columnOrNull(const std::vector<std::string>& columns, const char* name) {
    return hasColumn(columns, name) ? std::string(name) : std::string("NULL");
}

platform::json::JsonObject parseObject(const std::string& text) {
    if (text.empty()) return {};
    const core::Result<platform::json::JsonValue> parsed = platform::json::parse(text);
    if (!parsed.isOk()) return {};
    const platform::json::JsonValue value = parsed.value();
    if (!value.isObject()) return {};
    return value.asObject();
}

std::string stringField(const platform::json::JsonObject& object, const char* key) {
    const platform::json::JsonValue value = object.get(key);
    return value.isString() ? value.asString() : std::string();
}

std::string trimTrailingSlashes(std::string text) {
    while (!text.empty() && text.back() == '/') text.pop_back();
    return text;
}

// settings_config.env → (baseUrl, apiKey)，逐条对齐 resolve_usage_credentials 的
// Claude 分支：base_url 取 ANTHROPIC_BASE_URL 并 trim 尾斜杠；api_key 取四个键里
// **第一个非空串**（JS `||` 的语义：空串要跳过，不只是「键不存在」才换下一个）。
std::pair<std::string, std::string> resolveEnvCredentials(
    const std::string& settingsConfig) {
    const platform::json::JsonValue env = parseObject(settingsConfig).get("env");
    const platform::json::JsonObject envObject =
        env.isObject() ? env.asObject() : platform::json::JsonObject();
    std::string baseUrl;
    std::string apiKey;
    for (const char* name : kApiKeyEnvOrder) {
        const platform::json::JsonValue value = envObject.get(name);
        if (value.isString() && !value.asString().empty()) {
            apiKey = value.asString();
            break;
        }
    }
    // base_url 与 api_key **各自**解析：某些供应商有 base_url 但没 key（地址配好了
    // 但还没填 token），连坐清空会让「配了地址」退化成「什么都没配」。
    const platform::json::JsonValue base = envObject.get("ANTHROPIC_BASE_URL");
    if (base.isString()) baseUrl = trimTrailingSlashes(base.asString());
    return {baseUrl, apiKey};
}

// meta.usage_script → usage 相关字段。meta 是 NULL / 非法 JSON / 没有
// usage_script / usage_script 不是对象，四种都当「没配」——逐字段退化，不是整行丢。
void fillUsageScript(CcSwitchProviderInfo* info, const std::string& meta) {
    const platform::json::JsonValue script = parseObject(meta).get("usage_script");
    if (!script.isObject()) return;
    const platform::json::JsonObject usage = script.asObject();
    info->codingPlanProvider = stringField(usage, "codingPlanProvider");
    info->accessKeyId = stringField(usage, "accessKeyId");
    info->secretAccessKey = stringField(usage, "secretAccessKey");
}

CcSwitchProviderList unavailable(CcSwitchSourceStatus status, std::string reason) {
    CcSwitchProviderList out;
    out.status = status;
    out.unavailableReason = std::move(reason);
    return out;
}

std::string sqliteError(sqlite3* db, int code) {
    const char* message = db != nullptr ? sqlite3_errmsg(db) : nullptr;
    std::string text = message != nullptr ? message : sqlite3_errstr(code);
    if (text.empty()) text = "sqlite 错误 " + std::to_string(code);
    return text;
}

CcSwitchProviderList readAll() {
    const std::string path = ccSwitchDbPath();
    if (path.empty()) {
        return unavailable(CcSwitchSourceStatus::NotFound,
                           "找不到用户主目录，无法定位 cc-switch 配置库");
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return unavailable(CcSwitchSourceStatus::NotFound,
                           "未找到 cc-switch 配置库（" + path + "）");
    }

    // 只读打开：不含 READWRITE / CREATE，绝不把库建出来、绝不动 cc-switch 的文件
    sqlite3* raw = nullptr;
    const int openCode =
        sqlite3_open_v2(path.c_str(), &raw, kCcSwitchOpenFlags, nullptr);
    const DbHandle db(raw);
    if (openCode != SQLITE_OK) {
        // 取错误串时句柄可能已是 nullptr（sqlite3_open_v2 失败时也可能留一个非空
        // 的坏句柄），sqliteError 对两者都安全；DbHandle 保证它一定被关掉。
        return unavailable(CcSwitchSourceStatus::Unreadable,
                           "打不开 cc-switch 配置库（只读）：" +
                               sqliteError(db.get(), openCode));
    }
    sqlite3_busy_timeout(db.get(), kBusyTimeoutMs);

    const std::vector<std::string> columns = providerColumns(db.get());
    if (columns.empty()) {
        return unavailable(CcSwitchSourceStatus::Schema,
                           "cc-switch 配置库里没有 providers 表（库结构可能已变）");
    }
    // id = 身份（没有它没法当引用用）；app_type = 分不出 Claude 侧。
    // name / settings_config / meta 缺了只影响各自的字段，退化即可。
    if (!hasColumn(columns, "id") || !hasColumn(columns, "app_type")) {
        return unavailable(CcSwitchSourceStatus::Schema,
                           "cc-switch 的 providers 表缺少 id / app_type 列");
    }

    const std::string sql =
        "SELECT id, " + columnOrNull(columns, "name") + ", " +
        columnOrNull(columns, "settings_config") + ", " +
        columnOrNull(columns, "meta") + " FROM providers WHERE app_type = ?";
    sqlite3_stmt* rowStmt = nullptr;
    if (sqlite3_prepare_v2(db.get(), sql.c_str(), -1, &rowStmt, nullptr) != SQLITE_OK) {
        return unavailable(CcSwitchSourceStatus::Schema,
                           "读 cc-switch 的 providers 表失败：" +
                               sqliteError(db.get(), SQLITE_ERROR));
    }
    const StmtHandle stmt(rowStmt);
    sqlite3_bind_text(stmt.get(), 1, kClaudeAppType, -1, SQLITE_TRANSIENT);

    CcSwitchProviderList out;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        CcSwitchProviderInfo info;
        info.id = columnText(stmt.get(), 0);
        if (info.id.empty()) continue;  // 没有身份 = 用不了，直接跳过
        info.name = columnText(stmt.get(), 1);
        const std::pair<std::string, std::string> credentials =
            resolveEnvCredentials(columnText(stmt.get(), 2));
        info.baseUrl = credentials.first;
        info.apiKey = credentials.second;
        fillUsageScript(&info, columnText(stmt.get(), 3));
        out.providers.push_back(std::move(info));
    }
    if (step != SQLITE_DONE) {
        return unavailable(CcSwitchSourceStatus::Unreadable,
                           "查询 cc-switch 的 providers 表失败：" +
                               sqliteError(db.get(), step));
    }

    // 日志只记条数与库路径 —— **绝不**记 apiKey / accessKeyId / secretAccessKey。
    LOG_DEBUG("cc-switch 只读源：读到 " + std::to_string(out.providers.size()) +
              " 个 Claude 供应商（" + path + "）");
    return out;
}

}  // namespace

const int kCcSwitchOpenFlags =
    SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX;

std::string ccSwitchDbPath() {
    if (const char* override = std::getenv(kDbPathEnv)) {
        if (*override != '\0') return override;
    }
    const std::string home = userHomeDir();
    if (home.empty()) return {};
    return (std::filesystem::path(home) / kCcSwitchDir / kDbFileName).string();
}

CcSwitchProviderList listCcSwitchProviders() { return readAll(); }

std::string matchCcSwitchProviderByBaseUrl(const std::string& baseUrl) {
    const UrlParts want = splitUrl(baseUrl);
    if (want.host.empty()) return {};
    const CcSwitchProviderList all = readAll();
    for (const CcSwitchProviderInfo& provider : all.providers) {
        const UrlParts have = splitUrl(provider.baseUrl);
        if (have.host.empty() || have.host != want.host) continue;
        if (have.path != want.path) continue;
        // 只记 id（引用），不记任何凭据
        LOG_DEBUG("cc-switch 匹配命中：provider " + provider.id + " ← " + baseUrl);
        return provider.id;
    }
    LOG_DEBUG("cc-switch 匹配未命中：" + baseUrl);
    return {};
}

}  // namespace module::claude::quota