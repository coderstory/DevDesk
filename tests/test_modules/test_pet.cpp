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
//   9. 内置皮肤全集（robot/ghost 两款）：四态帧齐全、两款互异、
//      discoverSkins 发现、短语表区分、用户替换保留
//  10. （已移除 2026-09-25）内置美术皮肤 anime-girl CC0 四态测试——皮肤
//      随用户要求只留 robot/ghost 而删，测试同步移除（见 §10 占位注释）
//  11. 审计修复（2026-09-23 pet 簇）：帧名路径穿越拒绝 [28] + 已知坐标
//      钳进工作区纯函数 [12]（[0][1] 的 onWindowDestroyed 接线断言在 §5）
//  12. M2 收尾（2026-09-23 pet-m2）：多显示器按位置选屏钳制 + 负坐标持久化
//      往返 + Sleep 零渲染暂停门（电源事件真触发属人工点验）
//  13. 气泡内容源（产品审计 [18]，§4.5 ①②）：②逾期计数 mock 桥三分支
//      （N>0 计数文案 / N=0 正面文案 / 无数据回退）+ ①静态语料逐条往返
//  14. 气泡调度纯函数（PAB-05）：dt/状态/roll → 「文案来源 + 停留时长」的
//      来源判定与边界（门控/阈值/不足一帧）+ 节拍数字与注释对齐
// 子窗口真实渲染/拖拽/穿透/气泡视觉属人工点验（设计 §6 M2 验收项）。
#include "doctest.h"
#include "test_support.h"

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
#include <set>
#include <sstream>
#include <string>
#include <vector>

// app:: 实现来自 src/app_sidebar.cpp（compose 真实现直接编进测试二进制）
// + test_settings.cpp 的 dsl_app_impl.h 注入（dslAppConfig 等其余符号）
namespace app {
void compose(eui::Ui&, const eui::Screen&);
}

namespace {

std::filesystem::path tempDir(const char* name) {
    return test_temp::tempPath("devdesk_test_pet_" + std::string(name));
}

void removeTreeQuietly(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
}

// 审计 [16]：DEVDESK_PETS_DIR 隔离的 RAII 守卫（test_settings.cpp PruneEnv
// 同款约定）。构造 = 清残留 + 建临时目录 + 指环境变量；析构 = 环境变量拉回
// 未设置 + 删临时目录。断言 fail-fast 提前 return false 也会走析构，不留
// 环境/磁盘泄漏给同进程后续用例（旧版手工 set/clear 配对失败即跳尾）。
struct PetsDirEnv {
    const std::filesystem::path root;
    explicit PetsDirEnv(const std::string& tag) : root(tempDir(tag.c_str())) {
        removeTreeQuietly(root);
        test_support::setEnvVar("DEVDESK_PETS_DIR", root.string());
    }
    PetsDirEnv(const PetsDirEnv&) = delete;
    PetsDirEnv& operator=(const PetsDirEnv&) = delete;
    ~PetsDirEnv() {
        test_support::setEnvVar("DEVDESK_PETS_DIR", "");
        removeTreeQuietly(root);
    }
};

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
        test_temp::tempPath("devdesk_test_pet_config.json");
    std::error_code ec;
    std::filesystem::remove(file, ec);
    PetConfigStore store(file);
    PetConfig cfg;
    cfg.skinName = "my-cat";
    cfg.x = 1600;
    cfg.y = 800;
    DOCTEST_CHECK(store.save(cfg).isOk());
    const PetConfig loaded = store.load();
    DOCTEST_CHECK_EQ(loaded.skinName, "my-cat");
    DOCTEST_CHECK_EQ(loaded.x, 1600);
    DOCTEST_CHECK_EQ(loaded.y, 800);
    // 皮肤名白名单：路径分隔符/空 → 收敛默认 robot；坐标钳制保留 -1 哨兵
    cfg.skinName = "../evil";
    cfg.x = 999999;
    const PetConfig sanitizedCfg = PetConfigStore::sanitized(cfg);
    DOCTEST_CHECK_EQ(sanitizedCfg.skinName, "robot");
    DOCTEST_CHECK_EQ(sanitizedCfg.x, 32767);
    DOCTEST_CHECK_EQ(PetConfigStore::sanitized(PetConfig{}).x, -1);
    std::filesystem::remove(file, ec);
    return true;
});

// ============ 5. startPetWindow 无头冒烟 ============

