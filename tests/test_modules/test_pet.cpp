// tests/test_modules/test_pet.cpp
// 桌宠 MVP + M2（2026-09-22-desktop-pet-design.md §6 M1/M2）单测：
//   1. 皮肤 pet.json 解析（纯函数）：合法/缺字段/坏帧数组/数值钳制/poked 可选
//   2. 帧序列状态机（纯函数）：idle 循环 / poke 反应播一遍回 idle / 无 poked 帧皮肤
//   3. 占位皮肤生成：PNG 魔数 + IHDR 尺寸 + pet.json 可解析 + 幂等
//   4. 设置门控：pet 是已知模块 ID（可持久化）但默认关闭；pet 字段往返
//   5. startPetWindow 无头冒烟：openWindow 入队的 DslWindowRequest 逐字段断言
//     （透明/无边框/置顶/不抢焦点/不进任务栏/位置钳制/未知皮肤回退）
//   6. M2 四态状态机（纯函数）：sleep 入睡/唤醒 / typing 流式联动（经
//      core::proxy_stats 测试时钟）/ typing 帧回落 / 状态优先级
//   7. M2 气泡：超时消失纯函数 + 短语表（皮肤语料/内置回退）
//   8. M2 多皮肤：sleep/typing/phrases 解析、占位皮肤 M2 帧升级重生成、
//      discoverSkins 发现消费、applyPetSkin/resetPetPosition 持久化行为
//   9. 内置皮肤全集（duck/slime/robot/ghost）：四态帧齐全、五款互异、
//      discoverSkins 发现、短语表区分、用户替换保留
//  10. 内置美术皮肤 anime-girl（真实 CC0 素材包）：四态帧齐全 + 全帧
//      128×128 RGBA PNG + license 自带（素材纪律 R7）
//  11. 审计修复（2026-09-23 pet 簇）：帧名路径穿越拒绝 [28] + 已知坐标
//      钳进工作区纯函数 [12]（[0][1] 的 onWindowDestroyed 接线断言在 §5）
// 子窗口真实渲染/拖拽/穿透/气泡视觉属人工点验（设计 §6 M2 验收项）。
#include "doctest.h"

#include "module_pet/pet_bubble.h"
#include "module_pet/pet_config.h"
#include "module_pet/pet_frame_player.h"
#include "module_pet/pet_skin.h"
#include "module_pet/pet_window.h"
#include "settings_config.h"

#include "core/proxy_stats.h"

#include <eui/app.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// app:: 实现来自 src/app_sidebar.cpp（compose 真实现直接编进测试二进制）
// + test_settings.cpp 的 dsl_app_impl.h 注入（dslAppConfig 等其余符号）
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

// M2 四态齐全的皮肤（sleep/typing/phrases）
const char* kM2SkinJson =
    "{"
    "  \"name\": \"m2-cat\","
    "  \"frameSize\": [128, 128],"
    "  \"frameRate\": 4,"
    "  \"phrases\": [\"你好呀\", \"测试短语\"],"
    "  \"states\": {"
    "    \"idle\": [\"f0.png\", \"f1.png\", \"f2.png\"],"
    "    \"poked\": [\"p0.png\", \"p1.png\"],"
    "    \"sleep\": [\"s0.png\", \"s1.png\", \"s2.png\"],"
    "    \"typing\": [\"t0.png\", \"t1.png\"]"
    "  }"
    "}";

// 全部帧 PNG 结构有效（REGISTER_TEST 宏体内不能有裸逗号，也不能套断言
// 宏——DOCTEST_CHECK 展开含 return，进不了 lambda；辅助函数绕开两者）
bool allFramesPngValid(const std::filesystem::path& dir,
                       const std::vector<std::string>& frames) {
    for (const std::string& frame : frames) {
        if (!pngLooksValid(dir / frame, 128, 128)) return false;
    }
    return true;
}

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

