// src/module_claude_proxy/settings_sync.cpp
// 接管通道实现（协议见 settings_sync.h 文件头）
#include "settings_sync.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>

#include "claude_service.h"
#include "core/app_config.h"
#include "core/database.h"   // core::appDatabase()
#include "core/logging.h"
#include "core/paths.h"
#include "platform/json.h"
#include "provider_config.h"

namespace fs = std::filesystem;

namespace module::claude::settings_sync {

namespace {

using platform::json::JsonObject;
using platform::json::JsonValue;

bool fileExists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec) && !ec;
}

// 读文本文件（不存在 → 空串；失败 → 空串 + *readFailed=true，与空文件区分）
std::string readTextFile(const fs::path& p, bool* readFailed = nullptr) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        if (readFailed) *readFailed = true;
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (readFailed && f.bad()) *readFailed = true;  // 读中断（IO 错）同款上报
    return ss.str();
}

// 原子写：同目录 tmp → rename。MinGW rename 不覆盖已存在目标，先 remove 再
// rename（最后写者胜，窗口期可接受）
core::Result<void> atomicWrite(const fs::path& target,
                               const std::string& content) {
    std::error_code ec;
    const fs::path dir = target.parent_path();
    if (!dir.empty()) {
        fs::create_directories(dir, ec);
        if (ec) {
            return core::Result<void>::fail(
                core::ErrorCode::Internal,
                "目录创建失败 " + dir.string() + ": " + ec.message());
        }
    }
    fs::path tmp = target;
    tmp += ".devdesk-tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            return core::Result<void>::fail(core::ErrorCode::Internal,
                                            "临时文件写入失败: " + tmp.string());
        }
        f << content;
        f.close();
        if (!f) {
            std::error_code rmEc;
            fs::remove(tmp, rmEc);
            return core::Result<void>::fail(
                core::ErrorCode::Internal,
                "临时文件写入未完成: " + tmp.string());
        }
    }
    fs::remove(target, ec);  // 目标存在则先删（MinGW rename 不覆盖）
    fs::rename(tmp, target, ec);
    if (ec) {
        std::error_code rmEc;
        fs::remove(tmp, rmEc);
        return core::Result<void>::fail(
            core::ErrorCode::Internal,
            "原子替换失败 " + target.string() + ": " + ec.message());
    }
    return core::Result<void>::ok();
}

// 解析 JSON 文件为对象（不存在 → ok 空对象标记 exists=false；解析失败/非对象 → fail）
struct ParsedDoc {
    bool exists = false;
    bool ok = false;
    std::string error;
    JsonObject obj;
};

ParsedDoc parseJsonObjectFile(const fs::path& p) {
    ParsedDoc out;
    if (!fileExists(p)) {
        out.ok = true;  // 不存在 → 调用方按空对象处理
        return out;
    }
    out.exists = true;
    bool readFailed = false;
    const std::string text = readTextFile(p, &readFailed);
    if (readFailed) {
        // G8：存在但读不出（IO/权限）≠ 空文件，不静默当空对象重写
        out.error = "读取失败（IO/权限）: " + p.string();
        return out;
    }
    if (text.empty()) {
        out.ok = true;  // 0 字节空文件视同不存在（G9 no-op）
        return out;
    }
    auto parsed = platform::json::parse(text);
    if (parsed.isFail()) {
        out.error = parsed.errorMessage();
        return out;
    }
    if (!parsed.value().isObject()) {
        out.error = "顶层不是 JSON 对象";
        return out;
    }
    out.obj = parsed.value().asObject();
    out.ok = true;
    return out;
}

// sidecar 的 app_config 存取：语义与迁移前文件版逐条对齐——键不存在 →
// exists=false/ok=true；非合法 JSON 对象 → ok=false；db 缺失 → ok=true + WARN
bool sidecarDbMissing(const Paths& paths) {
    if (paths.db != nullptr) return false;
    LOG_WARN("settings_sync: 应用数据库未就绪，sidecar 快照不可用（"
             "接管状态按「无快照」处理，还原域可能被跳过）");
    return true;
}

