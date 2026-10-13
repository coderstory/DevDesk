// 皮肤解析 + 内置皮肤目录发现；机器人/幽灵为随包静态资产，运行期不程序生成 PNG。
#include "module_pet/pet_skin.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "core/paths.h"
// 桥函数契约（discoverSkins 定义在本 TU）：签名漂移由该头末尾 static_assert 拦
#include "module_pet_bridge.h"
#include "platform/json.h"

namespace module::pet {

using core::ErrorCode;
using core::Result;

namespace {

// 帧名会拼进皮肤目录，恶意皮肤包可用 "../.." 或绝对路径读皮肤目录外任意 PNG。
// 只放行相对文件名：'/'、'\\'、':' 或 ".." 子串任一出现即拒（单个 '.' 是扩展名，放行）。
bool isSafeFrameName(const std::string& name) {
    if (name.find_first_of("/\\:") != std::string::npos) {
        return false;
    }
    return name.find("..") == std::string::npos;
}

// 提取字符串数组（成员须全为非空字符串）。asFrameNames=true 时帧名过 isSafeFrameName。
Result<std::vector<std::string>> parseFrameArray(const platform::json::JsonValue& value,
                                                 bool asFrameNames) {
    if (!value.isArray()) {
        return Result<std::vector<std::string>>::fail(ErrorCode::ValidationError,
                                                      "states.* 必须是字符串数组");
    }
    std::vector<std::string> frames;
    for (int i = 0; i < value.size(); ++i) {
        if (!value[i].isString() || value[i].asString().empty()) {
            return Result<std::vector<std::string>>::fail(
                ErrorCode::ValidationError, "帧名必须是非空字符串");
        }
        const std::string& name = value[i].asString();
        if (asFrameNames && !isSafeFrameName(name)) {
            return Result<std::vector<std::string>>::fail(
                ErrorCode::ValidationError,
                "帧名只许相对文件名（含 / \\ : 或 .. 被拒）: " + name);
        }
        frames.push_back(name);
    }
    if (frames.empty()) {
        return Result<std::vector<std::string>>::fail(ErrorCode::ValidationError,
                                                      "帧数组不能为空");
    }
    return Result<std::vector<std::string>>::ok(std::move(frames));
}

} // namespace

Result<PetSkin> parsePetSkinJson(const std::string& jsonText) {
    auto parsed = platform::json::parse(jsonText);
    if (parsed.isFail() || !parsed.value().isObject()) {
        return Result<PetSkin>::fail(ErrorCode::JsonParseError,
                                     "pet.json 必须是 JSON object");
    }
    const platform::json::JsonObject obj = parsed.value().asObject();

    PetSkin skin;
    if (obj.has("name") && obj.get("name").isString()) {
        skin.name = obj.get("name").asString();
    }
    if (obj.has("frameRate") && obj.get("frameRate").isNumber()) {
        const double rate = obj.get("frameRate").asDouble();
        if (rate >= 1.0 && rate <= 30.0) skin.frameRate = rate;
    }
    if (obj.has("frameSize") && obj.get("frameSize").isArray() &&
        obj.get("frameSize").size() == 2 &&
        obj.get("frameSize")[0].isNumber() && obj.get("frameSize")[1].isNumber()) {
        const int w = static_cast<int>(obj.get("frameSize")[0].asInt());
        const int h = static_cast<int>(obj.get("frameSize")[1].asInt());
        // 界常量 = kMinFrameSize/kMaxFrameSize（与显示大小缩放共用）
        if (w >= kMinFrameSize && w <= kMaxFrameSize && h >= kMinFrameSize &&
            h <= kMaxFrameSize) {
            skin.frameWidth = w;
            skin.frameHeight = h;
        }
    }
    if (obj.has("license") && obj.get("license").isString()) {
        skin.license = obj.get("license").asString();
    }
    if (obj.has("source") && obj.get("source").isString()) {
        skin.source = obj.get("source").asString();
    }
    if (!obj.has("states") || !obj.get("states").isObject()) {
        return Result<PetSkin>::fail(ErrorCode::ValidationError,
                                     "pet.json 缺少 states 对象");
    }
    const platform::json::JsonObject states = obj.get("states").asObject();
    if (!states.has("idle")) {
        return Result<PetSkin>::fail(ErrorCode::ValidationError,
                                     "pet.json 缺少 states.idle");
    }
    auto idle = parseFrameArray(states.get("idle"), /*asFrameNames=*/true);
    if (idle.isFail()) {
        return Result<PetSkin>::fail(idle.errorCode(), "states.idle: " + idle.errorMessage());
    }
    skin.idleFrames = std::move(idle.value());
    // sleep/typing 帧缺失或非法一律静默忽略（状态机各自回落 idle）。
    if (states.has("poked")) {
        auto poked = parseFrameArray(states.get("poked"), /*asFrameNames=*/true);
        if (poked.isOk()) {
            skin.pokedFrames = std::move(poked.value());
        }
    }
    if (states.has("sleep")) {
        auto sleep = parseFrameArray(states.get("sleep"), /*asFrameNames=*/true);
        if (sleep.isOk()) {
            skin.sleepFrames = std::move(sleep.value());
        }
    }
    if (states.has("typing")) {
        auto typing = parseFrameArray(states.get("typing"), /*asFrameNames=*/true);
        if (typing.isOk()) {
            skin.typingFrames = std::move(typing.value());
        }
    }
    if (obj.has("phrases")) {
        auto phrases = parseFrameArray(obj.get("phrases"), /*asFrameNames=*/false);
        if (phrases.isOk()) {
            skin.phrases = std::move(phrases.value());
        }
    }
    return Result<PetSkin>::ok(std::move(skin));
}

Result<PetSkin> loadSkin(const std::filesystem::path& dir) {
    const std::filesystem::path jsonPath = dir / "pet.json";
    std::error_code ec;
    if (!std::filesystem::exists(jsonPath, ec) || ec) {
        return Result<PetSkin>::fail(ErrorCode::FileNotFound,
                                     "皮肤不存在: " + jsonPath.string());
    }
    std::ifstream file(jsonPath);
    if (!file) {
        return Result<PetSkin>::fail(ErrorCode::FileReadFailed,
                                     "皮肤读取失败: " + jsonPath.string());
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    file.close();
    auto skin = parsePetSkinJson(buffer.str());
    if (skin.isFail()) {
        return skin;
    }
    skin.value().dir = dir;
    if (skin.value().name.empty()) {
        skin.value().name = dir.filename().string();
    }
    return skin;
}

Result<PetSkin> ensureDefaultSkin() {
    // 默认皮肤 = 随包 robot。候选根按 skinRootCandidates 顺序逐个尝试，任一成功即返回。
    for (const std::filesystem::path& root : skinRootCandidates()) {
        auto skin = loadSkin(root / kDefaultSkinName);
        if (skin.isOk()) {
            return skin;
        }
    }
    return Result<PetSkin>::fail(ErrorCode::FileNotFound,
                                 "默认皮肤 \"" + std::string(kDefaultSkinName) +
                                     "\" 不可用（随包资产缺失或损坏）");
}

std::vector<std::filesystem::path> skinRootCandidates() {
    // 皮肤根纯派生（不读环境变量）：1) petsDir()（appData，恒可写，换肤写盘处）；
    // 2) exe 旁 assets/pets（随包分发的只读美术皮肤）。
    std::vector<std::filesystem::path> roots;
    roots.push_back(core::paths::petsDir());
    // exe 目录解析失败返回空 path，必须跳过该候选：空路径拼接会退化成相对 cwd。
    const std::filesystem::path exeDir = core::paths::executableDir();
    if (!exeDir.empty()) {
        roots.push_back(exeDir / "assets" / "pets");
    }
    return roots;
}

std::vector<std::string> discoverSkins() {
    // 皮肤名合法字符与 PetConfigStore::sanitized 一致（会拼回路径）。
    static const std::string kValid =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
    std::vector<std::string> names;
    for (const std::filesystem::path& root : skinRootCandidates()) {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec) || ec) continue;
        for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (!it->is_directory(ec) || ec) continue;
            const std::string name = it->path().filename().string();
            if (name.empty() || name.find_first_not_of(kValid) != std::string::npos) {
                continue;  // 非法名不进清单（路径安全）
            }
            std::error_code ec2;
            if (!std::filesystem::exists(it->path() / "pet.json", ec2) || ec2) continue;
            if (std::find(names.begin(), names.end(), name) == names.end()) {
                names.push_back(name);
            }
        }
    }
    return names;
}

} // namespace module::pet
