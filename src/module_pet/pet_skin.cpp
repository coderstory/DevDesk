// src/module_pet/pet_skin.cpp
// 皮肤解析 + 内置皮肤目录发现。
// robot/ghost 为随包静态资产（assets/pets/，2026-09-28 起随仓库分发），
// 运行期不再程序生成 PNG（生成器与自研 PNG 编码器已删）。
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

// ============ 解析（纯函数）============

namespace {

// 审计 [28]：帧名会直接拼进皮肤目录（PetSkin::framePath = dir / 帧名），恶意
// 皮肤包可用 "../.." 或绝对路径加载皮肤目录外的任意 PNG（有限任意文件读）。
// 只放行相对文件名：'/'、'\\'（兼收 Unix/Windows 绝对路径与 UNC）、':'（盘符
// 前缀 "C:"、ADS 一律拒）任一出现即拒；".." 子串即拒（穿越片段；单个 '.' 是
// PNG 扩展名必须放行）；空串由上游非空校验兜底
bool isSafeFrameName(const std::string& name) {
    if (name.find_first_of("/\\:") != std::string::npos) {
        return false;
    }
    return name.find("..") == std::string::npos;
}

// 提取字符串数组（成员必须全是非空字符串；否则失败）。asFrameNames=true 时
// 帧名过 isSafeFrameName（states.idle/poked/sleep/typing 用）；false 供
// phrases 等纯文本数组复用——短语是展示文本不是路径，不收字符集限制
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
        if (w >= 16 && w <= 512 && h >= 16 && h <= 512) {
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
    // 可选状态/字段（M2）：sleep/typing 帧缺失或非法一律静默忽略——
    // 状态机各自回落 idle，老皮肤包零迁移（poked 同款容错）
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

// ============ 默认皮肤（随包 robot）============

Result<PetSkin> ensureDefaultSkin() {
    // 默认皮肤 = 随包 robot（kDefaultSkinName = "robot"，assets/pets/robot 静态
    // 资产，CMake POST_BUILD 拷到可执行文件旁）。候选根按 skinRootCandidates
    // 顺序逐个尝试：petsDir 优先（旧版程序生成物/用户替换的 robot 在数据根）→
    // 可执行文件旁 assets/pets（随包）。任一候选加载成功即返回；全部失败 =
    // 返回 fail（生成器已删，不再写盘生成）
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
    // 皮肤根**纯派生**（审计 CP-04）：数据根 → 安装目录，两条都在此列齐，
    // 这里不读任何环境变量。旧实现在 DEVDESK_PETS_DIR 已设时直接丢掉安装
    // 目录候选——环境变量因此参与了**产品行为**（隔离环境下"看不见"随包
    // 皮肤），生产代码长期留着 getenv 分支；测试隔离改由数据根统一承担
    // （DEVDESK_DATA_ROOT / DEVDESK_PETS_DIR 只搬 petsDir()，不再改候选形态）。
    //
    //   1) petsDir()（appData，恒可写）：换肤写盘处（旧版生成物/用户替换）
    //   2) exe 旁 assets/pets：随包分发的只读美术皮肤（安装包/DMG 里与可执行
    //      文件同级；开发期 = 构建目录，由顶层 CMake POST_BUILD 拷贝）
    std::vector<std::filesystem::path> roots;
    roots.push_back(core::paths::petsDir());
    // exe 目录解析失败时 executableDir() 返回空 path，此时必须跳过该候选：
    // 空路径与 "assets/pets" 拼接会退化成相对 cwd 的路径
    const std::filesystem::path exeDir = core::paths::executableDir();
    if (!exeDir.empty()) {
        roots.push_back(exeDir / "assets" / "pets");
    }
    return roots;
}

std::vector<std::string> discoverSkins() {
    // 皮肤名合法字符与 PetConfigStore::sanitized 一致（会拼回路径）
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