bool sidecarExists(const Paths& paths) {
    if (sidecarDbMissing(paths)) return false;
    auto has = core::appconfig::has(*paths.db, kSidecarConfigKey);
    if (has.isFail()) {
        LOG_WARN("settings_sync: sidecar 存在性探测失败: " + has.errorMessage());
        return false;
    }
    return has.value();
}

// 读 sidecar。返回结构与 parseJsonObjectFile 同款（exists / ok / error / obj）
ParsedDoc parseSidecar(const Paths& paths) {
    ParsedDoc out;
    if (sidecarDbMissing(paths)) {
        out.ok = true;
        return out;
    }
    auto raw = core::appconfig::get(*paths.db, kSidecarConfigKey);
    if (raw.isFail()) {
        if (raw.errorCode() == core::ErrorCode::NotFound) {
            out.ok = true;   // 键不存在 → 调用方按空对象处理
            return out;
        }
        out.error = "读取 app_config 失败: " + raw.errorMessage();
        return out;
    }
    out.exists = true;
    if (raw.value().empty()) {
        out.ok = true;   // 空串视同不存在（对齐旧实现的 0 字节空文件口径）
        return out;
    }
    auto parsed = platform::json::parse(raw.value());
    if (parsed.isFail()) {
        out.error = parsed.errorMessage();
        return out;
    }
    if (!parsed.value().isObject()) {
        out.error = "顶层不是 JSON 对象";
        return out;
    }
    out.obj = parsed.value().asObject();
    out.ok = true;
    return out;
}

core::Result<void> writeSidecar(const Paths& paths,
                                const std::string& text) {
    if (paths.db == nullptr) {
        return core::Result<void>::fail(
            core::ErrorCode::NotFound,
            "应用数据库未就绪，sidecar 快照无法保存");
    }
    return core::appconfig::set(*paths.db, kSidecarConfigKey, text);
}

void removeSidecar(const Paths& paths) {
    if (sidecarDbMissing(paths)) return;
    auto erased = core::appconfig::erase(*paths.db, kSidecarConfigKey);
    if (erased.isFail()) {
        LOG_WARN("settings_sync: sidecar 删除失败（不影响还原结果）: " +
                 erased.errorMessage());
    }
}

// sidecar 结构：{"managedKeys": {<键名>: "", ...}, "envSnapshot": {...}}。
// managedKeys 以对象存键名集合（读侧统一走 keys()）是有意保留的形态
JsonObject managedKeysObject(const std::vector<std::string>& keys) {
    JsonObject obj;
    for (const auto& k : keys) {
        obj.set(k, platform::json::makeString(""));
    }
    return obj;
}

std::vector<std::string> managedKeysFromSidecar(const JsonObject& sidecar) {
    std::vector<std::string> out;
    if (sidecar.has("managedKeys") && sidecar.get("managedKeys").isObject()) {
        out = sidecar.get("managedKeys").asObject().keys();
    }
    return out;
}

// 本机代理形态的凭据键：值 == kProxyAuthPlaceholder 即「托管值」。必须命名空间
// 作用域 + constexpr——本表在退出链上（exit hook → … → findProxyFormKeys 读），
// 函数局部 static 惰性构造会因 atexit LIFO 先于 exit hook 析构（UAF）。
// 通用口径：exit hook 调用链上的表禁止「函数局部 + 动态初始化 static」。
constexpr std::string_view kProxyCredentialKeys[] = {
    "ANTHROPIC_AUTH_TOKEN",
    "ANTHROPIC_API_KEY",
};

