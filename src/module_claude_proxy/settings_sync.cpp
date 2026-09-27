// src/module_claude_proxy/settings_sync.cpp
// 接管通道实现（协议见 settings_sync.h 文件头）
#include "settings_sync.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

#include "claude_service.h"
#include "core/logging.h"
#include "core/paths.h"
#include "platform/json.h"
#include "provider_config.h"

namespace fs = std::filesystem;

namespace module::claude::settings_sync {

namespace {

using platform::json::JsonObject;
using platform::json::JsonValue;

// 覆盖检测关注的 env 键（§5.3：上层文件含这些键会静默压过用户级接管）
const std::vector<std::string>& conflictingEnvKeys() {
    static const std::vector<std::string> kKeys = {
        "ANTHROPIC_BASE_URL",
        "ANTHROPIC_AUTH_TOKEN",
        "ANTHROPIC_MODEL",
        "ANTHROPIC_DEFAULT_SONNET_MODEL",
        "ANTHROPIC_DEFAULT_OPUS_MODEL",
        "ANTHROPIC_DEFAULT_HAIKU_MODEL",
        "ANTHROPIC_DEFAULT_FABLE_MODEL",
        "CLAUDE_CODE_SUBAGENT_MODEL",
    };
    return kKeys;
}

bool fileExists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec) && !ec;
}

