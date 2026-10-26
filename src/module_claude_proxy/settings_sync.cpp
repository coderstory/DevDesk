// src/module_claude_proxy/settings_sync.cpp
// 接管通道实现（协议见 settings_sync.h 文件头）
#include "settings_sync.h"

#include <algorithm>
#include <string_view>
#include <system_error>

#include "claude_service.h"
#include "core/app_config.h"
#include "core/atomic_write.h"
#include "core/database.h"   // core::appDatabase()
#include "core/fs_text.h"
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
    const std::string text = core::readTextFile(p, &readFailed);
    if (readFailed) {
        // 存在但读不出（IO/权限）≠ 空文件，不静默当空对象重写
        out.error = "读取失败（IO/权限）: " + p.string();
        return out;
    }
    if (text.empty()) {
        out.ok = true;  // 0 字节空文件视同不存在（no-op）
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

// sidecar 的 app_config 存取：键不存在 → exists=false/ok=true；非合法
// JSON 对象 → ok=false；db 缺失 → ok=true + WARN
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
        out.ok = true;   // 空串视同不存在
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

// 本机代理形态的凭据键：值 == kProxyAuthPlaceholder 即「托管值」。必须命名空间作用域 + constexpr：函数局部 static 惰性构造会因 atexit LIFO 先于 exit hook 析构（UAF）
constexpr std::string_view kProxyCredentialKeys[] = {
    "ANTHROPIC_AUTH_TOKEN",
    "ANTHROPIC_API_KEY",
};

// 找出 env 里「本机代理形态」的键（回环 BASE_URL / 占位凭据）：托管值不是用户原值——不进首份快照，无快照时清除
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
    auto w = core::atomicWriteFile(paths.settingsFile, text.value() + "\n", "settings.json");
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

// 上层覆盖检测（只提示不代改）：检测集 = 托管键全集 ∪ extraManaged（本轮实际写入的托管键并集）
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

    // 托管 env（纯函数，规则见 provider_config.h）
    const auto managed = buildProxyEnv(profile.modelsConfigJson,
                                       profile.extraEnvJson, proxyPort);
    std::vector<std::string> managedKeys;
    managedKeys.reserve(managed.size());
    for (const auto& [k, v] : managed) managedKeys.push_back(k);

    // 接管不再写非 env 顶层键；仅保留旧 sidecar 的 nonEnv 记账用于还原
    const std::vector<std::string> pendingNonEnvKeys;

    // 1. 读现有 settings.json（解析失败 → 中止不写，宁可不让用户自动接入）
    auto doc = parseJsonObjectFile(paths.settingsFile);
    if (!doc.ok) {
        out.error = "settings.json 解析失败，中止接管（未写入）: " + doc.error;
        LOG_WARN("settings_sync: " + out.error);
        return out;
    }
    JsonObject root = std::move(doc.obj);

    JsonObject env;
    if (root.has("env") && root.get("env").isObject()) {
        env = root.get("env").asObject();
    }

    // 2. 首份快照（sidecar 已存在 → 保留原 envSnapshot 不覆盖，重拍会把已混入的托管值误当用户原值；sidecar 损坏 → 中止）
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
        // nonEnv 域既有记账（缺失字段 → 空集，读侧 .has() 守卫）
        if (sidecar.obj.has("nonEnvSnapshot") &&
            sidecar.obj.get("nonEnvSnapshot").isObject()) {
            priorNonEnvSnapshot = sidecar.obj.get("nonEnvSnapshot").asObject();
        }
        if (sidecar.obj.has("nonEnvKeys") &&
            sidecar.obj.get("nonEnvKeys").isObject()) {
            priorNonEnvKeys = sidecar.obj.get("nonEnvKeys").asObject().keys();
        }
    } else {
        // 首份快照污染守卫：本机代理形态键是托管值不是用户原值，不进快照（否则还原会把死代理地址固化成「用户配置」）
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

    std::vector<std::string> nonEnvKeys = pendingNonEnvKeys;
    nonEnvKeys.insert(nonEnvKeys.end(), priorNonEnvKeys.begin(),
                      priorNonEnvKeys.end());
    std::sort(nonEnvKeys.begin(), nonEnvKeys.end());
    nonEnvKeys.erase(std::unique(nonEnvKeys.begin(), nonEnvKeys.end()),
                     nonEnvKeys.end());
    {
        // sidecar 结构含 nonEnvKeys + nonEnvSnapshot（缺失字段 = 读侧按空集）
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

    // 3. 先删「上一轮托管、本轮不再生成」的键：槽位清空后旧模型名不能留在 env 里（仍在 managedKeys 并集内，还原语义不变）
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

    // 3.5 陈旧非 env 键删除：旧 sidecar 记录过、本轮不再写的键从 root 移除（留着会把托管值固化成用户配置）
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

    // 5. 原子写
    auto text = platform::json::stringify(JsonValue(root), /*pretty=*/true);
    if (text.isFail()) {
        out.error = "settings.json 序列化失败: " + text.errorMessage();
        LOG_WARN("settings_sync: " + out.error);
        return out;
    }
    auto w = core::atomicWriteFile(paths.settingsFile, text.value() + "\n", "settings.json");
    if (w.isFail()) {
        out.error = "settings.json 写入失败: " + w.errorMessage();
        LOG_WARN("settings_sync: " + out.error);
        return out;
    }

    // 6. 上层覆盖检测 + 快照污染，两源同出口
    out.warnings = detectOverlayConflicts(paths, managedKeys);
    out.warnings.insert(out.warnings.end(), snapshotWarnings.begin(),
                        snapshotWarnings.end());
    for (const auto& warn : out.warnings) {
        LOG_WARN("settings_sync: " + warn);
    }

    out.ok = true;
    return out;
}

SyncResult restore(const Paths& paths) {
    SyncResult out;
    if (!sidecarExists(paths)) {
        // 无快照 = 没接管过 → 通常 no-op；settings.json 仍留本机代理形态键时做最小清除
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

    // root 层重建：nonEnvKeys 里的键不透传当前值——快照有 → 恢复原值，无 → 删除；"env" 防御性跳过（防病态 sidecar 删掉整个 env 块）
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
    auto w = core::atomicWriteFile(paths.settingsFile, text.value() + "\n", "settings.json");
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
        return out;  // 未注入路径：no-op
    }

    auto doc = parseJsonObjectFile(paths.settingsFile);
    if (!doc.ok) {  // 读取/解析失败 → 调用方中止切换
        out.ok = false;
        out.error = "settings.json 读取或解析失败（" +
                    paths.settingsFile.string() + "）: " + doc.error;
        return out;
    }
    // 不存在 / 空文件 / 无 env 键 → 无真相源，no-op 成功
    if (!doc.exists || !doc.obj.has("env")) {
        return out;
    }

    auto envVal = doc.obj.get("env");
    if (!envVal.isObject()) {  // env 非对象 → 中止切换
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

    // sidecar 托管键（反向同步输入）：缺失 → 空集；损坏 → 空集 + WARN
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