// 找出 env 里「本机代理形态」的键（回环 BASE_URL / 占位凭据）：是上次接管残留
// 或别的工具写入的托管值，不是用户原值——不进首份快照，无快照时清除
std::vector<std::string> findProxyFormKeys(const JsonObject& env) {
    std::vector<std::string> out;
    if (env.has("ANTHROPIC_BASE_URL") &&
        env.get("ANTHROPIC_BASE_URL").isString() &&
        isLoopbackUrl(env.get("ANTHROPIC_BASE_URL").asString())) {
        out.push_back("ANTHROPIC_BASE_URL");
    }
    for (const std::string_view key : kProxyCredentialKeys) {
        // JsonObject::has/get 只收 const std::string&，显式构造一次
        const std::string name(key);
        if (env.has(name) && env.get(name).isString() &&
            env.get(name).asString() == kProxyAuthPlaceholder) {
            out.push_back(name);
        }
    }
    return out;
}

std::string joinKeys(const std::vector<std::string>& keys) {
    std::string out;
    for (const auto& k : keys) {
        if (!out.empty()) out += ", ";
        out += k;
    }
    return out;
}

// 无 sidecar 兜底：settings.json 仍留本机代理形态键 → 最小清除（只删这些键，
// 不按 active profile 重建 env）；env 无这些特征 → 真 no-op
SyncResult clearResidualProxyKeys(const Paths& paths) {
    SyncResult out;
    out.ok = true;
    if (!fileExists(paths.settingsFile)) return out;

    auto doc = parseJsonObjectFile(paths.settingsFile);
    if (!doc.ok) {
        out.ok = false;
        out.error = "settings.json 解析失败，跳过残留清理: " + doc.error;
        LOG_WARN("settings_sync: " + out.error);
        return out;
    }
    if (!doc.exists) return out;
    JsonObject root = std::move(doc.obj);
    if (!root.has("env") || !root.get("env").isObject()) return out;
    JsonObject env = root.get("env").asObject();

    const std::vector<std::string> residual = findProxyFormKeys(env);
    if (residual.empty()) return out;

    // 重建 env 对象实现删除（JsonObject 无 erase）
    JsonObject rebuilt;
    for (const auto& key : env.keys()) {
        if (std::find(residual.begin(), residual.end(), key) != residual.end()) {
            continue;
        }
        rebuilt.set(key, env.get(key));
    }
    root.set("env", JsonValue(rebuilt));

    auto text = platform::json::stringify(JsonValue(root), /*pretty=*/true);
    if (text.isFail()) {
        out.ok = false;
        out.error = "settings.json 序列化失败: " + text.errorMessage();
        LOG_WARN("settings_sync: " + out.error);
        return out;
    }
    auto w = atomicWrite(paths.settingsFile, text.value() + "\n");
    if (w.isFail()) {
        out.ok = false;
        out.error = "settings.json 写入失败: " + w.errorMessage();
        LOG_WARN("settings_sync: " + out.error);
        return out;
    }
    LOG_WARN("settings_sync: 无接管快照但 settings.json 仍是本机代理形态，"
             "已清除残留键（" + joinKeys(residual) + "）: " +
             paths.settingsFile.string());
    return out;
}

// 上层覆盖检测（只提示不代改）。检测集 = 托管键全集（managedEnvKeyNames()，
// 与槽位是否有值无关）∪ extraManaged（本轮实际写入的托管键并集）
std::vector<std::string> detectOverlayConflicts(
    const Paths& paths, const std::vector<std::string>& extraManaged) {
    std::vector<std::string> managed = managedEnvKeyNames();
    for (const auto& key : extraManaged) {
        if (std::find(managed.begin(), managed.end(), key) == managed.end()) {
            managed.push_back(key);  // 去重：两集天然重叠，重复会在命中清单里刷两遍
        }
    }
    std::vector<std::string> warnings;
    for (const auto& file : paths.overlayFiles) {
        if (!fileExists(file)) continue;
        auto doc = parseJsonObjectFile(file);
        if (!doc.ok || !doc.exists) continue;
        if (!doc.obj.has("env") || !doc.obj.get("env").isObject()) continue;
        auto env = doc.obj.get("env").asObject();
        std::string hits;
        for (const auto& key : managed) {
            if (env.has(key)) {
                if (!hits.empty()) hits += ", ";
                hits += key;
            }
        }
        if (!hits.empty()) {
            warnings.push_back("检测到 " + file.string() +
                               " 的 env 含 " + hits +
                               "，该层优先级更高，可能覆盖 DevDesk 接管");
        }
    }
    return warnings;
}