REGISTER_TEST(pet_settings_roundtrip, "pet: pet_config.json 读写往返 + 钳制", {
    using namespace module::pet;
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "devdesk_test_pet_config.json";
    std::error_code ec;
    std::filesystem::remove(file, ec);
    PetConfigStore store(file);
    PetConfig cfg;
    cfg.skinName = "my-cat";
    cfg.x = 1600;
    cfg.y = 800;
    cfg.clickThrough = true;
    DOCTEST_CHECK(store.save(cfg).isOk());
    const PetConfig loaded = store.load();
    DOCTEST_CHECK_EQ(loaded.skinName, "my-cat");
    DOCTEST_CHECK_EQ(loaded.x, 1600);
    DOCTEST_CHECK_EQ(loaded.y, 800);
    DOCTEST_CHECK(loaded.clickThrough);
    // 皮肤名白名单：路径分隔符/空 → 收敛默认 clay-cat；坐标钳制保留 -1 哨兵
    cfg.skinName = "../evil";
    cfg.x = 999999;
    const PetConfig sanitizedCfg = PetConfigStore::sanitized(cfg);
    DOCTEST_CHECK_EQ(sanitizedCfg.skinName, "clay-cat");
    DOCTEST_CHECK_EQ(sanitizedCfg.x, 32767);
    DOCTEST_CHECK_EQ(PetConfigStore::sanitized(PetConfig{}).x, -1);
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
    DOCTEST_CHECK(req.transparentFramebuffer);  // 逐像素透明（黑底修复：不随
                                                // 全局窗口效果档位回落 None）
    DOCTEST_CHECK(!req.mousePassthrough);     // 默认不穿透
    DOCTEST_CHECK(!req.followClearColorOverride);  // 全透底不被主题广播冲掉
    DOCTEST_CHECK(req.clearColor.a == 0.0f);  // 全透
    DOCTEST_CHECK(static_cast<bool>(req.compose));
    // 审计 [0][1]：创建/销毁回调都须随请求下传（销毁回调置空缓存句柄，
    // Alt+F4/菜单隐藏/应用退出所有路径覆盖）
    DOCTEST_CHECK(static_cast<bool>(req.onWindowCreated));
    DOCTEST_CHECK(static_cast<bool>(req.onWindowDestroyed));
    // 占位皮肤已生成在测试隔离目录
    DOCTEST_CHECK(std::filesystem::exists(petsRoot / "clay-cat" / "pet.json"));

    // 未知皮肤名 → 回退默认皮肤，窗口照常入队
    module::pet::startPetWindow(module::pet::PetWindowConfig{"no-such-skin", -1, -1, true});
    std::vector<app::DslWindowRequest> second = app::consumeWindowRequests();
    DOCTEST_CHECK_EQ(second.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(second[0].mousePassthrough);   // 本次 clickThrough=true
    DOCTEST_CHECK(second[0].width == 128);       // 默认皮肤 128×128

    // 单边 -1 哨兵：只重置未设置的那一边，已知坐标不丢
    //（无头环境无工作区 → 未设置边落到兜底值 100）
    module::pet::startPetWindow(module::pet::PetWindowConfig{"", 77, -1, false});
    std::vector<app::DslWindowRequest> third = app::consumeWindowRequests();
    DOCTEST_CHECK_EQ(third.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(third[0].x, 77);   // 已知的 x 原样保留
    DOCTEST_CHECK_EQ(third[0].y, 100);  // 未设置的 y 走兜底

    setEnvVar("DEVDESK_PETS_DIR", "");
    removeTreeQuietly(petsRoot);
    return true;
});

// ============ 6. M2 四态状态机（纯函数）============

REGISTER_TEST(pet_anim_sleep_cycle, "pet: 无交互超时入睡 + 交互唤醒", {
    using namespace module::pet;
    auto r = parsePetSkinJson(kM2SkinJson);
    DOCTEST_CHECK(r.isOk());
    const PetSkin& skin = r.unsafeValue();
    PetAnimation anim;
    // 未到阈值不入睡
    anim.considerSleep(kSleepAfterSeconds - 1.0, skin);
    DOCTEST_CHECK(anim.state == PetAnimState::Idle);
    // 到阈值 → Sleep，播 sleep 帧循环
    anim.considerSleep(kSleepAfterSeconds, skin);
    DOCTEST_CHECK(anim.state == PetAnimState::Sleep);
    DOCTEST_CHECK_EQ(anim.currentPath(skin).filename().string(), "s0.png");
    anim.tick(skin);
    anim.tick(skin);
    DOCTEST_CHECK_EQ(anim.currentPath(skin).filename().string(), "s2.png");
    anim.tick(skin);  // sleep 循环回卷，不退出
    DOCTEST_CHECK(anim.state == PetAnimState::Sleep);
    DOCTEST_CHECK_EQ(anim.currentPath(skin).filename().string(), "s0.png");
    // 交互唤醒 → Idle（先醒，再点才戳）
    anim.poke(skin);
    DOCTEST_CHECK(anim.state == PetAnimState::Idle);
    DOCTEST_CHECK_EQ(anim.currentPath(skin).filename().string(), "f0.png");
    // 无 sleep 帧的皮肤永不入睡（睡了也看不出来）
    const PetSkin noSleepSkin = parsePetSkinJson(kValidSkinJson).unsafeValue();
    PetAnimation noSleepAnim;
    noSleepAnim.considerSleep(kSleepAfterSeconds * 10.0, noSleepSkin);
    DOCTEST_CHECK(noSleepAnim.state == PetAnimState::Idle);
    return true;
});

REGISTER_TEST(pet_anim_typing, "pet: typing 态进出 + 帧回落 + 优先级", {
    using namespace module::pet;
    auto r = parsePetSkinJson(kM2SkinJson);
    DOCTEST_CHECK(r.isOk());
    const PetSkin& skin = r.unsafeValue();
    PetAnimation anim;
    anim.setStreaming(true, skin);
    DOCTEST_CHECK(anim.state == PetAnimState::Typing);
    DOCTEST_CHECK_EQ(anim.currentPath(skin).filename().string(), "t0.png");
    anim.tick(skin);
    DOCTEST_CHECK_EQ(anim.currentPath(skin).filename().string(), "t1.png");
    anim.tick(skin);  // typing 循环不退出
    DOCTEST_CHECK(anim.state == PetAnimState::Typing);
    // 流结束回 Idle
    anim.setStreaming(false, skin);
    DOCTEST_CHECK(anim.state == PetAnimState::Idle);
    // typing 中点击不重置（poke 只在 Idle 生效）
    anim.setStreaming(true, skin);
    anim.poke(skin);
    DOCTEST_CHECK(anim.state == PetAnimState::Typing);
    // 流式优先于入睡：Sleep 中来流 → Typing（AI 干活时宠物不睡）
    PetAnimation sleeper;
    sleeper.considerSleep(kSleepAfterSeconds, skin);
    DOCTEST_CHECK(sleeper.state == PetAnimState::Sleep);
    sleeper.setStreaming(true, skin);
    DOCTEST_CHECK(sleeper.state == PetAnimState::Typing);
    // 无 typing 帧的皮肤：状态语义仍在，视觉回落 idle 帧
    const PetSkin plainSkin = parsePetSkinJson(kValidSkinJson).unsafeValue();
    PetAnimation plain;
    plain.setStreaming(true, plainSkin);
    DOCTEST_CHECK(plain.state == PetAnimState::Typing);
    DOCTEST_CHECK_EQ(plain.currentPath(plainSkin).filename().string(), "f0.png");
    return true;
});

// 代理联动全链路（pet 状态机吃 core::proxy_stats 快照；测试时钟注入）
REGISTER_TEST(pet_anim_proxy_stats_linkage, "pet: proxy_stats 活动流驱动 typing", {
    using namespace module::pet;
    core::proxy_stats::resetForTest();
    int64_t nowMs = 1000;
    core::proxy_stats::setTestClock([&nowMs] { return nowMs; });
    auto r = parsePetSkinJson(kM2SkinJson);
    DOCTEST_CHECK(r.isOk());
    const PetSkin& skin = r.unsafeValue();
    // 模拟 UI 层 onTimer：读快照 activeStreams → setStreaming
    auto activeStreams = [] {
        return core::proxy_stats::snapshot().activeStreams;
    };
    PetAnimation anim;
    DOCTEST_CHECK_EQ(activeStreams(), 0);  // 无流
    anim.setStreaming(activeStreams() > 0, skin);
    DOCTEST_CHECK(anim.state == PetAnimState::Idle);
    const uint64_t stream = core::proxy_stats::beginStream();
    DOCTEST_CHECK_EQ(activeStreams(), 1);
    anim.setStreaming(activeStreams() > 0, skin);
    DOCTEST_CHECK(anim.state == PetAnimState::Typing);
    // 流式输出中保持 typing（usage 事件不改变状态）
    nowMs += 500;
    core::proxy_stats::updateStream(stream, 120);
    anim.setStreaming(activeStreams() > 0, skin);
    anim.tick(skin);
    DOCTEST_CHECK(anim.state == PetAnimState::Typing);
    // 流结束 → 回 Idle
    core::proxy_stats::endStream(stream);
    anim.setStreaming(activeStreams() > 0, skin);
    DOCTEST_CHECK(anim.state == PetAnimState::Idle);
    core::proxy_stats::resetForTest();
    return true;
});

// ============ 7. M2 气泡 ============

REGISTER_TEST(pet_bubble_timeout, "pet: 气泡超时消失 + 空短语不显示", {
    using namespace module::pet;
    PetBubble bubble;
    DOCTEST_CHECK(!bubble.visible);
    bubble.show("喵~", 3.0);
    DOCTEST_CHECK(bubble.visible);
    DOCTEST_CHECK_EQ(bubble.text, "喵~");
    bubble.tick(1.0);
    DOCTEST_CHECK(bubble.visible);  // 还剩 2s
    bubble.tick(2.5);               // 超时
    DOCTEST_CHECK(!bubble.visible);
    DOCTEST_CHECK(bubble.text.empty());
    bubble.tick(1.0);  // 隐藏后 tick 无操作（不复活）
    DOCTEST_CHECK(!bubble.visible);
    bubble.show("", 3.0);  // 空短语不显示
    DOCTEST_CHECK(!bubble.visible);
    return true;
});

REGISTER_TEST(pet_phrase_table, "pet: 短语表皮肤语料优先 + 内置回退", {
    using namespace module::pet;
    auto r = parsePetSkinJson(kM2SkinJson);
    DOCTEST_CHECK(r.isOk());
    DOCTEST_CHECK_EQ(r.unsafeValue().phrases.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(phraseTable(r.unsafeValue()).size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(phraseTable(r.unsafeValue())[0], "你好呀");
    // 无 phrases 皮肤 → 内置语料（非空，换肤可换性格）
    auto plain = parsePetSkinJson(kValidSkinJson);
    DOCTEST_CHECK(plain.unsafeValue().phrases.empty());
    DOCTEST_CHECK(!phraseTable(plain.unsafeValue()).empty());
    // 非法 phrases 数组（非字符串成员）静默忽略 → 回退内置
    auto bad = parsePetSkinJson(
        "{\"phrases\":[42],\"states\":{\"idle\":[\"f.png\"]}}");
    DOCTEST_CHECK(bad.isOk());
    DOCTEST_CHECK(bad.unsafeValue().phrases.empty());
    return true;
});

// ============ 8. M2 多皮肤 ============

REGISTER_TEST(pet_skin_parse_m2_states, "pet: pet.json sleep/typing 帧解析", {
    using namespace module::pet;
    auto r = parsePetSkinJson(kM2SkinJson);
    DOCTEST_CHECK(r.isOk());
    const PetSkin& skin = r.unsafeValue();
    DOCTEST_CHECK_EQ(skin.sleepFrames.size(), static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(skin.typingFrames.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(skin.sleepFrames[2], "s2.png");
    DOCTEST_CHECK_EQ(skin.typingFrames[0], "t0.png");
    // 状态帧查表：四态各自命中；缺帧状态回落 idle
    DOCTEST_CHECK_EQ(&stateFrames(skin, PetAnimState::Sleep), &skin.sleepFrames);
    DOCTEST_CHECK_EQ(&stateFrames(skin, PetAnimState::Typing),
                     &skin.typingFrames);
    DOCTEST_CHECK_EQ(&stateFrames(skin, PetAnimState::Idle), &skin.idleFrames);
    const PetSkin plainSkin = parsePetSkinJson(kValidSkinJson).unsafeValue();
    DOCTEST_CHECK_EQ(&stateFrames(plainSkin, PetAnimState::Sleep),
                     &plainSkin.idleFrames);  // 无 sleep 帧 → idle
    return true;
});

REGISTER_TEST(pet_default_skin_m2_frames, "pet: 占位皮肤含 M2 帧 + 旧版升级重生成", {
    using namespace module::pet;
    const std::filesystem::path dir = tempDir("skin_m2");
    removeTreeQuietly(dir);
    auto r = ensureDefaultSkin(dir);
    DOCTEST_CHECK_MESSAGE(r.isOk(), r.isFail() ? r.errorMessage() : "");
    const PetSkin& skin = r.unsafeValue();
    DOCTEST_CHECK(skin.sleepFrames.size() >= 3);
    DOCTEST_CHECK(skin.typingFrames.size() >= 2);
    DOCTEST_CHECK(!skin.phrases.empty());
    for (size_t i = 0; i < skin.sleepFrames.size(); ++i) {
        DOCTEST_CHECK(pngLooksValid(skin.dir / skin.sleepFrames[i], 128, 128));
    }
    for (size_t i = 0; i < skin.typingFrames.size(); ++i) {
        DOCTEST_CHECK(pngLooksValid(skin.dir / skin.typingFrames[i], 128, 128));
    }
    // 幂等：新版直接加载（帧数不变）
    auto again = ensureDefaultSkin(dir);
    DOCTEST_CHECK(again.isOk());
    DOCTEST_CHECK_EQ(again.unsafeValue().sleepFrames.size(),
                     skin.sleepFrames.size());
    // 旧版内置皮肤（缺 M2 帧，source 是内置标记）→ 重新生成升级
    removeTreeQuietly(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / "pet.json", std::ios::trunc);
        out << "{\"name\":\"clay-cat\",\"source\":\"DevDesk 内置生成\","
               "\"states\":{\"idle\":[\"f0.png\"]}}";
    }
    auto upgraded = ensureDefaultSkin(dir);
    DOCTEST_CHECK(upgraded.isOk());
    DOCTEST_CHECK(!upgraded.unsafeValue().sleepFrames.empty());
    // 用户替换过的皮肤（source 非内置标记）缺 M2 帧也原样保留
    removeTreeQuietly(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / "pet.json", std::ios::trunc);
        out << "{\"name\":\"my-art\",\"source\":\"artist\","
               "\"states\":{\"idle\":[\"a0.png\"]}}";
    }
    auto kept = ensureDefaultSkin(dir);
    DOCTEST_CHECK(kept.isOk());
    DOCTEST_CHECK(kept.unsafeValue().sleepFrames.empty());
    DOCTEST_CHECK_EQ(kept.unsafeValue().idleFrames.size(), static_cast<size_t>(1));
    removeTreeQuietly(dir);
    return true;
});

// 新内置皮肤名（宏外声明：REGISTER_TEST 宏体内不能出现裸逗号）
const char* kNewBuiltinSkins[] = {"duck", "slime", "robot", "ghost"};

REGISTER_TEST(pet_builtin_skins, "pet: 四款新内置皮肤（四态帧/互异/发现/短语表/替换保留）", {
    using namespace module::pet;
    const std::filesystem::path root = tempDir("builtin_skins");
    removeTreeQuietly(root);
    setEnvVar("DEVDESK_PETS_DIR", root.string());
    // ensureBuiltinSkins 生成全部 5 款（clay-cat + 四款新皮肤）
    const std::vector<std::string> generated = ensureBuiltinSkins(root);
    DOCTEST_CHECK_EQ(generated.size(), static_cast<size_t>(5));
    // discoverSkins 发现全部 5 款（设置页下拉/右键换肤菜单数据源，零改动）
    const std::vector<std::string> found = discoverSkins();
    DOCTEST_CHECK_EQ(found.size(), static_cast<size_t>(5));
    for (const std::string& name : builtinSkinNames()) {
        DOCTEST_CHECK(std::find(found.begin(), found.end(), name) != found.end());
    }
    // 四款新皮肤 + clay-cat 全部载入（后者做互异/短语对比基准）
    std::vector<PetSkin> skins;
    for (const char* name : kNewBuiltinSkins) {
        auto r = loadSkin(root / name);
        DOCTEST_CHECK_MESSAGE(r.isOk(), r.isFail() ? r.errorMessage() : name);
        if (r.isOk()) {
            skins.push_back(std::move(r.unsafeValue()));
        }
    }
    auto catR = loadSkin(root / "clay-cat");
    DOCTEST_CHECK(catR.isOk());
    if (catR.isOk()) {
        skins.push_back(std::move(catR.unsafeValue()));
    }
    DOCTEST_CHECK_EQ(skins.size(), static_cast<size_t>(5));
    // 每款：四态帧数齐全 + 128×128 + 全帧 PNG 结构有效 + 短语表非空
    for (const PetSkin& skin : skins) {
        DOCTEST_CHECK_EQ(skin.idleFrames.size(), static_cast<size_t>(6));
        DOCTEST_CHECK_EQ(skin.pokedFrames.size(), static_cast<size_t>(2));
        DOCTEST_CHECK_EQ(skin.sleepFrames.size(), static_cast<size_t>(3));
        DOCTEST_CHECK_EQ(skin.typingFrames.size(), static_cast<size_t>(2));
        DOCTEST_CHECK_EQ(skin.frameWidth, 128);
        DOCTEST_CHECK_EQ(skin.frameHeight, 128);
        DOCTEST_CHECK(!skin.phrases.empty());
        for (size_t i = 0; i < skin.idleFrames.size(); ++i) {
            DOCTEST_CHECK(pngLooksValid(skin.dir / skin.idleFrames[i], 128, 128));
        }
        for (size_t i = 0; i < skin.pokedFrames.size(); ++i) {
            DOCTEST_CHECK(pngLooksValid(skin.dir / skin.pokedFrames[i], 128, 128));
        }
        for (size_t i = 0; i < skin.sleepFrames.size(); ++i) {
            DOCTEST_CHECK(pngLooksValid(skin.dir / skin.sleepFrames[i], 128, 128));
        }
        for (size_t i = 0; i < skin.typingFrames.size(); ++i) {
            DOCTEST_CHECK(pngLooksValid(skin.dir / skin.typingFrames[i], 128, 128));
        }
    }
    // 五款互异：idle 首帧 PNG 原始字节两两不同（stored-deflate 不压缩，
    // 像素不同 → 字节必不同）
    std::vector<std::string> blobs;
    for (const PetSkin& skin : skins) {
        std::ifstream f(skin.dir / skin.idleFrames[0], std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        blobs.push_back(ss.str());
        DOCTEST_CHECK(!blobs.back().empty());
    }
    for (size_t i = 0; i < blobs.size(); ++i) {
        for (size_t j = i + 1; j < blobs.size(); ++j) {
            DOCTEST_CHECK(blobs[i] != blobs[j]);
        }
    }
    // 新皮肤短语表 ≠ clay-cat 语料（性格区分；clay-cat 是 skins.back()）
    for (size_t i = 0; i + 1 < skins.size(); ++i) {
        DOCTEST_CHECK(skins[i].phrases != skins.back().phrases);
    }
    // 用户替换语义：duck 换成美术包（source 非内置标记）→ 原样保留
    {
        std::ofstream out(root / "duck" / "pet.json", std::ios::trunc);
        out << "{\"name\":\"duck\",\"source\":\"artist\","
               "\"states\":{\"idle\":[\"a0.png\"]}}";
    }
    const std::vector<std::string> again = ensureBuiltinSkins(root);
    DOCTEST_CHECK_EQ(again.size(), static_cast<size_t>(5));
    auto duck = loadSkin(root / "duck");
    DOCTEST_CHECK(duck.isOk());
    DOCTEST_CHECK_EQ(duck.unsafeValue().idleFrames.size(), static_cast<size_t>(1));
    setEnvVar("DEVDESK_PETS_DIR", "");
    removeTreeQuietly(root);
    return true;
});

// ============ 10. 内置美术皮肤（真实素材包）============

// 仓库自带的真实美术素材包（assets/pets/anime-girl，OpenGameArt
// "Anime Girl" Michona by Alicahordana，CC0 可再分发）：随源码树分发，
// CMake POST_BUILD 拷到 build/assets/pets 供运行期发现。本用例直接从
// 源码树加载（__FILE__ 相对定位，不依赖 cwd），断言四态帧齐全 + 授权
// 字段自带（R7 素材纪律：内置分发的素材必须 CC0/宽松许可）
REGISTER_TEST(pet_anime_girl_asset_skin, "pet: 内置美术皮肤 anime-girl（CC0 四态断言）", {
    using namespace module::pet;
    const std::filesystem::path skinDir =
        std::filesystem::path(__FILE__).parent_path() / ".." / ".." / "assets" /
        "pets" / "anime-girl";
    auto r = loadSkin(skinDir.lexically_normal());
    DOCTEST_CHECK_MESSAGE(r.isOk(), r.isFail() ? r.errorMessage() : "");
    if (r.isFail()) return false;
    const PetSkin& skin = r.unsafeValue();
    // 元数据：名字/尺寸/帧率在合法区间；license/source 自带（含 CC0 标记）
    DOCTEST_CHECK_EQ(skin.name, "anime-girl");
    DOCTEST_CHECK_EQ(skin.frameWidth, 128);
    DOCTEST_CHECK_EQ(skin.frameHeight, 128);
    DOCTEST_CHECK(skin.frameRate >= 1.0 && skin.frameRate <= 30.0);
    DOCTEST_CHECK(skin.license.find("CC0") != std::string::npos);
    DOCTEST_CHECK(!skin.source.empty());
    // 四态帧齐全（blink→idle / jump→poked / walk→typing / 闭眼帧→sleep）
    DOCTEST_CHECK_EQ(skin.idleFrames.size(), static_cast<size_t>(5));
    DOCTEST_CHECK_EQ(skin.pokedFrames.size(), static_cast<size_t>(13));
    DOCTEST_CHECK_EQ(skin.typingFrames.size(), static_cast<size_t>(11));
    DOCTEST_CHECK_EQ(skin.sleepFrames.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(!skin.phrases.empty());
    // 全部状态帧都是结构合法的 128×128 RGBA PNG（美术包与占位皮肤同规格）
    DOCTEST_CHECK(allFramesPngValid(skinDir, skin.idleFrames));
    DOCTEST_CHECK(allFramesPngValid(skinDir, skin.pokedFrames));
    DOCTEST_CHECK(allFramesPngValid(skinDir, skin.sleepFrames));
    DOCTEST_CHECK(allFramesPngValid(skinDir, skin.typingFrames));
    // 名字合法（会拼回路径：discoverSkins/applyPetSkin 白名单同款字符集）
    DOCTEST_CHECK(skin.name.find_first_not_of(
                      "abcdefghijklmnopqrstuvwxyz"
                      "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") ==
                  std::string::npos);
    return true;
});

REGISTER_TEST(pet_discover_skins, "pet: 皮肤发现（下拉/换肤菜单的数据源）", {
    using namespace module::pet;
    const std::filesystem::path root = tempDir("discover");
    removeTreeQuietly(root);
    setEnvVar("DEVDESK_PETS_DIR", root.string());
    // 空目录：无皮肤
    std::filesystem::create_directories(root);
    DOCTEST_CHECK(discoverSkins().empty());
    // 生成默认皮肤 + 手工放第二个皮肤包 → 都被发现
    DOCTEST_CHECK(ensureDefaultSkin(root / "clay-cat").isOk());
    std::filesystem::create_directories(root / "pixel-dog");
    {
        std::ofstream out(root / "pixel-dog" / "pet.json", std::ios::trunc);
        out << "{\"name\":\"pixel-dog\",\"states\":{\"idle\":[\"d0.png\"]}}";
    }
    const std::vector<std::string> skins = discoverSkins();
    DOCTEST_CHECK_EQ(skins.size(), static_cast<size_t>(2));
    DOCTEST_CHECK(std::find(skins.begin(), skins.end(), "clay-cat") !=
                  skins.end());
    DOCTEST_CHECK(std::find(skins.begin(), skins.end(), "pixel-dog") !=
                  skins.end());
    // 无 pet.json 的目录与非法名目录（路径穿越风险）不进清单
    std::filesystem::create_directories(root / "no-json-dir");
    std::filesystem::create_directories(root / "bad..name");
    {
        std::ofstream out(root / "bad..name" / "pet.json", std::ios::trunc);
        out << "{\"states\":{\"idle\":[\"x.png\"]}}";
    }
    const std::vector<std::string> filtered = discoverSkins();
    DOCTEST_CHECK_EQ(filtered.size(), static_cast<size_t>(2));
    setEnvVar("DEVDESK_PETS_DIR", "");
    removeTreeQuietly(root);
    return true;
});

REGISTER_TEST(pet_apply_skin_and_reset, "pet: applyPetSkin 校验/持久化 + 位置重置", {
    using namespace module::pet;
    const std::filesystem::path root = tempDir("applyskin");
    removeTreeQuietly(root);
    setEnvVar("DEVDESK_PETS_DIR", root.string());
    DOCTEST_CHECK(ensureDefaultSkin(root / "clay-cat").isOk());
    std::filesystem::create_directories(root / "pixel-dog");
    {
        std::ofstream out(root / "pixel-dog" / "pet.json", std::ios::trunc);
        out << "{\"name\":\"pixel-dog\",\"states\":{\"idle\":[\"d0.png\"]}}";
    }
    // 预置配置：已知位置 + 默认皮肤（无头环境窗口未开 → 只持久化路径）
    const std::filesystem::path cfgFile = root / "pet_config.json";
    {
        PetConfigStore store(cfgFile);
        PetConfig cfg;
        cfg.skinName = "clay-cat";
        cfg.x = 500;
        cfg.y = 600;
        DOCTEST_CHECK(store.save(cfg).isOk());
    }
    // 未发现的皮肤名被拒绝，配置不动
    applyPetSkin("no-such-skin");
    {
        const PetConfig cfg = PetConfigStore(cfgFile).load();
        DOCTEST_CHECK_EQ(cfg.skinName, "clay-cat");
    }
    // 合法皮肤名 → 持久化生效，位置不动（窗口未开）
    applyPetSkin("pixel-dog");
    {
        const PetConfig cfg = PetConfigStore(cfgFile).load();
        DOCTEST_CHECK_EQ(cfg.skinName, "pixel-dog");
        DOCTEST_CHECK_EQ(cfg.x, 500);
        DOCTEST_CHECK_EQ(cfg.y, 600);
    }
    // 位置重置 → 坐标清回 -1（未设置哨兵），皮肤不被动
    resetPetPosition();
    {
        const PetConfig cfg = PetConfigStore(cfgFile).load();
        DOCTEST_CHECK_EQ(cfg.x, -1);
        DOCTEST_CHECK_EQ(cfg.y, -1);
        DOCTEST_CHECK_EQ(cfg.skinName, "pixel-dog");
    }
    setEnvVar("DEVDESK_PETS_DIR", "");
    removeTreeQuietly(root);
    return true;
});

// ---- 鼠标穿透桥（任务⑥：穿透锁死态的设置页恢复路径）----
// 无头环境 handle==nullptr → setPetClickThrough 走「只持久化」分支；
// 即时 setWindowMousePassthrough 分支真机验证。断言：toggle 写 config、
// 状态往返不丢其他字段
REGISTER_TEST(pet_click_through_bridge,
              "pet: setPetClickThrough 持久化 + 状态往返（⑥恢复路径）", {
    using namespace module::pet;
    const std::filesystem::path root = tempDir("through");
    removeTreeQuietly(root);
    std::filesystem::create_directories(root);
    setEnvVar("DEVDESK_PETS_DIR", root.string());
    const std::filesystem::path cfgFile = petConfigFile();
    // 预置：未穿透 + 已知皮肤/位置
    {
        PetConfigStore store(cfgFile);
        PetConfig cfg;
        cfg.skinName = "clay-cat";
        cfg.x = 300;
        cfg.y = 400;
        cfg.clickThrough = false;
        DOCTEST_CHECK(store.save(cfg).isOk());
    }
    // 开穿透 → 落盘 true，其他字段原样（读-改-写不串字段）
    setPetClickThrough(true);
    {
        const PetConfig cfg = PetConfigStore(cfgFile).load();
        DOCTEST_CHECK(cfg.clickThrough);
        DOCTEST_CHECK_EQ(cfg.skinName, "clay-cat");
        DOCTEST_CHECK_EQ(cfg.x, 300);
        DOCTEST_CHECK_EQ(cfg.y, 400);
    }
    // 关穿透（锁死恢复方向）→ false 往返
    setPetClickThrough(false);
    {
        const PetConfig cfg = PetConfigStore(cfgFile).load();
        DOCTEST_CHECK(!cfg.clickThrough);
    }
    setEnvVar("DEVDESK_PETS_DIR", "");
    removeTreeQuietly(root);
    return true;
});

// ---- 拖拽锚点跟随（2026-09-23 真机症状 1：拖动抖动）----
// 复现坐标模型：GLFW 上报的光标位置是「窗口相对」的——窗口被拖拽回调移动
// 后，同样的屏幕移动产生反向偏差的读数。旧 delta 累加方案在此模型下
// +10/-5/+10 振荡；锚点方案：首个拖拽事件捕获锚点（窗口原地起步），
// 此后窗口逐事件精确移动光标屏幕位移（1:1，无振荡无累积漂移）。

// 光标轨迹步进（REGISTER_TEST 宏体内不能有含逗号的初始化列表，拆出函数）
namespace {
int petDragStepX(int i) {
    static const int dx[5] = {8, 5, 5, 7, 6};
    return dx[i];
}
int petDragStepY(int i) {
    static const int dy[5] = {0, 3, 4, 2, 5};
    return dy[i];
}
} // namespace

REGISTER_TEST(pet_drag_anchor_follow, "pet: 拖拽锚点跟随——1:1 跟光标，无反馈抖动", {
    module::pet::PetDragAnchor anchor;
    int winX = 500;
    int winY = 400;
    double screenX = 560.0;  // 光标屏幕位（模拟真机轨迹：单调右移+下移）
    double screenY = 440.0;
    int prevTargetX = winX;
    int prevTargetY = winY;
    int prevScreenXi = static_cast<int>(screenX);
    int prevScreenYi = static_cast<int>(screenY);
    for (int i = 0; i < 5; ++i) {
        screenX += petDragStepX(i);
        screenY += petDragStepY(i);
        // GLFW 会报的窗口内光标位（窗口移动后参考系已变——抖动根源所在）
        const double relX = screenX - static_cast<double>(winX);
        const double relY = screenY - static_cast<double>(winY);
        const auto target =
            module::pet::petDragMove(anchor, winX, winY, relX, relY);
        if (i == 0) {
            // 首个拖拽事件：锚点捕获，窗口原地起步（不跳变）
            DOCTEST_CHECK_EQ(target.first, 500);
            DOCTEST_CHECK_EQ(target.second, 400);
        } else {
            // 1:1 跟随：窗口位移 == 光标屏幕位移（旧 delta 方案在此轨迹上
            // 会出现负增量/振荡）
            DOCTEST_CHECK_EQ(target.first - prevTargetX,
                             static_cast<int>(screenX) - prevScreenXi);
            DOCTEST_CHECK_EQ(target.second - prevTargetY,
                             static_cast<int>(screenY) - prevScreenYi);
            DOCTEST_CHECK(target.first > prevTargetX);  // 单调不回跳
        }
        prevTargetX = target.first;
        prevTargetY = target.second;
        prevScreenXi = static_cast<int>(screenX);
        prevScreenYi = static_cast<int>(screenY);
        winX = target.first;
        winY = target.second;
    }
    // 光标不动 → 窗口不动（锚点保持，无漂移）
    {
        const double relX = screenX - static_cast<double>(winX);
        const double relY = screenY - static_cast<double>(winY);
        const auto target =
            module::pet::petDragMove(anchor, winX, winY, relX, relY);
        DOCTEST_CHECK_EQ(target.first, winX);
        DOCTEST_CHECK_EQ(target.second, winY);
    }
    // 释放清锚点；下次拖拽在任意新位置重锚定（不跳变到旧偏移）
    module::pet::petDragEnd(anchor);
    DOCTEST_CHECK(!anchor.valid);
    {
        winX = 900;
        winY = 700;
        screenX = 930.0;
        screenY = 715.0;
        const double relX = screenX - static_cast<double>(winX);
        const double relY = screenY - static_cast<double>(winY);
        const auto target =
            module::pet::petDragMove(anchor, winX, winY, relX, relY);
        DOCTEST_CHECK_EQ(target.first, 900);  // 新锚点：窗口停在原地起步
        DOCTEST_CHECK_EQ(target.second, 700);
    }
    return true;
});

// ============ 11. 审计修复（2026-09-23 pet 簇）============

REGISTER_TEST(pet_pos_clamp_work_area, "pet: 已知坐标钳进工作区（审计[12]）", {
    // 常规位置不动
    const auto normal =
        module::pet::clampPetPosToWorkArea(100, 200, 0, 0, 1920, 1080, 128, 128);
    DOCTEST_CHECK_EQ(normal.first, 100);
    DOCTEST_CHECK_EQ(normal.second, 200);
    // 负坐标（拖出左/上缘）→ 钳到工作区原点——这是哨兵冲突根因：落盘后
    // sanitized 的 x<-1→-1 把「未设置」哨兵和真实负位混为一谈，重启跳默认位
    const auto negative =
        module::pet::clampPetPosToWorkArea(-50, -80, 0, 0, 1920, 1080, 128, 128);
    DOCTEST_CHECK_EQ(negative.first, 0);
    DOCTEST_CHECK_EQ(negative.second, 0);
    // 越右下缘 → 工作区内边缘（1920-128 = 1792）
    const auto overflow =
        module::pet::clampPetPosToWorkArea(99999, 99999, 0, 0, 1920, 1080, 128, 128);
    DOCTEST_CHECK_EQ(overflow.first, 1792);
    DOCTEST_CHECK_EQ(overflow.second, 952);
    // 非零工作区原点（副屏）：钳界随 workX 平移（-2000 ~ -2000+1920-128）
    const auto offOrigin =
        module::pet::clampPetPosToWorkArea(-10, 300, -2000, 0, 1920, 1080, 128, 128);
    DOCTEST_CHECK_EQ(offOrigin.first, -208);
    DOCTEST_CHECK_EQ(offOrigin.second, 300);
    // 工作区不可用（无头）→ 原样
    const auto noArea =
        module::pet::clampPetPosToWorkArea(-50, -60, 0, 0, 0, 0, 128, 128);
    DOCTEST_CHECK_EQ(noArea.first, -50);
    DOCTEST_CHECK_EQ(noArea.second, -60);
    // 落盘哨兵语义回归：钳后的 0 合法存取；只有 -1 保持「未设置」
    const auto cfg0 = module::pet::PetConfigStore::sanitized(
        [&] { module::pet::PetConfig c; c.x = 0; c.y = 0; return c; }());
    DOCTEST_CHECK_EQ(cfg0.x, 0);
    DOCTEST_CHECK_EQ(cfg0.y, 0);
    return true;
});

// 帧名穿越拒绝判定（合法皮肤包/单点扩展名必须放行）。辅助函数绕开
// REGISTER_TEST 宏体裸逗号限制
static bool frameNameRejected(const char* frameName) {
    const std::string json =
        std::string("{\"states\":{\"idle\":[\"") + frameName + "\"]}}";
    return module::pet::parsePetSkinJson(json).isFail();
}

REGISTER_TEST(pet_skin_frame_name_traversal, "pet: 帧名路径穿越拒绝（审计[28]）", {
    // 穿越/绝对路径/盘符形态 → 解析整体失败（idle 必填）
    DOCTEST_CHECK(frameNameRejected("../secret.png"));
    DOCTEST_CHECK(frameNameRejected("..\\secret.png"));
    DOCTEST_CHECK(frameNameRejected("../../assets/../x.png"));
    DOCTEST_CHECK(frameNameRejected("/etc/passwd.png"));
    DOCTEST_CHECK(frameNameRejected("C:/Windows/evil.png"));
    DOCTEST_CHECK(frameNameRejected("sub/inner.png"));
    // 合法名不误伤：PNG 扩展名单点、中文名、连字符
    DOCTEST_CHECK(!frameNameRejected("f0.png"));
    DOCTEST_CHECK(!frameNameRejected("帧一.png"));
    DOCTEST_CHECK(!frameNameRejected("idle-0.PNG"));
    // 可选状态含非法帧名 → 静默丢弃回落 idle（皮肤照常解析）
    const std::string pokedJson =
        "{\"states\":{\"idle\":[\"ok.png\"],\"poked\":[\"../x.png\"]}}";
    const auto pokedResult = module::pet::parsePetSkinJson(pokedJson);
    DOCTEST_CHECK(pokedResult.isOk());
    DOCTEST_CHECK(pokedResult.unsafeValue().pokedFrames.empty());
    DOCTEST_CHECK_EQ(pokedResult.unsafeValue().frames(true).size(),
                     static_cast<size_t>(1));
    // phrases 是展示文本不是路径：含路径字符不拖垮解析
    const std::string phraseJson =
        "{\"states\":{\"idle\":[\"f.png\"]},"
        "\"phrases\":[\"走之前去C:/Downloads拿东西\",\"嗯……/../没什么\"]}";
    const auto phraseResult = module::pet::parsePetSkinJson(phraseJson);
    DOCTEST_CHECK(phraseResult.isOk());
    DOCTEST_CHECK_EQ(phraseResult.unsafeValue().phrases.size(),
                     static_cast<size_t>(2));
    return true;
});