// 读文本文件（不存在 → 空串；失败 → 空串，调用方按解析失败处理）
std::string readTextFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// 原子写：同目录 tmp → rename。MinGW 的 std::filesystem::rename 不覆盖已存在
// 目标（底层 _wrename 语义），所以先 remove 再 rename（设计 §5.3 开放问题 6：
// 最后写者胜，窗口期可接受）
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
    const std::string text = readTextFile(p);
    if (text.empty()) {
        out.ok = true;  // 空文件视同不存在
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

// sidecar 结构：{"managedKeys": {<键名>: "", ...}, "envSnapshot": {...}}。
// managedKeys 以对象存键名集合（读侧统一走 keys()）是有意保留的形态，不必改成数组。
// 原注"platform::json 不支持读数组元素（size() 恒 0）"已过时：size()/operator[] 早已实装
//（见 "platform/json.h"，模块插拔 enabledModules 是首个消费者）。
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

// 上层覆盖检测（§5.3）：只提示不代改
std::vector<std::string> detectOverlayConflicts(const Paths& paths) {
    std::vector<std::string> warnings;
    for (const auto& file : paths.overlayFiles) {
        if (!fileExists(file)) continue;
        auto doc = parseJsonObjectFile(file);
        if (!doc.ok || !doc.exists) continue;
        if (!doc.obj.has("env") || !doc.obj.get("env").isObject()) continue;
        auto env = doc.obj.get("env").asObject();
        std::string hits;
        for (const auto& key : conflictingEnvKeys()) {
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
    p.sidecarFile = core::paths::appDataDir() / "claude_settings_snapshot.json";
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
    const auto managed = buildProxyEnv(profile.modelsConfigJson, profile.model,
                                       profile.extraEnvJson, proxyPort);
    std::vector<std::string> managedKeys;
    managedKeys.reserve(managed.size());
    for (const auto& [k, v] : managed) managedKeys.push_back(k);

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

    // 2. 首份快照（sidecar 已存在 → 保留原 envSnapshot 不覆盖——当前 env 里
    //    已混入上次托管值，重拍会把托管值误当用户原值；仅并入新托管键。
    //    sidecar 存在但损坏 → 中止，同理）
    JsonObject snapshotEnv = env;  // 首次接管：接管前的原 env
    if (fileExists(paths.sidecarFile)) {
        auto sidecar = parseJsonObjectFile(paths.sidecarFile);
        if (!sidecar.ok || !sidecar.exists) {
            out.error = "sidecar 快照损坏，中止接管（请人工检查 " +
                        paths.sidecarFile.string() + "）";
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
    }
    std::sort(managedKeys.begin(), managedKeys.end());
    managedKeys.erase(std::unique(managedKeys.begin(), managedKeys.end()),
                      managedKeys.end());
    {
        JsonObject sidecar;
        sidecar.set("managedKeys",
                    JsonValue(managedKeysObject(managedKeys)));
        sidecar.set("envSnapshot", JsonValue(snapshotEnv));
        auto text = platform::json::stringify(JsonValue(sidecar));
        if (text.isFail()) {
            out.error = "sidecar 序列化失败: " + text.errorMessage();
            return out;
        }
        auto w = atomicWrite(paths.sidecarFile, text.value());
        if (w.isFail()) {
            out.error = "sidecar 写入失败: " + w.errorMessage();
            LOG_WARN("settings_sync: " + out.error);
            return out;
        }
    }
    // 快照先落地、settings.json 后改：中间崩溃 → 补还原恢复的是相同内容，安全

    // 3. 先删「上一轮托管、本轮不再生成」的键（条目 1）：槽位被清空后旧模型名
    //    不能继续留在 env 里被客户端拿去请求新供应商；这些键仍在 managedKeys
    //    并集内，还原语义不变（重建对象实现删除，JsonObject 无 erase）
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

    // 4. 合并写入 env（托管键覆盖，用户 env 键保留），非 env 顶层字段不动
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
    auto w = atomicWrite(paths.settingsFile, text.value() + "\n");
    if (w.isFail()) {
        out.error = "settings.json 写入失败: " + w.errorMessage();
        LOG_WARN("settings_sync: " + out.error);
        return out;
    }

    // 6. 上层覆盖检测（只警告不代改）
    out.warnings = detectOverlayConflicts(paths);
    for (const auto& warn : out.warnings) {
        LOG_WARN("settings_sync: " + warn);
    }

    out.ok = true;
    return out;
}

SyncResult restore(const Paths& paths) {
    SyncResult out;
    if (!fileExists(paths.sidecarFile)) {
        out.ok = true;  // 无快照 = 没接管过，no-op
        return out;
    }

    // 读 sidecar（损坏 → 报错并保留，供人工恢复）
    auto sidecar = parseJsonObjectFile(paths.sidecarFile);
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
    JsonObject env = root.has("env") && root.get("env").isObject()
        ? root.get("env").asObject() : JsonObject();

    // 按键级还原：快照有值 → 恢复；原本不存在 → 删除；其余（用户 env 键 +
    // 非 env 字段）不动。JsonObject 无 erase，重建 env 对象实现删除
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
    //（重建对象已天然剔除应删除的托管键）
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
    std::error_code ec;
    fs::remove(paths.sidecarFile, ec);
    if (ec) {
        LOG_WARN("settings_sync: sidecar 删除失败（不影响还原结果）: " +
                 ec.message());
    }
    out.ok = true;
    return out;
}

bool hasPendingSnapshot(const Paths& paths) {
    return fileExists(paths.sidecarFile);
}

SyncResult recoverPendingSnapshot(const Paths& paths) {
    if (!hasPendingSnapshot(paths)) {
        return SyncResult{/*.ok=*/true, {}, {}};
    }
    LOG_INFO("settings_sync: 检测到上次未完成的接管还原（sidecar 残留），"
             "自动补还原");
    return restore(paths);
}

// 反向同步读侧（协议见 settings_sync.h 的 SettingsEnvRead 注释）。
// 复用本文件既有 readTextFile / parseJsonObjectFile / managedKeysFromSidecar。
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
    // G7/G9：不存在（doc.exists=false）/ 0 字节空文件（视同不存在）/ 无 env 键
    // → 无真相源，no-op 成功。注：readTextFile 对「存在但读不出」（IO/权限）
    // 也返回空串，该形态下与空文件同处置（复用既有 IO 助手，不新增读取通道）
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
    if (fileExists(paths.sidecarFile)) {
        auto sidecar = parseJsonObjectFile(paths.sidecarFile);
        if (sidecar.ok && sidecar.exists) {
            out.managedKeys = managedKeysFromSidecar(sidecar.obj);
        } else {
            LOG_WARN("settings_sync: sidecar 快照损坏，反向同步按无托管键处理: " +
                     paths.sidecarFile.string() +
                     (sidecar.error.empty() ? "" : ": " + sidecar.error));
        }
    }
    return out;
}

} // namespace module::claude::settings_sync