// 非 env 顶层键移除冲突检测：返回即将从 root 移除的键名——在 nonEnvKeys 并集
// 里（曾被接管写入）但本轮不写的键。live 值可能是用户手写的，移除会动到用户
// 可见键，故一律告警；键在并集但 root 没有 → 遍历不到，静默。已去重 + 排序
std::vector<std::string> detectNonEnvRemovalConflicts(
    const JsonObject& root, const std::vector<std::string>& nonEnvKeys,
    const std::vector<std::string>& pendingNonEnvKeys) {
    std::vector<std::string> hits;
    for (const auto& key : root.keys()) {
        if (key == "env") continue;   // env 块永不由 3.5 步移除
        const bool pendingThisRound =
            std::find(pendingNonEnvKeys.begin(), pendingNonEnvKeys.end(),
                      key) != pendingNonEnvKeys.end();
        if (pendingThisRound) continue;
        const bool managedBefore =
            std::find(nonEnvKeys.begin(), nonEnvKeys.end(), key) !=
            nonEnvKeys.end();
        if (!managedBefore) continue;   // 从未托管 = 用户自己的键，不碰
        hits.push_back(key);
    }
    std::sort(hits.begin(), hits.end());
    hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
    return hits;
}

} // namespace

Paths defaultPaths() {
    Paths p;
    p.settingsFile = core::paths::claudeSettingsFile();
    p.db = core::appDatabase();
    // managed（企业策略）+ project（当前工作目录）两层；user 层即 settingsFile
    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    p.overlayFiles = {
        core::paths::claudeManagedSettingsFile(),
        cwd / ".claude" / "settings.json",
        cwd / ".claude" / "settings.local.json",
    };
    return p;
}