REGISTER_TEST(pet_start_window_request, "pet: 开窗请求逐字段断言（无头）", {
    const PetsDirEnv pets("petsroot");
    const std::filesystem::path& petsRoot = pets.root;
    // 清空可能残留的窗口请求队列
    (void)app::consumeWindowRequests();

    module::pet::startPetWindow(module::pet::PetWindowConfig{
        "", 50, 60});  // 皮肤空 = 默认；位置已知
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
    // 默认皮肤已生成在测试隔离目录
    DOCTEST_CHECK(std::filesystem::exists(petsRoot / "robot" / "pet.json"));

    // 未知皮肤名 → 回退默认皮肤，窗口照常入队
    module::pet::startPetWindow(module::pet::PetWindowConfig{"no-such-skin", -1, -1});
    std::vector<app::DslWindowRequest> second = app::consumeWindowRequests();
    DOCTEST_CHECK_EQ(second.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(!second[0].mousePassthrough);   // 挂机穿透已移除，恒不穿透
    DOCTEST_CHECK(second[0].width == 128);       // 默认皮肤 128×128

    // 单边 -1 哨兵：只重置未设置的那一边，已知坐标不丢
    //（无头环境无工作区 → 未设置边落到兜底值 100）
    module::pet::startPetWindow(module::pet::PetWindowConfig{"", 77, -1});
    std::vector<app::DslWindowRequest> third = app::consumeWindowRequests();
    DOCTEST_CHECK_EQ(third.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(third[0].x, 77);   // 已知的 x 原样保留
    DOCTEST_CHECK_EQ(third[0].y, 100);  // 未设置的 y 走兜底

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

// 内容源②（审计 [18]，§4.5）：mock 逾期计数桥三分支——N>0 含计数文案 /
// N=0 正面文案不带死数字 / 桥空或 nullopt（todo 关、查询失败）回退静态语料
REGISTER_TEST(pet_bubble_overdue_content, "pet: 气泡内容源②逾期数文案（N>0/N=0/无数据）", {
    using namespace module::pet;
    // N>0 → 「你有 N 条逾期任务」：文本含计数与款式词，装气泡可往返
    std::function<std::optional<int64_t>()> three =
        [] { return std::optional<int64_t>(3); };
    const std::string t3 = overdueBubbleText(three);
    DOCTEST_CHECK_MESSAGE(t3.find("3") != std::string::npos, t3);
    DOCTEST_CHECK_MESSAGE(t3.find("逾期任务") != std::string::npos, t3);
    PetBubble bubble;
    bubble.show(t3, 4.0);
    DOCTEST_CHECK(bubble.visible);
    DOCTEST_CHECK_EQ(bubble.text, t3);
    bubble.tick(4.5);
    DOCTEST_CHECK(!bubble.visible);
    // 大数字同款（串接 std::to_string，不做千分位）
    std::function<std::optional<int64_t>()> hundred =
        [] { return std::optional<int64_t>(120); };
    DOCTEST_CHECK_MESSAGE(
        overdueBubbleText(hundred).find("120") != std::string::npos,
        overdueBubbleText(hundred));
    // N=0 → 正面文案，且不得出现「0 条」死数字
    std::function<std::optional<int64_t>()> zero =
        [] { return std::optional<int64_t>(0); };
    const std::string t0 = overdueBubbleText(zero);
    DOCTEST_CHECK_MESSAGE(!t0.empty(), t0);
    DOCTEST_CHECK_MESSAGE(t0.find("0") == std::string::npos, t0);
    // 桥返回 nullopt（todo 模块关闭 / 查询失败）→ 空串 = 回退内容源①
    std::function<std::optional<int64_t>()> unavailable =
        [] { return std::optional<int64_t>(std::nullopt); };
    DOCTEST_CHECK(overdueBubbleText(unavailable).empty());
    // 桥未注入（空 std::function）→ 同样空串回退，气泡绝不出「你有 条…」残句
    std::function<std::optional<int64_t>()> none;
    DOCTEST_CHECK(overdueBubbleText(none).empty());
    return true;
});

// 内容源①补强（审计 [18]）：静态语料逐条装气泡往返——语料退化（空表/空串/
// 装不进气泡）直接红，M2 验收「关 todo 模块时回退通用语料」的数据侧保障
REGISTER_TEST(pet_bubble_static_corpus_roundtrip, "pet: 内容源①静态语料逐条气泡往返", {
    using namespace module::pet;
    const std::vector<std::string>& phrases = defaultPhrases();
    DOCTEST_CHECK(phrases.size() >= 4);
    for (const std::string& p : phrases) {
        DOCTEST_CHECK_MESSAGE(!p.empty(), p);
        PetBubble bubble;
        bubble.show(p, 3.0);
        DOCTEST_CHECK(bubble.visible);
        DOCTEST_CHECK_EQ(bubble.text, p);
        bubble.tick(3.5);
        DOCTEST_CHECK(!bubble.visible);
    }
    return true;
});

// 内容源③（M2 §4.5，产品审计 [18]）：代理事件文案——mock ProxyEventGetter
// 返回各终态码，断言气泡文案；无事件（nullopt）/ 未注入桥回退（空串）
REGISTER_TEST(pet_bubble_proxy_event_content,
              "pet: 气泡内容源③代理事件文案（2xx/4xx/5xx/网络失败/无事件）", {
    using namespace module::pet;
    // 2xx → 转发完成
    std::function<std::optional<int>()> ok = [] {
        return std::optional<int>(200);
    };
    const std::string tOk = proxyEventBubbleText(ok);
    DOCTEST_CHECK_MESSAGE(!tOk.empty(), tOk);
    DOCTEST_CHECK_MESSAGE(tOk.find("代理") != std::string::npos, tOk);
    DOCTEST_CHECK_MESSAGE(tOk.find("完成") != std::string::npos, tOk);
    // 装气泡可往返
    PetBubble bubble;
    bubble.show(tOk, 3.0);
    DOCTEST_CHECK(bubble.visible);
    DOCTEST_CHECK_EQ(bubble.text, tOk);
    bubble.tick(3.5);
    DOCTEST_CHECK(!bubble.visible);
    // 3xx（如 301）也归转发完成，不出「出错」字样
    std::function<std::optional<int>()> redirect = [] {
        return std::optional<int>(301);
    };
    const std::string tRedirect = proxyEventBubbleText(redirect);
    DOCTEST_CHECK_MESSAGE(tRedirect.find("完成") != std::string::npos, tRedirect);
    // 4xx → 「代理出错：{status}」含状态码
    std::function<std::optional<int>()> rateLimited = [] {
        return std::optional<int>(429);
    };
    const std::string t429 = proxyEventBubbleText(rateLimited);
    DOCTEST_CHECK_MESSAGE(t429.find("429") != std::string::npos, t429);
    DOCTEST_CHECK_MESSAGE(t429.find("出错") != std::string::npos, t429);
    // 5xx → 同款带码
    std::function<std::optional<int>()> serverErr = [] {
        return std::optional<int>(503);
    };
    const std::string t503 = proxyEventBubbleText(serverErr);
    DOCTEST_CHECK_MESSAGE(t503.find("503") != std::string::npos, t503);
    DOCTEST_CHECK_MESSAGE(t503.find("出错") != std::string::npos, t503);
    // 0（上游不可达，网络层失败无响应码）→ 网络失败文案，不带裸 0 码
    std::function<std::optional<int>()> netFail = [] {
        return std::optional<int>(0);
    };
    const std::string tNet = proxyEventBubbleText(netFail);
    DOCTEST_CHECK_MESSAGE(tNet.find("网络") != std::string::npos, tNet);
    DOCTEST_CHECK_MESSAGE(tNet.find("0") == std::string::npos, tNet);
    // 桥返回 nullopt（无待消费事件 / claude 关 / 过期）→ 空串 = 回退②/①
    std::function<std::optional<int>()> unavailable = [] {
        return std::optional<int>(std::nullopt);
    };
    DOCTEST_CHECK(proxyEventBubbleText(unavailable).empty());
    // 桥未注入（空 std::function）→ 空串回退，绝不出残句
    std::function<std::optional<int>()> none;
    DOCTEST_CHECK(proxyEventBubbleText(none).empty());
    return true;
});

// 三级内容源回退序（§4.5 ③>②>①，审计 [18]）：petDataBubbleText 纯函数——
// ③有事件必压过②；③无回退②；③②皆无数据回空串（调用方出①静态语料）
REGISTER_TEST(pet_bubble_three_level_fallback,
              "pet: 气泡三级内容源回退序（③代理事件>②逾期>①静态）", {
    using namespace module::pet;
    std::function<std::optional<int>()> proxyOk = [] {
        return std::optional<int>(200);
    };
    std::function<std::optional<int64_t>()> overdue3 = [] {
        return std::optional<int64_t>(3);
    };
    std::function<std::optional<int>()> proxyNone = [] {
        return std::optional<int>(std::nullopt);
    };
    std::function<std::optional<int64_t>()> overdueNone = [] {
        return std::optional<int64_t>(std::nullopt);
    };
    // ③有 + ②有 → 取③（3s），即便②也有逾期数
    const auto both = petDataBubbleText(proxyOk, overdue3);
    DOCTEST_CHECK_MESSAGE(both.first.find("代理") != std::string::npos, both.first);
    DOCTEST_CHECK_MESSAGE(both.first.find("逾期") == std::string::npos, both.first);
    DOCTEST_CHECK(both.second == 3.0);
    // ③无 + ②有 → 取②（4s，逾期文案）
    const auto onlyOverdue = petDataBubbleText(proxyNone, overdue3);
    DOCTEST_CHECK_MESSAGE(onlyOverdue.first.find("逾期") != std::string::npos,
                          onlyOverdue.first);
    DOCTEST_CHECK(onlyOverdue.second == 4.0);
    // ③有（4xx）+ ②有 → 仍取③（出错事件优先）
    std::function<std::optional<int>()> proxyErr = [] {
        return std::optional<int>(500);
    };
    const auto proxyErrWins = petDataBubbleText(proxyErr, overdue3);
    DOCTEST_CHECK_MESSAGE(proxyErrWins.first.find("500") != std::string::npos,
                          proxyErrWins.first);
    // ③无 + ②无 → 空串 = 调用方回退①静态语料
    const auto empty = petDataBubbleText(proxyNone, overdueNone);
    DOCTEST_CHECK(empty.first.empty());
    // 两桥都未注入 → 空串
    std::function<std::optional<int>()> noProxy;
    std::function<std::optional<int64_t>()> noOverdue;
    DOCTEST_CHECK(petDataBubbleText(noProxy, noOverdue).first.empty());
    return true;
});

// 气泡调度纯函数（PAB-05）：调度原先内联在 pet_window.cpp 的 onTimer 闭包里
// 无法单测——这里固定 dt / 状态 / roll 输入，断言「弹哪一级 + 停多久」与各边界
REGISTER_TEST(pet_bubble_schedule_pure,
              "pet: 气泡调度纯函数（来源/停留/门控/边界）", {
    using namespace module::pet;
    // dt = 1s 时的两个阈值：数据槽位 1/150 ≈ 0.00667，两级共享槽位上界 1/60
    const double tData = 1.0 / kPetDataSlotSec;
    const double tAny = 1.0 / kPetAnyBubbleSlotSec;
    // 命中数据槽位（③代理事件 > ②逾期）→ 停留 = 桥无数据降级静态语料的兜底
    const PetBubblePlan data = petBubbleSchedule(1.0, false, true, 0.0);
    DOCTEST_CHECK(data.source == PetBubbleSource::Data);
    DOCTEST_CHECK_EQ(data.holdSec, 3.0);
    // 槽位内贴近上界仍是数据槽位；恰好压在上界（严格小于）→ 落①静态语料
    DOCTEST_CHECK(petBubbleSchedule(1.0, false, true, tData - 1e-9).source ==
                  PetBubbleSource::Data);
    DOCTEST_CHECK(petBubbleSchedule(1.0, false, true, tData).source ==
                  PetBubbleSource::Static);
    // 无可用文案（③②两桥都无数据 → petDataBubbleText 空串）：调用方据此走
    // ①静态语料兜底，兜底停留就是计划里的 holdSec（3.0，非 0——0 会让气泡立消）
    std::function<std::optional<int>()> noProxy;
    std::function<std::optional<int64_t>()> noOverdue;
    DOCTEST_CHECK(petDataBubbleText(noProxy, noOverdue).first.empty());
    DOCTEST_CHECK_EQ(data.holdSec, kPetBubbleHoldSec);
    // 静态语料带区间 [1/150, 1/60)
    const PetBubblePlan stat = petBubbleSchedule(1.0, false, true, tAny - 1e-9);
    DOCTEST_CHECK(stat.source == PetBubbleSource::Static);
    DOCTEST_CHECK_EQ(stat.holdSec, 3.0);
    // roll 达到 1/60 / 接近 1（抽样上界）→ 本轮不弹
    const PetBubblePlan none = petBubbleSchedule(1.0, false, true, tAny);
    DOCTEST_CHECK(none.source == PetBubbleSource::None);
    DOCTEST_CHECK_EQ(none.holdSec, 0.0);
    DOCTEST_CHECK(petBubbleSchedule(1.0, false, true, 0.999999).source ==
                  PetBubbleSource::None);
    // 门控：气泡在场（上一泡没走不让位）/ 非 Idle（Poked/Sleep/Typing）→ 不弹
    DOCTEST_CHECK(petBubbleSchedule(1.0, true, true, 0.0).source ==
                  PetBubbleSource::None);
    DOCTEST_CHECK(petBubbleSchedule(1.0, false, false, 0.0).source ==
                  PetBubbleSource::None);
    // dt 不足一帧：首帧 dt = 0（阈值 0，roll 再小也不命中）必不弹
    DOCTEST_CHECK(petBubbleSchedule(0.0, false, true, 0.0).source ==
                  PetBubbleSource::None);
    DOCTEST_CHECK(petBubbleSchedule(-1.0, false, true, 0.0).source ==
                  PetBubbleSource::None);
    // 1ms 帧：阈值同比缩小（6.7e-6），常规 roll 不命中、极小 roll 才命中
    DOCTEST_CHECK(petBubbleSchedule(0.001, false, true, 0.5).source ==
                  PetBubbleSource::None);
    DOCTEST_CHECK(petBubbleSchedule(0.001, false, true, 1e-6).source ==
                  PetBubbleSource::Data);
    return true;
});

// 调度节拍 = 注释数字（PAB-05 核心：旧注释写「①静态语料平均 ~60s 一句」，代码
// 实为 ~100s）。dt = 1s 上用整数网格数各级命中占比（全固定算术、无随机源）：
//   数据槽位 = 1/150 → 1000/150000 点；静态 = 1/60 − 1/150 = 1/100 → 1500 点。
// 断言钉死这两个数，注释与代码再漂移即红
REGISTER_TEST(pet_bubble_schedule_cadence,
              "pet: 气泡调度节拍与注释数字一致（静态 ~100s 非 ~60s）", {
    using namespace module::pet;
    DOCTEST_CHECK_EQ(kPetDataSlotSec, 150.0);
    DOCTEST_CHECK_EQ(kPetAnyBubbleSlotSec, 60.0);
    DOCTEST_CHECK_EQ(kPetBubbleHoldSec, 3.0);
    constexpr int kGrid = 150000;
    int dataHits = 0;
    int staticHits = 0;
    for (int i = 0; i < kGrid; ++i) {
        const double roll = (i + 0.5) / kGrid;  // 避开 0/1 端点的均匀网格
        const PetBubblePlan plan = petBubbleSchedule(1.0, false, true, roll);
        if (plan.source == PetBubbleSource::Data) ++dataHits;
        if (plan.source == PetBubbleSource::Static) ++staticHits;
    }
    DOCTEST_CHECK_EQ(dataHits, 1000);    // 1/150（~2.5min 一泡）
    DOCTEST_CHECK_EQ(staticHits, 1500);  // 1/100（~100s；按 1/60 算才是旧注释）
    // 计划 → 气泡接线语义：按计划的停留时长装上气泡，到点消失
    PetBubble bubble;
    bubble.show("按调度弹一句", kPetBubbleHoldSec);
    DOCTEST_CHECK(bubble.visible);
    bubble.tick(kPetBubbleHoldSec + 0.01);
    DOCTEST_CHECK(!bubble.visible);
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

// 内置皮肤名（宏外声明：REGISTER_TEST 宏体内不能出现裸逗号）
const char* kNewBuiltinSkins[] = {"robot", "ghost"};

REGISTER_TEST(pet_builtin_skins, "pet: 两款内置皮肤（四态帧/互异/发现/短语表/替换保留）", {
    using namespace module::pet;
    const PetsDirEnv pets("builtin_skins");
    const std::filesystem::path& root = pets.root;
    // ensureBuiltinSkins 生成全部 2 款（robot + ghost）
    const std::vector<std::string> generated = ensureBuiltinSkins(root);
    DOCTEST_CHECK_EQ(generated.size(), static_cast<size_t>(2));
    // discoverSkins 发现全部 2 款（设置页下拉/右键换肤菜单数据源，零改动）
    const std::vector<std::string> found = discoverSkins();
    DOCTEST_CHECK_EQ(found.size(), static_cast<size_t>(2));
    for (const std::string& name : builtinSkinNames()) {
        DOCTEST_CHECK(std::find(found.begin(), found.end(), name) != found.end());
    }
    // 两款内置皮肤全部载入
    std::vector<PetSkin> skins;
    for (const char* name : kNewBuiltinSkins) {
        auto r = loadSkin(root / name);
        DOCTEST_CHECK_MESSAGE(r.isOk(), r.isFail() ? r.errorMessage() : name);
        if (r.isOk()) {
            skins.push_back(std::move(r.unsafeValue()));
        }
    }
    DOCTEST_CHECK_EQ(skins.size(), static_cast<size_t>(2));
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
    // 两款互异：idle 首帧 PNG 原始字节两两不同（stored-deflate 不压缩，
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
    // 两款短语表互异（性格区分）
    for (size_t i = 0; i < skins.size(); ++i) {
        for (size_t j = i + 1; j < skins.size(); ++j) {
            DOCTEST_CHECK(skins[i].phrases != skins[j].phrases);
        }
    }
    // 用户替换语义：robot 换成美术包（source 非内置标记）→ 原样保留
    {
        std::ofstream out(root / "robot" / "pet.json", std::ios::trunc);
        out << "{\"name\":\"robot\",\"source\":\"artist\","
               "\"states\":{\"idle\":[\"a0.png\"]}}";
    }
    const std::vector<std::string> again = ensureBuiltinSkins(root);
    DOCTEST_CHECK_EQ(again.size(), static_cast<size_t>(2));
    auto replaced = loadSkin(root / "robot");
    DOCTEST_CHECK(replaced.isOk());
    DOCTEST_CHECK_EQ(replaced.unsafeValue().idleFrames.size(), static_cast<size_t>(1));
    return true;
});

// ============ 10. （已移除）内置美术皮肤 anime-girl 测试 ============
// 2026-09-25：用户要求只留 robot/ghost + 本地 tatsu-pet，anime-girl CC0
// 美术皮肤已从 assets/pets 删除，本 CC0 四态断言用例随之移除。

REGISTER_TEST(pet_discover_skins, "pet: 皮肤发现（下拉/换肤菜单的数据源）", {
    using namespace module::pet;
    const PetsDirEnv pets("discover");
    const std::filesystem::path& root = pets.root;
    // 空目录：无皮肤
    std::filesystem::create_directories(root);
    DOCTEST_CHECK(discoverSkins().empty());
    // 生成默认皮肤 + 手工放第二个皮肤包 → 都被发现
    DOCTEST_CHECK(ensureDefaultSkin(root / "robot").isOk());
    std::filesystem::create_directories(root / "pixel-dog");
    {
        std::ofstream out(root / "pixel-dog" / "pet.json", std::ios::trunc);
        out << "{\"name\":\"pixel-dog\",\"states\":{\"idle\":[\"d0.png\"]}}";
    }
    const std::vector<std::string> skins = discoverSkins();
    DOCTEST_CHECK_EQ(skins.size(), static_cast<size_t>(2));
    DOCTEST_CHECK(std::find(skins.begin(), skins.end(), "robot") !=
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
    return true;
});

REGISTER_TEST(pet_apply_skin_and_reset, "pet: applyPetSkin 校验/持久化 + 位置重置", {
    using namespace module::pet;
    const PetsDirEnv pets("applyskin");
    const std::filesystem::path& root = pets.root;
    DOCTEST_CHECK(ensureDefaultSkin(root / "robot").isOk());
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
        cfg.skinName = "robot";
        cfg.x = 500;
        cfg.y = 600;
        DOCTEST_CHECK(store.save(cfg).isOk());
    }
    // 未发现的皮肤名被拒绝，配置不动
    applyPetSkin("no-such-skin");
    {
        const PetConfig cfg = PetConfigStore(cfgFile).load();
        DOCTEST_CHECK_EQ(cfg.skinName, "robot");
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

// ---- 审计 [25]：退出二段确认 ----

// 嵌套路径构造（宏体裸逗号限制，见文件头注释）
static std::vector<int> petExitNestedPath() {
    std::vector<int> p;
    p.push_back(2);
    p.push_back(1);
    return p;
}

REGISTER_TEST(pet_exit_confirm_menu, "pet: 退出确认菜单文本结构（审计[25]）", {
    const auto items = module::pet::petExitConfirmItems();
    DOCTEST_REQUIRE_EQ(items.size(), static_cast<size_t>(4));
    // 0=影响说明（不可点）：必须讲清断代理 + 丢弹窗状态两个后果
    DOCTEST_CHECK(items[0].disabled);
    DOCTEST_CHECK(items[0].text.find("将退出 DevDesk") != std::string::npos);
    DOCTEST_CHECK(items[0].text.find("断开代理") != std::string::npos);
    DOCTEST_CHECK(items[0].text.find("未保存弹窗状态") != std::string::npos);
    DOCTEST_CHECK(items[0].text.find("确定") != std::string::npos);
    // 1=分隔线 2=确认 3=取消（下标与 petExitConfirmChosen 耦合）
    DOCTEST_CHECK_EQ(items[1].text, std::string("-"));
    DOCTEST_CHECK_EQ(items[2].text, std::string("确认退出"));
    DOCTEST_CHECK(!items[2].disabled);
    DOCTEST_CHECK_EQ(items[3].text, std::string("取消"));
    DOCTEST_CHECK(!items[3].disabled);
    return true;
})

REGISTER_TEST(pet_exit_confirm_decision, "pet: 只有「确认退出」路径触发退出（审计[25]）", {
    using module::pet::petExitConfirmChosen;
    // 确认退出（下标 2）→ 退
    DOCTEST_CHECK(petExitConfirmChosen(std::vector<int>{2}));
    // 取消 / 点中说明行 / 嵌套路径（防御）→ 不退
    DOCTEST_CHECK(!petExitConfirmChosen(std::vector<int>{3}));
    DOCTEST_CHECK(!petExitConfirmChosen(std::vector<int>{0}));
    DOCTEST_CHECK(!petExitConfirmChosen(std::vector<int>{1}));
    DOCTEST_CHECK(!petExitConfirmChosen(petExitNestedPath()));
    // 空选择（Esc/点别处/平台不支持）→ 不退
    DOCTEST_CHECK(!petExitConfirmChosen(std::vector<int>{}));
    return true;
})

// ============ 12. M2 收尾：多显示器钳制 + Sleep 零渲染（2026-09-23 pet-m2）============

namespace {

// 工作区 mock 构造（REGISTER_TEST 宏体不能有含逗号的初始化列表，照
// frameNameRejected 先例拆出）
module::pet::PetWorkArea mockArea(int x, int y, int w, int h) {
    module::pet::PetWorkArea a;
    a.x = x;
    a.y = y;
    a.width = w;
    a.height = h;
    return a;
}

// 左副屏布局：主屏 [0,1920) + 副屏 [-1920,0)（负坐标区）
std::vector<module::pet::PetWorkArea> dualMonitorLeft() {
    std::vector<module::pet::PetWorkArea> v;
    v.push_back(mockArea(0, 0, 1920, 1080));
    v.push_back(mockArea(-1920, 0, 1920, 1080));
    return v;
}

// 右副屏布局：主屏 [0,1920) + 副屏 [1920,3840)
std::vector<module::pet::PetWorkArea> dualMonitorRight() {
    std::vector<module::pet::PetWorkArea> v;
    v.push_back(mockArea(0, 0, 1920, 1080));
    v.push_back(mockArea(1920, 0, 1920, 1080));
    return v;
}

// 仅主屏（M1 形态回归基准）
std::vector<module::pet::PetWorkArea> primaryOnly() {
    std::vector<module::pet::PetWorkArea> v;
    v.push_back(mockArea(0, 0, 1920, 1080));
    return v;
}

} // namespace

REGISTER_TEST(pet_multi_monitor_clamp, "pet: 多显示器按位置选屏钳制（M2 任务A）", {
    using module::pet::clampPetPosToMonitors;
    // 副屏内合法负坐标：不动（M1 只按主屏钳会把 -1500 钳回 0 = 跳回主屏）
    const auto onSecondary = clampPetPosToMonitors(-1500, 500, dualMonitorLeft(), 128, 128);
    DOCTEST_CHECK_EQ(onSecondary.first, -1500);
    DOCTEST_CHECK_EQ(onSecondary.second, 500);
    // 超出左副屏左缘 → 钳到副屏工作区原点（-1920），不是主屏 0
    const auto pastLeft = clampPetPosToMonitors(-3000, 500, dualMonitorLeft(), 128, 128);
    DOCTEST_CHECK_EQ(pastLeft.first, -1920);
    DOCTEST_CHECK_EQ(pastLeft.second, 500);
    // 副屏纵向越界 → 钳副屏内（1080-128），横坐标原样
    const auto secondaryOverflow = clampPetPosToMonitors(-1500, 99999, dualMonitorLeft(), 128, 128);
    DOCTEST_CHECK_EQ(secondaryOverflow.first, -1500);
    DOCTEST_CHECK_EQ(secondaryOverflow.second, 952);
    // 右副屏内：正坐标超出主屏边界同样不动
    const auto onRight = clampPetPosToMonitors(2500, 300, dualMonitorRight(), 128, 128);
    DOCTEST_CHECK_EQ(onRight.first, 2500);
    DOCTEST_CHECK_EQ(onRight.second, 300);
    // 两块屏都不含的远处（x=99999）→ 取最近屏（右副屏）钳 3840-128=3712，
    // 不是主屏的 1792（MONITOR_DEFAULTTONEAREST 语义）
    const auto farRight = clampPetPosToMonitors(99999, 300, dualMonitorRight(), 128, 128);
    DOCTEST_CHECK_EQ(farRight.first, 3712);
    DOCTEST_CHECK_EQ(farRight.second, 300);
    // M1 行为回归：仅主屏枚举时负坐标仍钳回主屏原点（单机无副屏用户零变化）
    const auto legacy = clampPetPosToMonitors(-50, -80, primaryOnly(), 128, 128);
    DOCTEST_CHECK_EQ(legacy.first, 0);
    DOCTEST_CHECK_EQ(legacy.second, 0);
    // 空枚举（无头兜底）→ 原样
    const auto headless = clampPetPosToMonitors(-50, -60, std::vector<module::pet::PetWorkArea>{}, 128, 128);
    DOCTEST_CHECK_EQ(headless.first, -50);
    DOCTEST_CHECK_EQ(headless.second, -60);
    // 无效块（宽高 0）不参与选屏：[无效, 主屏] 布局下仍按主屏钳
    std::vector<module::pet::PetWorkArea> withInvalid;
    withInvalid.push_back(mockArea(5000, 5000, 0, 0));
    withInvalid.push_back(mockArea(0, 0, 1920, 1080));
    const auto skipInvalid = clampPetPosToMonitors(99999, 300, withInvalid, 128, 128);
    DOCTEST_CHECK_EQ(skipInvalid.first, 1792);
    return true;
});

REGISTER_TEST(pet_negative_coord_persist, "pet: 副屏负坐标落盘往返（M2 任务A 哨兵）", {
    using namespace module::pet;
    const std::filesystem::path file =
        test_temp::tempPath("devdesk_test_pet_config_neg.json");
    std::error_code ec;
    std::filesystem::remove(file, ec);
    PetConfigStore store(file);
    // 左副屏坐标落盘 → 读回原值（旧 sanitize 的 x<-1→-1 会把它吞成「未设置」）
    PetConfig cfg;
    cfg.skinName = "robot";
    cfg.x = -1500;
    cfg.y = 500;
    DOCTEST_CHECK(store.save(cfg).isOk());
    const PetConfig loaded = store.load();
    DOCTEST_CHECK_EQ(loaded.x, -1500);
    DOCTEST_CHECK_EQ(loaded.y, 500);
    // -1「未设置」哨兵语义保持：单边独立、往返不变
    PetConfig unsetCfg;  // 默认 -1/-1
    DOCTEST_CHECK_EQ(PetConfigStore::sanitized(unsetCfg).x, -1);
    unsetCfg.x = 100;
    unsetCfg.y = -1;
    const PetConfig half = PetConfigStore::sanitized(unsetCfg);
    DOCTEST_CHECK_EQ(half.x, 100);
    DOCTEST_CHECK_EQ(half.y, -1);
    // 乱值钳制方向对称：负向也收在 int16 量级（-99999 → -32767）
    PetConfig wild;
    wild.x = -99999;
    wild.y = -99999;
    DOCTEST_CHECK_EQ(PetConfigStore::sanitized(wild).x, -32767);
    DOCTEST_CHECK_EQ(PetConfigStore::sanitized(wild).y, -32767);
    std::filesystem::remove(file, ec);
    return true;
});

// Sleep 零渲染（任务B）：无头可断言的是「暂停门」——compose 入口唯一的读值
// isPetRenderPaused()（true 即整帧 return，不重建成/不推动画/不跑计时器），
// 以及电源回调写入方与读取方共用同一标志。WM_POWERBROADCAST → 回调 → 标志
// 的真实链路属真机点验（EUI-NEO power_events 只在 Windows 消息泵触发）。
REGISTER_TEST(pet_sleep_render_pause, "pet: Sleep 零渲染暂停门（M2 任务B）", {
    using module::pet::isPetRenderPaused;
    using module::pet::setPetRenderPaused;
    // 默认不暂停（首帧正常 compose）
    DOCTEST_CHECK(!isPetRenderPaused());
    // 挂起回调侧：置位 → compose 跳帧门生效
    setPetRenderPaused(true);
    DOCTEST_CHECK(isPetRenderPaused());
    // 唤醒回调侧：清位 → 恢复渲染
    setPetRenderPaused(false);
    DOCTEST_CHECK(!isPetRenderPaused());
    // 重复挂起/唤醒幂等（PBT_APMRESUMEAUTOMATIC 与 RESUMESUSPEND 先后到也只收敛到 false）
    setPetRenderPaused(true);
    setPetRenderPaused(true);
    setPetRenderPaused(false);
    setPetRenderPaused(false);
    DOCTEST_CHECK(!isPetRenderPaused());
    return true;
});

// ---- 审计 [5][36]：设置页解耦只读桥 currentSkinName ----
// settings 不再 include pet 头也不 link pet，桌宠卡初值改走该标量只读
// 函数（盘即真值：apply 路径双写运行时 + pet_config.json）。本用例锁桥语义：
// 无盘 → 默认值；store 写皮肤 → 桥跟随。（挂机穿透桥 currentClickThrough
// 已于 2026-09-24 随功能移除）
REGISTER_TEST(pet_bridge_readonly_accessors,
              "pet: currentSkinName 只读桥（审计 [5][36] 设置页解耦）", {
    PetsDirEnv env("bridge");
    using namespace module::pet;

    // pet_config.json 不存在 → PetConfig 默认值
    DOCTEST_CHECK_EQ(currentSkinName(), std::string(kDefaultSkinName));

    // 换皮肤名走盘 store（applyPetSkin 需 discoverSkins 命中，桥只读同一张盘）
    PetConfig cfg;
    cfg.skinName = "bridge-skin";
    DOCTEST_CHECK(!PetConfigStore(petConfigFile()).save(cfg).isFail());
    DOCTEST_CHECK_EQ(currentSkinName(), std::string("bridge-skin"));
    return true;
});

// ---- 审计 THS-03 第二批：pet 侧两个写点的原子写收口 ----
// 失败注入照上一批（test_view_state.cpp）约定：把临时文件路径 <目标>.devdesk-tmp
// 用目录占住（打开必失败），不 sleep、不杀进程，且可顺带断言「失败时旧内容一字未变」。

namespace {

std::string readAllBytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void writeTextFile(const std::filesystem::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

std::filesystem::path tmpPathOf(const std::filesystem::path& p) {
    return std::filesystem::path(p.string() + ".devdesk-tmp");
}

} // namespace

REGISTER_TEST(pet_config_save_failure_keeps_old, "pet: 配置原子写失败不破坏旧文件（THS-03）", {
    using namespace module::pet;
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "devdesk_test_pet_config_atomic.json";
    const std::filesystem::path tmp = tmpPathOf(file);
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::remove(file, ec);

    PetConfigStore store(file);
    PetConfig cfg;
    cfg.skinName = "my-cat";
    cfg.x = 1600;
    cfg.y = 800;
    DOCTEST_REQUIRE(store.save(cfg).isOk());
    // 成功后字节与旧实现一致：serialize 结果 + "\n" 尾
    const std::string bytes = readAllBytes(file);
    DOCTEST_CHECK(!bytes.empty());
    DOCTEST_CHECK_EQ(bytes.back(), '\n');

    // 落盘通道被占住 → 保存失败，但上一次成功落盘的配置必须原样可读
    DOCTEST_REQUIRE(std::filesystem::create_directory(tmp, ec));
    PetConfig changed;
    changed.skinName = "robot";
    changed.x = 0;
    changed.y = 0;
    DOCTEST_CHECK(store.save(changed).isFail());

    PetConfigStore reopened(file);
    const PetConfig loaded = reopened.load();
    DOCTEST_CHECK_EQ(loaded.skinName, std::string("my-cat"));
    DOCTEST_CHECK_EQ(loaded.x, 1600);
    DOCTEST_CHECK_EQ(loaded.y, 800);

    std::filesystem::remove(tmp, ec);
    std::filesystem::remove(file, ec);
    return true;
});

REGISTER_TEST(pet_skin_json_write_failure_keeps_old, "pet: 皮肤清单原子写失败不破坏旧 pet.json（THS-03）", {
    using namespace module::pet;
    const std::filesystem::path dir = tempDir("skin_json_fail");
    removeTreeQuietly(dir);
    std::error_code ec;
    DOCTEST_REQUIRE(std::filesystem::create_directories(dir, ec));
    // 旧版内置皮肤（source 是内置标记、缺 M2 帧）→ ensureDefaultSkin 走重生成升级，
    // 盘上现存的 pet.json 即「旧内容」
    const std::string oldJson =
        "{\"name\":\"robot\",\"source\":\"DevDesk 内置生成\","
        "\"states\":{\"idle\":[\"f0.png\"]}}";
    writeTextFile(dir / "pet.json", oldJson);
    // 清单落盘通道被目录占住（帧文件通道是空的，PNG 照常写出）→ 最后一步 pet.json 失败
    DOCTEST_REQUIRE(std::filesystem::create_directory(tmpPathOf(dir / "pet.json"), ec));
    const auto r = ensureDefaultSkin(dir);
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK_EQ(readAllBytes(dir / "pet.json"), oldJson);  // 旧清单一字未变

    std::filesystem::remove_all(tmpPathOf(dir / "pet.json"), ec);
    removeTreeQuietly(dir);
    return true;
});

REGISTER_TEST(pet_skin_frame_write_failure_keeps_old, "pet: 皮肤帧原子写失败不破坏旧帧（THS-03）", {
    using namespace module::pet;
    const std::filesystem::path dir = tempDir("skin_frame_fail");
    removeTreeQuietly(dir);
    std::error_code ec;
    DOCTEST_REQUIRE(std::filesystem::create_directories(dir, ec));
    writeTextFile(dir / "pet.json",
                  "{\"name\":\"robot\",\"source\":\"DevDesk 内置生成\","
                  "\"states\":{\"idle\":[\"f0.png\"]}}");
    const std::string oldFrame = "OLD-FRAME-BYTES";
    writeTextFile(dir / "f0.png", oldFrame);
    // 首帧通道被占住 → 重生成在第一帧就失败（旧实现 trunc 原地写会先毁掉旧帧）
    DOCTEST_REQUIRE(std::filesystem::create_directory(tmpPathOf(dir / "f0.png"), ec));
    const auto r = ensureDefaultSkin(dir);
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK_EQ(readAllBytes(dir / "f0.png"), oldFrame);  // 旧帧一字未变

    std::filesystem::remove_all(tmpPathOf(dir / "f0.png"), ec);
    removeTreeQuietly(dir);
    return true;
});

// ---- PAB-12（波 1 L7）：气泡短语随机源 ----
// 旧实现 std::rand 从未 srand = 每次启动同一序列（且短语顺序固定）。
// 单进程内只能锁「随机源活着」：64 次抽样须出 ≥2 种短语，且每个结果都
// 落在短语表内（不越界、不空串）。跨进程不再同序列无法在本用例断言
REGISTER_TEST(pet_phrase_randomized, "pet: 气泡短语随机化（64 次抽样出多种）", {
    using namespace module::pet;
    auto parsed = parsePetSkinJson(kM2SkinJson);
    DOCTEST_REQUIRE(parsed.isOk());
    const PetSkin& skin = parsed.unsafeValue();
    DOCTEST_REQUIRE(skin.phrases.size() >= static_cast<size_t>(2));

    std::set<std::string> seen;
    for (int i = 0; i < 64; ++i) {
        seen.insert(pickPetPhrase(skin));
    }
    DOCTEST_CHECK(seen.size() >= static_cast<size_t>(2));
    for (const std::string& phrase : seen) {
        DOCTEST_CHECK(std::find(skin.phrases.begin(), skin.phrases.end(),
                                phrase) != skin.phrases.end());
    }
    return true;
});

// ---- PAB-13（波 1 L7）：设置页「显示桌宠」桥 ----
// 无头：handle 恒为 nullptr（onWindowCreated 只在主循环里回填），故这里锁
// 「已装配过的会话 → showPetWindow 重新入队一条开窗请求，几何沿用 rt.config」
REGISTER_TEST(pet_show_window_reenqueues,
              "pet: showPetWindow 重新入队开窗请求（PAB-13）", {
    const PetsDirEnv pets("showwin");
    (void)app::consumeWindowRequests();  // 清残留

    // 先装配一次（= main 模块门控通过；同时把 windowRequested 置位）
    module::pet::startPetWindow(module::pet::PetWindowConfig{"", 120, 130});
    const std::vector<app::DslWindowRequest> first = app::consumeWindowRequests();
    DOCTEST_REQUIRE_EQ(first.size(), static_cast<size_t>(1));

    // 重新显示：应再入队一条（皮肤/位置沿用 rt.config）
    DOCTEST_CHECK(module::pet::showPetWindow());
    const std::vector<app::DslWindowRequest> second = app::consumeWindowRequests();
    DOCTEST_REQUIRE_EQ(second.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(second[0].pageId, "pet");
    DOCTEST_CHECK_EQ(second[0].width, 128);
    DOCTEST_CHECK_EQ(second[0].height, 128);
    DOCTEST_CHECK(second[0].positionSet);
    DOCTEST_CHECK_EQ(second[0].x, 120);
    DOCTEST_CHECK_EQ(second[0].y, 130);
    return true;
});
