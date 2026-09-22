// tests/test_modules/test_pet.cpp
// 桌宠 MVP（2026-09-22-desktop-pet-design.md §6 M1）单测：
//   1. 皮肤 pet.json 解析（纯函数）：合法/缺字段/坏帧数组/数值钳制/poked 可选
//   2. 帧序列状态机（纯函数）：idle 循环 / poke 反应播一遍回 idle / 无 poked 帧皮肤
//   3. 占位皮肤生成：PNG 魔数 + IHDR 尺寸 + pet.json 可解析 + 幂等
//   4. 设置门控：pet 是已知模块 ID（可持久化）但默认关闭；pet 字段往返
//   5. startPetWindow 无头冒烟：openWindow 入队的 DslWindowRequest 逐字段断言
//     （透明/无边框/置顶/不抢焦点/不进任务栏/位置钳制/未知皮肤回退）
// 子窗口真实渲染/拖拽/穿透属人工点验（设计 §6 M1 验收项）。
#include "doctest.h"

#include "module_pet/pet_frame_player.h"
#include "module_pet/pet_skin.h"
#include "module_pet/pet_window.h"
#include "settings_config.h"

#include <eui/app.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// app:: 实现来自 test_settings.cpp 的 dsl_app_impl.h 注入（单一定义点）
namespace app {
void compose(eui::Ui&, const eui::Screen&);
}

namespace {

std::filesystem::path tempDir(const char* name) {
    return std::filesystem::temp_directory_path() /
           ("devdesk_test_pet_" + std::string(name));
}

void removeTreeQuietly(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
}

void setEnvVar(const char* name, const std::string& value) {
#if defined(_WIN32)
    ::_putenv_s(name, value.c_str());
#else
    if (value.empty()) {
        ::unsetenv(name);
    } else {
        ::setenv(name, value.c_str(), 1);
    }
#endif
}

// PNG 结构断言：魔数 + IHDR 宽高（不做完整解码，解码属 EUI-NEO stb 路径）
bool pngLooksValid(const std::filesystem::path& path, int width, int height) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    unsigned char header[33] = {};
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!file) return false;
    static const unsigned char kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    for (int i = 0; i < 8; ++i) {
        if (header[i] != kSignature[i]) return false;
    }
    // 8 魔数 + 4 长度 + 4 "IHDR" + 4 宽 + 4 高 + ...
    const auto be32 = [](const unsigned char* p) {
        return (static_cast<std::uint32_t>(p[0]) << 24) |
               (static_cast<std::uint32_t>(p[1]) << 16) |
               (static_cast<std::uint32_t>(p[2]) << 8) |
               static_cast<std::uint32_t>(p[3]);
    };
    if (be32(header + 8) != 13) return false;               // IHDR 长度
    if (header[12] != 'I' || header[13] != 'H' || header[14] != 'D' || header[15] != 'R') return false;
    if (static_cast<int>(be32(header + 16)) != width) return false;
    if (static_cast<int>(be32(header + 20)) != height) return false;
    if (header[24] != 8 || header[25] != 6) return false;   // 8bit RGBA
    return true;
}

const char* kValidSkinJson =
    "{"
    "  \"name\": \"clay-cat\","
    "  \"frameSize\": [128, 128],"
    "  \"frameRate\": 4,"
    "  \"license\": \"CC0\","
    "  \"source\": \"test\","
    "  \"states\": {"
    "    \"idle\": [\"f0.png\", \"f1.png\", \"f2.png\"],"
    "    \"poked\": [\"p0.png\", \"p1.png\"]"
    "  }"
    "}";

} // namespace

// ============ 1. 皮肤 JSON 解析（纯函数）============

