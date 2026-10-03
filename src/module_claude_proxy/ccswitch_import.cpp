// src/module_claude_proxy/ccswitch_import.cpp
// 从 cc-switch 导入 Profile：只读读侧 + 纯映射实现（头文件见字段映射表与
// 只读硬约束说明）
//
// 平台适配（DevDesk 铁律：业务代码零 #ifdef）：用户主目录探测与打开 flags
// 都在 quota/ccswitch_source 里（已交付、已被 test_claude_quota_ccswitch.cpp
// 守）；sqlite 读取骨架（RAII / 探列 / columnText / 只读打开）在
// quota/ccswitch_sqlite.h，本文件与 quota/ccswitch_source.cpp **真的共用同一份**
// （2026-10-03 轻量化修正：此前这句话与实现相反——6 个函数被逐字抄了一遍）。
//   ⚠ Windows 待验证：cc-switch 在 Windows 上的库路径**未经真机核实**。当前
//   userHomeDir() 探测 USERPROFILE（+ HOMEDRIVE/HOMEPATH 兜底），推测落在
//   `%USERPROFILE%\.cc-switch\cc-switch.db` —— 但那只是推测，**未实测**。
//   TODO(跨平台)：在真 Windows 上装一次 cc-switch，确认实际路径与表结构后
//   把本注释改写成实测结论；若与推测不同，改 ccSwitchDbPath 一处即可
//   （本文件不含任何路径假设）。
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

// 结果体装配的一行 wrapper（模板的 ListT 只在返回类型里，无法推导）。
CcSwitchImportRows unavailable(CcSwitchImportStatus status, std::string reason) {
    return quota::ccswitch_sqlite::makeUnavailable<CcSwitchImportRows>(
        status, std::move(reason));
}

// 导入只取 Claude Code 侧两类：DevDesk 管的是 Claude Code 的 settings.json。
// 同库的 codex / gemini / grokbuild 是别的产品，凭据形态不同，导进来必然坏。
constexpr char kAppTypeClaude[] = "claude";
constexpr char kAppTypeClaudeDesktop[] = "claude-desktop";

// busy timeout 见 quota/ccswitch_sqlite.h（kBusyTimeoutMs，两条读路径共用）。

// env 里被「抽成独立 Profile 字段」而**不进 extraEnvJson** 的四个键。
// 顺序无关（是集合），但逐条写出理由便于日后核对：
//   ANTHROPIC_BASE_URL     → baseUrl 列
//   ANTHROPIC_AUTH_TOKEN   → apiKey（与 API_KEY 二选一，TOKEN 优先）
//   ANTHROPIC_API_KEY      → apiKey（兜底）
//   ANTHROPIC_MODEL        → model 列
// 其余 env 键（含 CLAUDE_CODE_SUBAGENT_MODEL / ANTHROPIC_DEFAULT_*_MODEL 等）
// 全部落 extraEnvJson ——「导入全部的配置内容」靠的就是这一条。
// 例外（审计批 ②）：双 key 并存时，未用作 apiKey 的 ANTHROPIC_API_KEY 例外
// 保留进 extraEnvJson（不丢值，见 mapCcSwitchProviderToProfile 内注释）。
constexpr char kEnvBaseUrl[] = "ANTHROPIC_BASE_URL";
constexpr char kEnvAuthToken[] = "ANTHROPIC_AUTH_TOKEN";
constexpr char kEnvApiKey[] = "ANTHROPIC_API_KEY";
constexpr char kEnvModel[] = "ANTHROPIC_MODEL";

bool isReservedEnvKey(const std::string& key) {
    return key == kEnvBaseUrl || key == kEnvAuthToken || key == kEnvApiKey ||
           key == kEnvModel;
}

// ---- 读侧 ----
// sqlite 骨架（RAII / 探列 / columnText / sqliteError / 只读打开）来自
// quota/ccswitch_sqlite.h —— 2026-10-03 轻量化前本文件把它们从
// quota/ccswitch_source.cpp 逐字抄了一份（文件头却写着「不重写一份」）。

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

    // 缺密钥 → 确定性跳过（审计批 ①，2026-10-03）：与「缺 base_url → 跳过」
    // 同类——源条目缺必填字段，重试也不会好；「失败」留给可重试/系统错误
    //（DB 错 / UNIQUE 竞态）。不在这里拦，会落进 createProfile 的
    //「apiKey 不能为空」（profile_repository.cpp:379）失败桶，稀释失败信号。
    if (apiKey.empty()) {
        return skipped("缺密钥（env 无 ANTHROPIC_AUTH_TOKEN / ANTHROPIC_API_KEY）");
    }

    // authField 三态（值域同 claude_service.h / provider_config.h）：
    // 两个键都非空 → both；只有 TOKEN → bearer；只有 API_KEY → api_key。
    // （都无 → 上方「缺密钥」已整条跳过，到不了这里；初值 both 只作兜底。）
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
    // 双 key 并存：未用作 apiKey 的那个 key 保留进 extraEnvJson，不丢值
    //（审计批 ②，2026-10-03，M4）。TOKEN 优先 → apiKey 取 TOKEN 时，API_KEY
    // 的值原本整条丢（既是保留键不进 extraEnv、又不进 apiKey）。保留的运行期
    // 安全由既有守卫保证，不靠这里收窄：
    //   - 直连：buildDirectEnv 写选中密钥键时**清掉另一密钥键**
    //     （provider_config.cpp:717-732，test_claude_service.cpp:3500 钉死）
    //     ——直连恒单凭据；
    //   - 代理：UpstreamDialect::applyHeaders 无条件丢弃客户端凭据、只注入
    //     profile 的 key（upstream_dialect.cpp:142-155）——上游恒单凭据；
    //   - 展示：详情页 env 面板按键名掩码（键名含 token/key/secret 的值截断，
    //     claude_ui_detail.cpp）。
    // 只可能是 ANTHROPIC_API_KEY：TOKEN 非空时 apiKey 已取 TOKEN；apiKey 取
    // API_KEY 的前提是 TOKEN 为空，那时没有第二个值可保留。
    if (!authToken.empty() && !apiKeyEnv.empty()) {
        extraEnv[kEnvApiKey] = apiKeyEnv;
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