SyncResult takeOver(const Profile& profile, int proxyPort, const Paths& paths) {
    SyncResult out;

    // 托管 env（纯函数，规则见 provider_config.h）；片段走 JSON 域，
    // 经 profile.extraSettingsJson 在下方 apply 步并入
    const auto managed = buildProxyEnv(profile.modelsConfigJson,
                                       profile.extraEnvJson, proxyPort);
    std::vector<std::string> managedKeys;
    managedKeys.reserve(managed.size());
    for (const auto& [k, v] : managed) managedKeys.push_back(k);

    // 本轮将写入的顶层键（best-effort 枚举，非法 → 空集；解析失败警告由
    // applyExtraSettingsToSettingsDoc 统一发，防双报）
    const std::vector<std::string> pendingNonEnvKeys =
        extraSettingsTopLevelKeys(profile.extraSettingsJson);

    // 1. 读现有 settings.json（解析失败 → 中止不写，宁可不让用户自动接入）
    auto doc = parseJsonObjectFile(paths.settingsFile);
    if (!doc.ok) {
        out.error = "settings.json 解析失败，中止接管（未写入）: " + doc.error;
        LOG_WARN("settings_sync: " + out.error);
        return out;
    }
    JsonObject root = std::move(doc.obj);

    // 现有 env 对象（无 / 非对象 → 空对象）
    JsonObject env;
    if (root.has("env") && root.get("env").isObject()) {
        env = root.get("env").asObject();
    }

    // 2. 首份快照（sidecar 已存在 → 保留原 envSnapshot 不覆盖，重拍会把已混入
    //    的托管值误当用户原值；sidecar 损坏 → 中止）
    JsonObject snapshotEnv = env;  // 首次接管：接管前的原 env
    std::vector<std::string> snapshotWarnings;  // 快照污染守卫命中（条目 2①）
    JsonObject priorNonEnvSnapshot;             // sidecar 已有 nonEnvSnapshot
    std::vector<std::string> priorNonEnvKeys;   // sidecar 已有 nonEnvKeys 并集
    if (sidecarExists(paths)) {
        auto sidecar = parseSidecar(paths);
        if (!sidecar.ok || !sidecar.exists) {
            out.error = "sidecar 快照损坏，中止接管（" +
                        std::string(kSidecarConfigKey) + " 键）: " + sidecar.error;
            LOG_WARN("settings_sync: " + out.error);
            return out;
        }
        for (const auto& k : managedKeysFromSidecar(sidecar.obj)) {
            managedKeys.push_back(k);  // 并集，稍后去重
        }
        // 沿用首份快照（缺失 envSnapshot 字段 → 空对象，按「全删」还原）
        snapshotEnv = JsonObject();
        if (sidecar.obj.has("envSnapshot") &&
            sidecar.obj.get("envSnapshot").isObject()) {
            snapshotEnv = sidecar.obj.get("envSnapshot").asObject();
        }
        // nonEnv 域既有记账（§5.2③：缺失字段 → 空集，读侧 .has() 守卫）
        if (sidecar.obj.has("nonEnvSnapshot") &&
            sidecar.obj.get("nonEnvSnapshot").isObject()) {
            priorNonEnvSnapshot = sidecar.obj.get("nonEnvSnapshot").asObject();
        }
        if (sidecar.obj.has("nonEnvKeys") &&
            sidecar.obj.get("nonEnvKeys").isObject()) {
            priorNonEnvKeys = sidecar.obj.get("nonEnvKeys").asObject().keys();
        }
    } else {
        // 首份快照污染守卫：本机代理形态键是残留/托管值不是用户原值，不进快照
        //（否则还原会把死代理地址固化成「用户配置」）
        const std::vector<std::string> polluted = findProxyFormKeys(snapshotEnv);
        if (!polluted.empty()) {
            JsonObject cleaned;
            for (const auto& key : snapshotEnv.keys()) {
                if (std::find(polluted.begin(), polluted.end(), key) !=
                    polluted.end()) {
                    continue;
                }
                cleaned.set(key, snapshotEnv.get(key));
            }
            snapshotEnv = std::move(cleaned);
            snapshotWarnings.push_back(
                "settings.json 的 env 已是本机代理形态（" + joinKeys(polluted) +
                "），这些键不计入接管快照（还原时不恢复）");
        }
    }
    std::sort(managedKeys.begin(), managedKeys.end());
    managedKeys.erase(std::unique(managedKeys.begin(), managedKeys.end()),
                      managedKeys.end());

    // nonEnv 域记账：快照政策刻意与 envSnapshot 不同构——envSnapshot 首拍兜
    // 整个对象，「不重拍」成立；nonEnv 只拍本轮写入键，已在并集里的键不重拍
    //（其 live 值必是托管值），新增键 live 有且快照无 → 补拍。"env" 自身
    // 防御性跳过（extraSettings 路径永不托管 "env"）
    {
        std::vector<std::string> prior = priorNonEnvKeys;
        std::sort(prior.begin(), prior.end());
        for (const auto& k : pendingNonEnvKeys) {
            if (k == "env") continue;
            const bool alreadyManaged =
                std::binary_search(prior.begin(), prior.end(), k);
            if (alreadyManaged) continue;   // 已托管：不重拍（F-1）
            if (root.has(k) && !priorNonEnvSnapshot.has(k)) {
                priorNonEnvSnapshot.set(k, root.get(k));
            }
        }
    }
    std::vector<std::string> nonEnvKeys = pendingNonEnvKeys;
    nonEnvKeys.insert(nonEnvKeys.end(), priorNonEnvKeys.begin(),
                      priorNonEnvKeys.end());
    std::sort(nonEnvKeys.begin(), nonEnvKeys.end());
    nonEnvKeys.erase(std::unique(nonEnvKeys.begin(), nonEnvKeys.end()),
                     nonEnvKeys.end());
    {
        // sidecar v2：增 nonEnvKeys + nonEnvSnapshot（旧 sidecar 无新字段 =
        // 读侧按空集）
        JsonObject sidecar;
        sidecar.set("managedKeys",
                    JsonValue(managedKeysObject(managedKeys)));
        sidecar.set("envSnapshot", JsonValue(snapshotEnv));
        sidecar.set("nonEnvKeys", JsonValue(managedKeysObject(nonEnvKeys)));
        sidecar.set("nonEnvSnapshot", JsonValue(priorNonEnvSnapshot));
        auto text = platform::json::stringify(JsonValue(sidecar));
        if (text.isFail()) {
            out.error = "sidecar 序列化失败: " + text.errorMessage();
            return out;
        }
        auto w = writeSidecar(paths, text.value());
        if (w.isFail()) {
            out.error = "sidecar 写入失败: " + w.errorMessage();
            LOG_WARN("settings_sync: " + out.error);
            return out;
        }
    }
    // 快照先落地、settings.json 后改：中间崩溃 → 补还原恢复的是相同内容，安全

    // 3. 先删「上一轮托管、本轮不再生成」的键：槽位清空后旧模型名不能留在 env
    //    里（仍在 managedKeys 并集内，还原语义不变）
    {
        JsonObject kept;
        for (const auto& key : env.keys()) {
            const bool generatedThisRound = managed.find(key) != managed.end();
            const bool staleManaged =
                !generatedThisRound &&
                std::find(managedKeys.begin(), managedKeys.end(), key) !=
                    managedKeys.end();
            if (staleManaged) continue;
            kept.set(key, env.get(key));
        }
        env = std::move(kept);
    }

    // 3.5 陈旧非 env 键删除：在 nonEnvKeys 并集里但本轮不写的键从 root 移除
    //（留着会把上一轮托管值固化成用户配置）。移除前先取冲突清单并入
    // out.warnings（用户手写的同名键被移除前必须可见）
    std::vector<std::string> nonEnvRemovalWarnings;
    {
        const std::vector<std::string> removed =
            detectNonEnvRemovalConflicts(root, nonEnvKeys, pendingNonEnvKeys);
        if (!removed.empty()) {
            nonEnvRemovalWarnings.push_back(
                "settings.json 顶层键 " + joinKeys(removed) +
                " 曾被 DevDesk 接管写入，本轮接管会移除 live 文件中的同名手写值"
                "（还原时按接管前快照恢复或删除）；如需保留该配置，请在 DevDesk 的"
                "高级配置 JSON 里改，不要直接手改 settings.json");
        }
    }
    {
        JsonObject kept;
        for (const auto& key : root.keys()) {
            const bool pendingThisRound =
                std::find(pendingNonEnvKeys.begin(), pendingNonEnvKeys.end(),
                          key) != pendingNonEnvKeys.end();
            if (key == "env" || pendingThisRound) {
                kept.set(key, root.get(key));
                continue;
            }
            const bool staleNonEnv =
                std::find(nonEnvKeys.begin(), nonEnvKeys.end(), key) !=
                nonEnvKeys.end();
            if (staleNonEnv) continue;
            kept.set(key, root.get(key));
        }
        root = std::move(kept);
    }

    // 4. 合并写入 env（托管键覆盖，用户 env 键保留）
    for (const auto& [k, v] : managed) {
        env.set(k, platform::json::makeString(v));
    }
    root.set("env", JsonValue(env));

    // 4.5 生效 JSON 并入顶层：非 env 属性赢过 live 既有同名键；"model" 非空
    // 触发 ANTHROPIC_MODEL 抑制（在 apply 步）
    {
        auto apply = applyExtraSettingsToSettingsDoc(root,
                                                     profile.extraSettingsJson);
        for (const auto& warn : apply.warnings) {
            out.warnings.push_back("高级设置 JSON: " + warn);
        }
    }

    // 5. 原子写
    auto text = platform::json::stringify(JsonValue(root), /*pretty=*/true);
    if (text.isFail()) {
        out.error = "settings.json 序列化失败: " + text.errorMessage();
        LOG_WARN("settings_sync: " + out.error);
        return out;
    }
    auto w = atomicWrite(paths.settingsFile, text.value() + "\n");
    if (w.isFail()) {
        out.error = "settings.json 写入失败: " + w.errorMessage();
        LOG_WARN("settings_sync: " + out.error);
        return out;
    }

    // 6. 上层覆盖检测 + 快照污染 + 非 env 移除冲突，三源同出口
    out.warnings = detectOverlayConflicts(paths, managedKeys);
    out.warnings.insert(out.warnings.end(), snapshotWarnings.begin(),
                        snapshotWarnings.end());
    out.warnings.insert(out.warnings.end(), nonEnvRemovalWarnings.begin(),
                        nonEnvRemovalWarnings.end());
    for (const auto& warn : out.warnings) {
        LOG_WARN("settings_sync: " + warn);
    }

    out.ok = true;
    return out;
}