REGISTER_TEST(pet_skin_parse_valid, "pet: pet.json 合法解析", {
    auto r = module::pet::parsePetSkinJson(kValidSkinJson);
    DOCTEST_CHECK(r.isOk());
    const module::pet::PetSkin& skin = r.unsafeValue();
    DOCTEST_CHECK_EQ(skin.name, "clay-cat");
    DOCTEST_CHECK_EQ(skin.frameWidth, 128);
    DOCTEST_CHECK_EQ(skin.frameHeight, 128);
    DOCTEST_CHECK_EQ(skin.frameRate, 4.0);
    DOCTEST_CHECK_EQ(skin.idleFrames.size(), static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(skin.idleFrames[0], "f0.png");
    DOCTEST_CHECK_EQ(skin.pokedFrames.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(skin.license, "CC0");
    // 帧路径拼接（dir 由 loadSkin 填）
    const module::pet::PetSkin withDir = [&] {
        module::pet::PetSkin s = skin;
        s.dir = std::filesystem::path("C:/skins/clay-cat");
        return s;
    }();
    DOCTEST_CHECK_EQ(withDir.framePath(false, 0).generic_string(),
                     "C:/skins/clay-cat/f0.png");
    return true;
});

REGISTER_TEST(pet_skin_parse_missing_states, "pet: pet.json 缺 states/idle 报错", {
    DOCTEST_CHECK(module::pet::parsePetSkinJson("{\"name\":\"x\"}").isFail());
    DOCTEST_CHECK(
        module::pet::parsePetSkinJson("{\"states\":{\"poked\":[\"p.png\"]}}").isFail());
    DOCTEST_CHECK(
        module::pet::parsePetSkinJson("{\"states\":{\"idle\":[]}}").isFail());
    DOCTEST_CHECK(
        module::pet::parsePetSkinJson("{\"states\":{\"idle\":[\"\"]}}").isFail());
    DOCTEST_CHECK(
        module::pet::parsePetSkinJson("{\"states\":{\"idle\":[42]}}").isFail());
    DOCTEST_CHECK(module::pet::parsePetSkinJson("not json").isFail());
    return true;
});

REGISTER_TEST(pet_skin_parse_clamps, "pet: frameRate/frameSize 越界收敛默认", {
    // frameRate 超范围 → 保持默认 4；frameSize 越界 → 保持默认 128
    const std::string json =
        "{\"frameRate\": 999, \"frameSize\": [8, 9999],"
        " \"states\": {\"idle\": [\"f.png\"]}}";
    auto r = module::pet::parsePetSkinJson(json);
    DOCTEST_CHECK(r.isOk());
    DOCTEST_CHECK_EQ(r.unsafeValue().frameRate, 4.0);
    DOCTEST_CHECK_EQ(r.unsafeValue().frameWidth, 128);
    DOCTEST_CHECK_EQ(r.unsafeValue().frameHeight, 128);
    // poked 可选：缺失 → pokedFrames 空，frames(true) 回落 idle
    DOCTEST_CHECK(r.unsafeValue().pokedFrames.empty());
    DOCTEST_CHECK_EQ(r.unsafeValue().frames(true).size(), static_cast<size_t>(1));
    return true;
});

// ============ 2. 帧序列状态机（纯函数）============

REGISTER_TEST(pet_anim_idle_cycle, "pet: idle 帧循环推进", {
    auto r = module::pet::parsePetSkinJson(kValidSkinJson);
    DOCTEST_CHECK(r.isOk());
    module::pet::PetAnimation anim;
    DOCTEST_CHECK(anim.state == module::pet::PetAnimState::Idle);
    DOCTEST_CHECK_EQ(anim.currentPath(r.unsafeValue()).filename().string(), "f0.png");
    anim.tick(r.unsafeValue());
    DOCTEST_CHECK_EQ(anim.frame, 1);
    anim.tick(r.unsafeValue());
    anim.tick(r.unsafeValue());
    DOCTEST_CHECK_EQ(anim.frame, 0);  // 3 帧循环回卷
    return true;
});

REGISTER_TEST(pet_anim_poke_once, "pet: poke 反应帧播一遍回 Idle", {
    auto r = module::pet::parsePetSkinJson(kValidSkinJson);
    DOCTEST_CHECK(r.isOk());
    const module::pet::PetSkin& skin = r.unsafeValue();
    module::pet::PetAnimation anim;
    anim.poke(skin);
    DOCTEST_CHECK(anim.state == module::pet::PetAnimState::Poked);
    DOCTEST_CHECK_EQ(anim.currentPath(skin).filename().string(), "p0.png");
    anim.tick(skin);  // poked 第 2 帧
    DOCTEST_CHECK_EQ(anim.currentPath(skin).filename().string(), "p1.png");
    DOCTEST_CHECK(anim.state == module::pet::PetAnimState::Poked);
    anim.tick(skin);  // poked 播完（2 帧）→ 回 Idle
    DOCTEST_CHECK(anim.state == module::pet::PetAnimState::Idle);
    DOCTEST_CHECK_EQ(anim.frame, 0);
    DOCTEST_CHECK_EQ(anim.currentPath(skin).filename().string(), "f0.png");
    // Idle 中重复 poke 才响应；Poked 中 poke 不重置
    return true;
});

REGISTER_TEST(pet_anim_no_poked_frames, "pet: 无 poked 帧皮肤 poke 无反应", {
    auto r = module::pet::parsePetSkinJson(
        "{\"states\": {\"idle\": [\"f.png\", \"g.png\"]}}");
    DOCTEST_CHECK(r.isOk());
    module::pet::PetAnimation anim;
    anim.poke(r.unsafeValue());
    DOCTEST_CHECK(anim.state == module::pet::PetAnimState::Idle);
    return true;
});

// ============ 3. 占位皮肤生成 ============

REGISTER_TEST(pet_default_skin_generate, "pet: 占位皮肤生成（PNG 结构 + 幂等）", {
    const std::filesystem::path dir = tempDir("skin");
    removeTreeQuietly(dir);
    auto r = module::pet::ensureDefaultSkin(dir);
    DOCTEST_CHECK_MESSAGE(r.isOk(), r.isFail() ? r.errorMessage() : "");
    const module::pet::PetSkin& skin = r.unsafeValue();
    DOCTEST_CHECK_EQ(skin.frameWidth, 128);
    DOCTEST_CHECK_EQ(skin.frameHeight, 128);
    DOCTEST_CHECK_EQ(skin.frameRate, 4.0);
    DOCTEST_CHECK(skin.idleFrames.size() >= 3);
    DOCTEST_CHECK(skin.pokedFrames.size() >= 2);
    // idle + poked 全部帧都是结构合法的 RGBA PNG
    for (size_t i = 0; i < skin.idleFrames.size(); ++i) {
        DOCTEST_CHECK(pngLooksValid(skin.framePath(false, i), 128, 128));
    }
    for (size_t i = 0; i < skin.pokedFrames.size(); ++i) {
        DOCTEST_CHECK(pngLooksValid(skin.framePath(true, i), 128, 128));
    }
    // 幂等：已存在直接加载（帧数不变，不重画）
    auto again = module::pet::ensureDefaultSkin(dir);
    DOCTEST_CHECK(again.isOk());
    DOCTEST_CHECK_EQ(again.unsafeValue().idleFrames.size(), skin.idleFrames.size());
    removeTreeQuietly(dir);
    return true;
});

// ============ 4. 设置门控 + pet 字段往返 ============

REGISTER_TEST(pet_gating_defaults, "pet: 默认关闭、已知 ID 可持久化", {
    using namespace module::settings;
    SettingsConfig cfg;
    // 默认不含 pet（氛围功能默认关，设计 §4.1）
    DOCTEST_CHECK(!moduleEnabled(cfg, kModuleIdPet));
    DOCTEST_CHECK(std::find(cfg.enabledModules.begin(), cfg.enabledModules.end(),
                            std::string(kModuleIdPet)) == cfg.enabledModules.end());
    // 手动加入 → sanitized 保留（pet 是已知 ID）→ 开启
    cfg.enabledModules.push_back(kModuleIdPet);
    const SettingsConfig fixed = SettingsStore::sanitized(cfg);
    DOCTEST_CHECK(moduleEnabled(fixed, kModuleIdPet));
    // 默认五模块数量不变（既有 gating 测试的基线）
    DOCTEST_CHECK_EQ(SettingsConfig{}.enabledModules.size(), static_cast<size_t>(5));
    return true;
});

REGISTER_TEST(pet_settings_roundtrip, "pet: petSkin/petX/petY/petClickThrough 往返", {
    using namespace module::settings;
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "devdesk_test_pet_config.json";
    std::error_code ec;
    std::filesystem::remove(file, ec);
    SettingsStore store(file);
    SettingsConfig cfg;
    cfg.petSkin = "my-cat";
    cfg.petX = 1600;
    cfg.petY = 800;
    cfg.petClickThrough = true;
    DOCTEST_CHECK(store.save(cfg).isOk());
    const SettingsConfig loaded = store.load();
    DOCTEST_CHECK_EQ(loaded.petSkin, "my-cat");
    DOCTEST_CHECK_EQ(loaded.petX, 1600);
    DOCTEST_CHECK_EQ(loaded.petY, 800);
    DOCTEST_CHECK(loaded.petClickThrough);
    // 皮肤名白名单：路径分隔符/空 → 收敛默认 clay-cat；坐标钳制
    cfg.petSkin = "../evil";
    cfg.petX = 999999;
    const SettingsConfig sanitizedCfg = SettingsStore::sanitized(cfg);
    DOCTEST_CHECK_EQ(sanitizedCfg.petSkin, "clay-cat");
    DOCTEST_CHECK_EQ(sanitizedCfg.petX, 32767);
    std::filesystem::remove(file, ec);
    return true;
});

// ============ 5. startPetWindow 无头冒烟 ============

REGISTER_TEST(pet_start_window_request, "pet: 开窗请求逐字段断言（无头）", {
    const std::filesystem::path petsRoot = tempDir("petsroot");
    removeTreeQuietly(petsRoot);
    setEnvVar("DEVDESK_PETS_DIR", petsRoot.string());
    // 清空可能残留的窗口请求队列
    (void)app::consumeWindowRequests();

    module::pet::startPetWindow(module::pet::PetWindowConfig{
        "", 50, 60, false});  // 皮肤空 = 默认；位置已知
    std::vector<app::DslWindowRequest> requests = app::consumeWindowRequests();
    DOCTEST_CHECK_EQ(requests.size(), static_cast<size_t>(1));
    const app::DslWindowRequest& req = requests[0];
    DOCTEST_CHECK_EQ(req.pageId, "pet");
    DOCTEST_CHECK_EQ(req.width, 128);   // G2：128×128 不再被放大
    DOCTEST_CHECK_EQ(req.height, 128);
    DOCTEST_CHECK(req.positionSet);
    DOCTEST_CHECK_EQ(req.x, 50);
    DOCTEST_CHECK_EQ(req.y, 60);
    DOCTEST_CHECK(!req.decorated);            // 无边框
    DOCTEST_CHECK(req.alwaysOnTop);           // 置顶
    DOCTEST_CHECK(!req.resizable);
    DOCTEST_CHECK(!req.focusOnShow);          // 不抢焦点
    DOCTEST_CHECK(req.hideFromTaskbar);       // 不进任务栏/Alt+Tab
    DOCTEST_CHECK(!req.mousePassthrough);     // 默认不穿透
    DOCTEST_CHECK(!req.followClearColorOverride);  // 全透底不被主题广播冲掉
    DOCTEST_CHECK(req.clearColor.a == 0.0f);  // 全透
    DOCTEST_CHECK(static_cast<bool>(req.compose));
    // 占位皮肤已生成在测试隔离目录
    DOCTEST_CHECK(std::filesystem::exists(petsRoot / "clay-cat" / "pet.json"));

    // 未知皮肤名 → 回退默认皮肤，窗口照常入队
    module::pet::startPetWindow(module::pet::PetWindowConfig{"no-such-skin", -1, -1, true});
    std::vector<app::DslWindowRequest> second = app::consumeWindowRequests();
    DOCTEST_CHECK_EQ(second.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(second[0].mousePassthrough);   // 本次 clickThrough=true
    DOCTEST_CHECK(second[0].width == 128);       // 默认皮肤 128×128

    setEnvVar("DEVDESK_PETS_DIR", "");
    removeTreeQuietly(petsRoot);
    return true;
});
