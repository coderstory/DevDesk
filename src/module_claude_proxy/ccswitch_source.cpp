// src/module_claude_proxy/ccswitch_source.cpp
// 从 cc-switch 只读读取 Claude 供应商配置（不复制密钥）。
// base_url 匹配用 host 完全相等 + path 相等（非裸 contains）。
// 主目录探测 HOME / USERPROFILE / HOMEDRIVE+HOMEPATH，不写平台宏。
#include "core/str_util.h"
#include "ccswitch_source.h"

#include "ccswitch_sqlite.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/logging.h"
#include "platform/json.h"

namespace module::claude::ccswitch {
namespace {

constexpr char kDbPathEnv[] = "CCSWITCH_DB_PATH";
constexpr char kCcSwitchDir[] = ".cc-switch";
constexpr char kDbFileName[] = "cc-switch.db";
// 只取 app_type = "claude"：同库的 claude-desktop / codex / gemini 是别的产品。
constexpr char kClaudeAppType[] = "claude";

// 取 key 顺序有语义（同 first_non_empty）：空串也要跳过，可能是空串占位。
constexpr const char* const kApiKeyEnvOrder[] = {
    "ANTHROPIC_AUTH_TOKEN",
    "ANTHROPIC_API_KEY",
    "OPENROUTER_API_KEY",
    "GOOGLE_API_KEY",
};

std::string userHomeDir() {
    for (const char* name : {"HOME", "USERPROFILE"}) {
        const char* value = std::getenv(name);
        if (value != nullptr && *value != '\0') return value;
    }
    // Windows 漫游配置重定向时只剩 HOMEDRIVE+HOMEPATH
    const char* drive = std::getenv("HOMEDRIVE");
    const char* rest = std::getenv("HOMEPATH");
    if (drive != nullptr && *drive != '\0' && rest != nullptr && *rest != '\0') {
        return std::string(drive) + rest;
    }
    return {};
}

// URL → (host, path)：小写化，去 userinfo / 端口 / query / 尾斜杠。
struct UrlParts {
    std::string host;
    std::string path;
};

UrlParts splitUrl(const std::string& rawUrl) {
    UrlParts parts;
    std::string rest = core::str::toLowerAscii(rawUrl);
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

using ccswitch_sqlite::columnOrNull;
using ccswitch_sqlite::columnText;
using ccswitch_sqlite::DbHandle;
using ccswitch_sqlite::hasColumn;
using ccswitch_sqlite::kBusyTimeoutMs;
using ccswitch_sqlite::OpenStatus;
using ccswitch_sqlite::openReadOnly;
using ccswitch_sqlite::providerColumns;
using ccswitch_sqlite::sqliteError;
using ccswitch_sqlite::StmtHandle;

// 结果体装配 wrapper（ListT 只在返回类型里，无法推导）。
CcSwitchProviderList unavailable(CcSwitchSourceStatus status, std::string reason) {
    return ccswitch_sqlite::makeUnavailable<CcSwitchProviderList>(
        status, std::move(reason));
}

platform::json::JsonObject parseObject(const std::string& text) {
    if (text.empty()) return {};
    const core::Result<platform::json::JsonValue> parsed = platform::json::parse(text);
    if (!parsed.isOk()) return {};
    const platform::json::JsonValue value = parsed.value();
    if (!value.isObject()) return {};
    return value.asObject();
}

std::string trimTrailingSlashes(std::string text) {
    while (!text.empty() && text.back() == '/') text.pop_back();
    return text;
}

// settings_config.env → (baseUrl, apiKey)：base_url 取 ANTHROPIC_BASE_URL 并 trim 尾斜杠；
// api_key 取四键里第一个非空串（空串也跳过，非仅键缺失）。
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
    // base_url 与 api_key 各自解析：有地址没 key 时不连坐清空，否则「配了地址」退化成「什么都没配」。
    const platform::json::JsonValue base = envObject.get("ANTHROPIC_BASE_URL");
    if (base.isString()) baseUrl = trimTrailingSlashes(base.asString());
    return {baseUrl, apiKey};
}

CcSwitchProviderList readAll() {
    const std::string path = ccSwitchDbPath();
    DbHandle db;
    std::string reason;
    const OpenStatus opened = openReadOnly(db, &reason, path, "配置库");
    if (opened == OpenStatus::NotFound) {
        return unavailable(CcSwitchSourceStatus::NotFound, std::move(reason));
    }
    if (opened == OpenStatus::Unreadable) {
        return unavailable(CcSwitchSourceStatus::Unreadable, std::move(reason));
    }

    const std::vector<std::string> columns = providerColumns(db.get());
    if (columns.empty()) {
        return unavailable(CcSwitchSourceStatus::Schema,
                           "cc-switch 配置库里没有 providers 表（库结构可能已变）");
    }
    // id / app_type 缺了不可用；name / settings_config / meta 缺了只退化各自字段。
    if (!hasColumn(columns, "id") || !hasColumn(columns, "app_type")) {
        return unavailable(CcSwitchSourceStatus::Schema,
                           "cc-switch 的 providers 表缺少 id / app_type 列");
    }

    const std::string sql =
        "SELECT id, " + columnOrNull(columns, "name") + ", " +
        columnOrNull(columns, "settings_config") + " FROM providers WHERE app_type = ?";
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
        if (info.id.empty()) continue;  // 没有身份 = 用不了
        info.name = columnText(stmt.get(), 1);
        const std::pair<std::string, std::string> credentials =
            resolveEnvCredentials(columnText(stmt.get(), 2));
        info.baseUrl = credentials.first;
        info.apiKey = credentials.second;
        out.providers.push_back(std::move(info));
    }
    if (step != SQLITE_DONE) {
        return unavailable(CcSwitchSourceStatus::Unreadable,
                           "查询 cc-switch 的 providers 表失败：" +
                               sqliteError(db.get(), step));
    }

    // 只记条数与路径，绝不记密钥。
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
    // generic_string()：对外口径（日志/错误文案/测试断言）一律正斜杠；Windows 上
    // string() 是反斜杠，而 sqlite3_open_v2 同样接受正斜杠。
    return (std::filesystem::path(home) / kCcSwitchDir / kDbFileName)
        .generic_string();
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

}  // namespace module::claude::ccswitch