SyncResult restore(const Paths& paths) {
    SyncResult out;
    if (!sidecarExists(paths)) {
        // 无快照 = 没接管过 → 通常 no-op；settings.json 仍留本机代理形态键时
        // 做最小清除
        return clearResidualProxyKeys(paths);
    }

    // 读 sidecar（损坏 → 报错并保留，供人工恢复）
    auto sidecar = parseSidecar(paths);
    if (!sidecar.ok || !sidecar.exists) {
        out.error = "sidecar 快照损坏，跳过还原（保留文件供人工检查）: " +
                    sidecar.error;
        LOG_WARN("settings_sync: " + out.error);
        return out;
    }
    const std::vector<std::string> managedKeys =
        managedKeysFromSidecar(sidecar.obj);
    JsonObject snapshotEnv;
    if (sidecar.obj.has("envSnapshot") &&
        sidecar.obj.get("envSnapshot").isObject()) {
        snapshotEnv = sidecar.obj.get("envSnapshot").asObject();
    }
    // nonEnv 域记账读取：旧 sidecar 无新字段 → 空集 = root 层全透传
    std::vector<std::string> nonEnvKeys;
    if (sidecar.obj.has("nonEnvKeys") &&
        sidecar.obj.get("nonEnvKeys").isObject()) {
        nonEnvKeys = sidecar.obj.get("nonEnvKeys").asObject().keys();
    }
    JsonObject nonEnvSnapshot;
    if (sidecar.obj.has("nonEnvSnapshot") &&
        sidecar.obj.get("nonEnvSnapshot").isObject()) {
        nonEnvSnapshot = sidecar.obj.get("nonEnvSnapshot").asObject();
    }

    // 读 settings.json（解析失败 → 报错并保留 sidecar）
    if (!fileExists(paths.settingsFile)) {
        out.error = "settings.json 不存在，无法还原（sidecar 保留）";
        LOG_WARN("settings_sync: " + out.error);
        return out;
    }
    auto doc = parseJsonObjectFile(paths.settingsFile);
    if (!doc.ok) {
        out.error = "settings.json 解析失败，跳过还原（sidecar 保留）: " +
                    doc.error;
        LOG_WARN("settings_sync: " + out.error);
        return out;
    }
    JsonObject root = std::move(doc.obj);

    // root 层重建：nonEnvKeys 里的键不透传当前值——快照有 → 恢复原值，无 →
    // 删除。"env" 自身防御性跳过（防病态 sidecar 把整个 env 块删掉）
    {
        JsonObject rebuiltRoot;
        for (const auto& key : root.keys()) {
            if (key == "env") {
                rebuiltRoot.set(key, root.get(key));
                continue;
            }
            if (std::find(nonEnvKeys.begin(), nonEnvKeys.end(), key) !=
                nonEnvKeys.end()) {
                continue;   // 托管非 env 键统一走快照恢复，不透传当前值
            }
            rebuiltRoot.set(key, root.get(key));
        }
        for (const auto& key : nonEnvKeys) {
            if (key == "env") continue;
            if (nonEnvSnapshot.has(key)) {
                rebuiltRoot.set(key, nonEnvSnapshot.get(key));   // 原值恢复
            }   // 快照里没有 → 原本不存在 → 不进 rebuiltRoot（即删除）
        }
        root = std::move(rebuiltRoot);
    }

    JsonObject env = root.has("env") && root.get("env").isObject()
        ? root.get("env").asObject() : JsonObject();

    // 按键级还原：快照有值 → 恢复；原本不存在 → 删除；其余不动
    JsonObject rebuilt;
    for (const auto& key : env.keys()) {
        if (std::find(managedKeys.begin(), managedKeys.end(), key) !=
            managedKeys.end()) {
            continue;  // 托管键统一走快照恢复，不透传当前值
        }
        rebuilt.set(key, env.get(key));
    }
    for (const auto& key : managedKeys) {
        if (snapshotEnv.has(key)) {
            rebuilt.set(key, snapshotEnv.get(key));  // 原值恢复
        }  // 快照里没有 → 原本不存在 → 不进 rebuilt（即删除）
    }
    // env 全空且接管前没有 env 块 → 不留空 env 键；否则写回
    root.set("env", JsonValue(rebuilt));

    auto text = platform::json::stringify(JsonValue(root), /*pretty=*/true);
    if (text.isFail()) {
        out.error = "settings.json 序列化失败: " + text.errorMessage();
        return out;
    }
    auto w = atomicWrite(paths.settingsFile, text.value() + "\n");
    if (w.isFail()) {
        out.error = "settings.json 写入失败: " + w.errorMessage();
        LOG_WARN("settings_sync: " + out.error);
        return out;
    }

    // 还原成功才删 sidecar
    removeSidecar(paths);
    out.ok = true;
    return out;
}

bool hasPendingSnapshot(const Paths& paths) {
    return sidecarExists(paths);
}

SyncResult recoverPendingSnapshot(const Paths& paths) {
    if (!hasPendingSnapshot(paths)) {
        return SyncResult{/*.ok=*/true, {}, {}};
    }
    LOG_INFO("settings_sync: 检测到上次未完成的接管还原（sidecar 残留），"
             "自动补还原");
    return restore(paths);
}

// 反向同步读侧（协议见 settings_sync.h 的 SettingsEnvRead 注释）
SettingsEnvRead readSettingsEnvForSync(const Paths& paths) {
    SettingsEnvRead out;
    if (paths.settingsFile.empty()) {
        return out;  // 未注入路径（§3.6）：no-op
    }

    auto doc = parseJsonObjectFile(paths.settingsFile);
    if (!doc.ok) {  // G8：读取/解析失败 → 调用方中止切换
        out.ok = false;
        out.error = "settings.json 读取或解析失败（" +
                    paths.settingsFile.string() + "）: " + doc.error;
        return out;
    }
    // G7/G9：不存在 / 空文件 / 无 env 键 → 无真相源，no-op 成功
    if (!doc.exists || !doc.obj.has("env")) {
        return out;
    }

    auto envVal = doc.obj.get("env");
    if (!envVal.isObject()) {  // G8：env 非对象 → 中止切换
        out.ok = false;
        out.error = "settings.json 的 env 字段不是 JSON 对象（" +
                    paths.settingsFile.string() + "）";
        return out;
    }
    auto text = platform::json::stringify(envVal);
    if (text.isFail()) {
        out.ok = false;
        out.error = "env 序列化失败（" + paths.settingsFile.string() +
                    "）: " + text.errorMessage();
        return out;
    }
    out.envJson = text.value();
    out.present = true;

    // sidecar 托管键（反向同步的 G1 输入）：缺失 → 空集；损坏 → 空集 + WARN
    if (sidecarExists(paths)) {
        auto sidecar = parseSidecar(paths);
        if (sidecar.ok && sidecar.exists) {
            out.managedKeys = managedKeysFromSidecar(sidecar.obj);
        } else {
            LOG_WARN("settings_sync: sidecar 快照损坏，反向同步按无托管键处理（" +
                     std::string(kSidecarConfigKey) + " 键）" +
                     (sidecar.error.empty() ? "" : ": " + sidecar.error));
        }
    }
    return out;
}

} // namespace module::claude::settings_sync
