// tests/test_modules/test_pet.cpp
// 桌宠模块单测：皮肤解析 / 帧序列状态机 / 开窗请求 / 气泡 / 多显示器钳制 / 拖拽锚点。
// 跨文件前提：
//   - pet 配置存 app_config 表（不是文件），经 core::appDatabase() 取连接；用例必须注册应用库。
//   - 皮肤候选根由 skinRootCandidates 纯派生；随包资产不在测试 exeDir 内，须用夹具模拟。
//   - 无头环境没有工作区也没有窗口 handle，位置与开窗断言只能到请求字段为止。
#include "doctest.h"
#include "test_support.h"

#include "module_pet/pet_bubble.h"
#include "module_pet/pet_config.h"
#include "module_pet/pet_frame_player.h"
#include "module_pet/pet_skin.h"
#include "module_pet/pet_window.h"
#include "settings_config.h"

#include "core/paths.h"
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

// makeTempDir 先清残骸再建空目录；调用点随后的 remove / 建目录保持原样
constexpr const char* kPetDirPrefix = "devdesk_test_pet_";

void removeTreeQuietly(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
}

// DEVDESK_PETS_DIR 隔离 + 应用库注册的 RAII 守卫。
// registerAppDb 不可去掉：pet 各桥的 save 经 core::appDatabase() 取连接，库未注册时
// save 会因库未就绪如实失败，整组用例必红。
// 析构顺序不可换：先注销 appDatabase，再删库文件——句柄还开着时删文件会失败。
struct PetsDirEnv {
    const std::filesystem::path root;
    test_support::TestEnv db;
    explicit PetsDirEnv(const std::string& tag)
        : root(test_support::makeTempDir(kPetDirPrefix, tag.c_str())),
          db("devdesk_test_pet_", tag, /*registerAppDb=*/true) {
        removeTreeQuietly(root);
        test_support::setEnvVar("DEVDESK_PETS_DIR", root.string());
    }
    PetsDirEnv(const PetsDirEnv&) = delete;
    PetsDirEnv& operator=(const PetsDirEnv&) = delete;
    ~PetsDirEnv() {
        test_support::setEnvVar("DEVDESK_PETS_DIR", "");
        removeTreeQuietly(root);
    }
    core::data::SqliteDb& handle() { return db.handle(); }
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

    // 四态齐全的皮肤（sleep/typing/phrases）
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

// 随包皮肤夹具：测试 exeDir（build/tests）没有 assets/pets 拷贝（POST_BUILD 只拷给
// DevDesk 目标），故以 petsDir 候选根下的 shipped 形态皮肤模拟随包加载路径。
// frames 名与真实 assets/pets 逐字一致。
const char* kRobotFixtureJson =
    "{"
    "  \"name\": \"robot\","
    "  \"frameSize\": [128, 128],"
    "  \"frameRate\": 4,"
    "  \"phrases\": [\"滴——系统正常\", \"电量 87%\", \"扫描中…\"],"
    "  \"states\": {"
    "    \"idle\": [\"f0.png\", \"f1.png\", \"f2.png\", \"f3.png\", \"f4.png\", \"f5.png\"],"
    "    \"poked\": [\"p0.png\", \"p1.png\"],"
    "    \"sleep\": [\"s0.png\", \"s1.png\", \"s2.png\"],"
    "    \"typing\": [\"t0.png\", \"t1.png\"]"
    "  }"
    "}";

const char* kGhostFixtureJson =
    "{"
    "  \"name\": \"ghost\","
    "  \"frameSize\": [128, 128],"
    "  \"frameRate\": 4,"
    "  \"phrases\": [\"呼~…\", \"别怕，我很友好\", \"漂浮中…\"],"
    "  \"states\": {"
    "    \"idle\": [\"f0.png\", \"f1.png\", \"f2.png\", \"f3.png\", \"f4.png\", \"f5.png\"],"
    "    \"poked\": [\"p0.png\", \"p1.png\"],"
    "    \"sleep\": [\"s0.png\", \"s1.png\", \"s2.png\"],"
    "    \"typing\": [\"t0.png\", \"t1.png\"]"
    "  }"
    "}";


} // namespace


REGISTER_TEST(pet_skin_parse_valid, "pet: pet.json 合法解析", {
    auto r = module::pet::parsePetSkinJson(kValidSkinJson);
    DOCTEST_CHECK(r.isOk());
    const module::pet::PetSkin& skin = r.value();
    DOCTEST_CHECK_EQ(skin.name, "clay-cat");
    DOCTEST_CHECK_EQ(skin.frameWidth, 128);
    DOCTEST_CHECK_EQ(skin.frameHeight, 128);
    DOCTEST_CHECK_EQ(skin.frameRate, 4.0);
    DOCTEST_CHECK_EQ(skin.idleFrames.size(), static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(skin.idleFrames[0], "f0.png");
    DOCTEST_CHECK_EQ(skin.pokedFrames.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(skin.license, "CC0");
    // framePath 以 s.dir 为前缀；dir 由 loadSkin 填入
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
// 越界值不 clamp，一律回落默认（4 / 128）
    const std::string json =
        "{\"frameRate\": 999, \"frameSize\": [8, 9999],"
        " \"states\": {\"idle\": [\"f.png\"]}}";
    auto r = module::pet::parsePetSkinJson(json);
    DOCTEST_CHECK(r.isOk());
    DOCTEST_CHECK_EQ(r.value().frameRate, 4.0);
    DOCTEST_CHECK_EQ(r.value().frameWidth, 128);
    DOCTEST_CHECK_EQ(r.value().frameHeight, 128);
    // poked 可选：缺失 → pokedFrames 空，frames(true) 回落 idle
    DOCTEST_CHECK(r.value().pokedFrames.empty());
    DOCTEST_CHECK_EQ(r.value().frames(true).size(), static_cast<size_t>(1));
    return true;
});


REGISTER_TEST(pet_anim_idle_cycle, "pet: idle 帧循环推进", {
    auto r = module::pet::parsePetSkinJson(kValidSkinJson);
    DOCTEST_CHECK(r.isOk());
    module::pet::PetAnimation anim;
    DOCTEST_CHECK(anim.state == module::pet::PetAnimState::Idle);
    DOCTEST_CHECK_EQ(anim.currentPath(r.value()).filename().string(), "f0.png");
    anim.tick(r.value());
    DOCTEST_CHECK_EQ(anim.frame, 1);
    anim.tick(r.value());
    anim.tick(r.value());
    DOCTEST_CHECK_EQ(anim.frame, 0);  // 3 帧循环回卷
    return true;
});

REGISTER_TEST(pet_anim_poke_once, "pet: poke 反应帧播一遍回 Idle", {
    auto r = module::pet::parsePetSkinJson(kValidSkinJson);
    DOCTEST_CHECK(r.isOk());
    const module::pet::PetSkin& skin = r.value();
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
    anim.poke(r.value());
    DOCTEST_CHECK(anim.state == module::pet::PetAnimState::Idle);
    return true;
});


REGISTER_TEST(pet_default_skin_shipped, "pet: 默认皮肤加载随包 robot（缺失返回 fail）", {
    using namespace module::pet;
    const PetsDirEnv pets("defaultskin");
    const std::filesystem::path& root = pets.root;
    // 无候选根含 robot → fail；ensureDefaultSkin 不写盘生成
    const auto missing = ensureDefaultSkin();
    DOCTEST_CHECK(missing.isFail());
    test_support::writeShippedSkinFixture(root, "robot", kRobotFixtureJson);
    const auto r = ensureDefaultSkin();
    DOCTEST_CHECK_MESSAGE(r.isOk(), r.isFail() ? r.errorMessage() : "");
    const module::pet::PetSkin& skin = r.value();
    DOCTEST_CHECK_EQ(skin.frameWidth, 128);
    DOCTEST_CHECK_EQ(skin.frameHeight, 128);
    DOCTEST_CHECK_EQ(skin.frameRate, 4.0);
    DOCTEST_CHECK(skin.idleFrames.size() >= 3);
    DOCTEST_CHECK(skin.pokedFrames.size() >= 2);
    for (size_t i = 0; i < skin.idleFrames.size(); ++i) {
        DOCTEST_CHECK(pngLooksValid(skin.framePath(false, i), 128, 128));
    }
    for (size_t i = 0; i < skin.pokedFrames.size(); ++i) {
        DOCTEST_CHECK(pngLooksValid(skin.framePath(true, i), 128, 128));
    }
    // 重复调用结果一致（加载无副作用）
    const auto again = ensureDefaultSkin();
    DOCTEST_CHECK(again.isOk());
    DOCTEST_CHECK_EQ(again.value().idleFrames.size(), skin.idleFrames.size());
    return true;
});


REGISTER_TEST(pet_gating_defaults, "pet: 默认关闭、已知 ID 可持久化", {
    using namespace module::settings;
    SettingsConfig cfg;
    // 默认不含 pet（氛围功能默认关）
    DOCTEST_CHECK(!moduleEnabled(cfg, kModuleIdPet));
    DOCTEST_CHECK(std::find(cfg.enabledModules.begin(), cfg.enabledModules.end(),
                            std::string(kModuleIdPet)) == cfg.enabledModules.end());
    cfg.enabledModules.push_back(kModuleIdPet);
    const SettingsConfig fixed = SettingsStore::sanitized(cfg);
    DOCTEST_CHECK(moduleEnabled(fixed, kModuleIdPet));
    // 默认模块清单 5 项（agent 默认关闭）
    DOCTEST_CHECK_EQ(SettingsConfig{}.enabledModules.size(), static_cast<size_t>(5));
    return true;
});

REGISTER_TEST(pet_settings_roundtrip, "pet: pet_config.json 读写往返 + 钳制", {
// pet 配置存 app_config 表，经 core::appDatabase() 取连接；未注册则 save() 如实失败。
    test_support::TestEnv cfgDbEnv("devdesk_test_pet_", "petcfg1", /*registerAppDb=*/true);
    using namespace module::pet;
    PetConfigStore store(::core::appDatabase());
    PetConfig cfg;
    cfg.skinName = "my-cat";
    cfg.x = 1600;
    cfg.y = 800;
    DOCTEST_CHECK(store.save(cfg).isOk());
    const PetConfig loaded = store.load();
    DOCTEST_CHECK_EQ(loaded.skinName, "my-cat");
    DOCTEST_CHECK_EQ(loaded.x, 1600);
    DOCTEST_CHECK_EQ(loaded.y, 800);
    // 皮肤名白名单：路径分隔符 / 空 → 收敛默认 robot；坐标钳制保留 -1 哨兵
    cfg.skinName = "../evil";
    cfg.x = 999999;
    const PetConfig sanitizedCfg = PetConfigStore::sanitized(cfg);
    DOCTEST_CHECK_EQ(sanitizedCfg.skinName, "robot");
    DOCTEST_CHECK_EQ(sanitizedCfg.x, 32767);
    DOCTEST_CHECK_EQ(PetConfigStore::sanitized(PetConfig{}).x, -1);
    return true;
});


REGISTER_TEST(pet_start_window_request, "pet: 开窗请求逐字段断言（无头）", {
    const PetsDirEnv pets("petsroot");
    const std::filesystem::path& petsRoot = pets.root;
    // startPetWindow 不写盘生成皮肤，夹具须先备在候选根
    (void)app::consumeWindowRequests();
    test_support::writeShippedSkinFixture(petsRoot, "robot", kRobotFixtureJson);

    module::pet::startPetWindow(module::pet::PetWindowConfig{
        "", 50, 60});  // 皮肤空 = 默认；位置已知
    std::vector<app::DslWindowRequest> requests = app::consumeWindowRequests();
    DOCTEST_CHECK_EQ(requests.size(), static_cast<size_t>(1));
    const app::DslWindowRequest& req = requests[0];
    DOCTEST_CHECK_EQ(req.pageId, "pet");
    DOCTEST_CHECK_EQ(req.width, 128);   // 默认帧 128×128 不放大
    // 窗口高 = 帧高 + 气泡让位条 kBubbleStripH
    DOCTEST_CHECK_EQ(req.height, 128 + static_cast<int>(module::pet::kBubbleStripH));
    DOCTEST_CHECK(req.positionSet);
    DOCTEST_CHECK_EQ(req.x, 50);
    DOCTEST_CHECK_EQ(req.y, 60);
    DOCTEST_CHECK(!req.decorated);            // 无边框
    DOCTEST_CHECK(req.alwaysOnTop);           // 置顶
    DOCTEST_CHECK(!req.resizable);
    DOCTEST_CHECK(!req.focusOnShow);          // 不抢焦点
    DOCTEST_CHECK(req.hideFromTaskbar);       // 不进任务栏/Alt+Tab
    DOCTEST_CHECK(req.transparentFramebuffer);  // 逐像素透明：不随全局窗口效果档位回落 None
    DOCTEST_CHECK(!req.mousePassthrough);     // 默认不穿透
    DOCTEST_CHECK(!req.followClearColorOverride);  // 全透底不被主题广播冲掉
    DOCTEST_CHECK(req.clearColor.a == 0.0f);  // 全透
    DOCTEST_CHECK(static_cast<bool>(req.compose));
    // 创建 / 销毁回调随请求下传；销毁回调须置空缓存句柄（覆盖 Alt+F4 / 菜单隐藏 / 退出）
    DOCTEST_CHECK(static_cast<bool>(req.onWindowCreated));
    DOCTEST_CHECK(static_cast<bool>(req.onWindowDestroyed));
    // 无头环境启动前后不生成皮肤
    DOCTEST_CHECK(std::filesystem::exists(petsRoot / "robot" / "pet.json"));

    // 未知皮肤名 → 回退默认皮肤，窗口照常入队
    module::pet::startPetWindow(module::pet::PetWindowConfig{"no-such-skin", -1, -1});
    std::vector<app::DslWindowRequest> second = app::consumeWindowRequests();
    DOCTEST_CHECK_EQ(second.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(!second[0].mousePassthrough);   // 恒不穿透
    DOCTEST_CHECK(second[0].width == 128);       // 默认皮肤 128×128

// 单边 -1 哨兵：只重置未设置的那一边（无头无工作区 → 兜底 100）
    module::pet::startPetWindow(module::pet::PetWindowConfig{"", 77, -1});
    std::vector<app::DslWindowRequest> third = app::consumeWindowRequests();
    DOCTEST_CHECK_EQ(third.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(third[0].x, 77);   // 已知边原样保留
    DOCTEST_CHECK_EQ(third[0].y, 100);  // 未设置边走兜底

    return true;
});


REGISTER_TEST(pet_anim_sleep_cycle, "pet: 无交互超时入睡 + 交互唤醒", {
    using namespace module::pet;
    auto r = parsePetSkinJson(kM2SkinJson);
    DOCTEST_CHECK(r.isOk());
    const PetSkin& skin = r.value();
    PetAnimation anim;
    anim.considerSleep(kSleepAfterSeconds - 1.0, skin);
    DOCTEST_CHECK(anim.state == PetAnimState::Idle);
    anim.considerSleep(kSleepAfterSeconds, skin);
    DOCTEST_CHECK(anim.state == PetAnimState::Sleep);
    DOCTEST_CHECK_EQ(anim.currentPath(skin).filename().string(), "s0.png");
    anim.tick(skin);
    anim.tick(skin);
    DOCTEST_CHECK_EQ(anim.currentPath(skin).filename().string(), "s2.png");
    anim.tick(skin);  // sleep 循环回卷，不退出
    DOCTEST_CHECK(anim.state == PetAnimState::Sleep);
    DOCTEST_CHECK_EQ(anim.currentPath(skin).filename().string(), "s0.png");
    anim.poke(skin);
    DOCTEST_CHECK(anim.state == PetAnimState::Idle);
    DOCTEST_CHECK_EQ(anim.currentPath(skin).filename().string(), "f0.png");
    // 无 sleep 帧的皮肤永不入睡（睡了也看不出来）
    const PetSkin noSleepSkin = parsePetSkinJson(kValidSkinJson).value();
    PetAnimation noSleepAnim;
    noSleepAnim.considerSleep(kSleepAfterSeconds * 10.0, noSleepSkin);
    DOCTEST_CHECK(noSleepAnim.state == PetAnimState::Idle);
    return true;
});

REGISTER_TEST(pet_anim_typing, "pet: typing 态进出 + 帧回落 + 优先级", {
    using namespace module::pet;
    auto r = parsePetSkinJson(kM2SkinJson);
    DOCTEST_CHECK(r.isOk());
    const PetSkin& skin = r.value();
    PetAnimation anim;
    anim.setStreaming(true, skin);
    DOCTEST_CHECK(anim.state == PetAnimState::Typing);
    DOCTEST_CHECK_EQ(anim.currentPath(skin).filename().string(), "t0.png");
    anim.tick(skin);
    DOCTEST_CHECK_EQ(anim.currentPath(skin).filename().string(), "t1.png");
    anim.tick(skin);  // typing 循环不退出
    DOCTEST_CHECK(anim.state == PetAnimState::Typing);
    anim.setStreaming(false, skin);
    DOCTEST_CHECK(anim.state == PetAnimState::Idle);
    // poke 仅 Idle 生效，typing 中点击不重置
    anim.setStreaming(true, skin);
    anim.poke(skin);
    DOCTEST_CHECK(anim.state == PetAnimState::Typing);
    // 状态优先级：流式 > 入睡，Sleep 中来流转 Typing
    PetAnimation sleeper;
    sleeper.considerSleep(kSleepAfterSeconds, skin);
    DOCTEST_CHECK(sleeper.state == PetAnimState::Sleep);
    sleeper.setStreaming(true, skin);
    DOCTEST_CHECK(sleeper.state == PetAnimState::Typing);
    // 无 typing 帧：状态语义仍在，视觉回落 idle 帧
    const PetSkin plainSkin = parsePetSkinJson(kValidSkinJson).value();
    PetAnimation plain;
    plain.setStreaming(true, plainSkin);
    DOCTEST_CHECK(plain.state == PetAnimState::Typing);
    DOCTEST_CHECK_EQ(plain.currentPath(plainSkin).filename().string(), "f0.png");
    return true;
});

// pet 状态机吃 core::proxy_stats 快照；测试时钟注入
REGISTER_TEST(pet_anim_proxy_stats_linkage, "pet: proxy_stats 活动流驱动 typing", {
    using namespace module::pet;
    core::proxy_stats::resetForTest();
    int64_t nowMs = 1000;
    core::proxy_stats::setTestClock([&nowMs] { return nowMs; });
    auto r = parsePetSkinJson(kM2SkinJson);
    DOCTEST_CHECK(r.isOk());
    const PetSkin& skin = r.value();
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
    DOCTEST_CHECK_EQ(r.value().phrases.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(phraseTable(r.value()).size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(phraseTable(r.value())[0], "你好呀");
    // 无 phrases 皮肤 → 内置语料（非空）
    auto plain = parsePetSkinJson(kValidSkinJson);
    DOCTEST_CHECK(plain.value().phrases.empty());
    DOCTEST_CHECK(!phraseTable(plain.value()).empty());
    // phrases 含非字符串成员 → 静默忽略，回退内置语料
    auto bad = parsePetSkinJson(
        "{\"phrases\":[42],\"states\":{\"idle\":[\"f.png\"]}}");
    DOCTEST_CHECK(bad.isOk());
    DOCTEST_CHECK(bad.value().phrases.empty());
    return true;
});

// 内容源②：逾期计数桥三分支——N>0 计数文案 / N=0 正面文案不带死数字 /
// 桥空或 nullopt → 空串，由调用方回退内容源①
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
    // 桥返回 nullopt（todo 关 / 查询失败）→ 空串 = 回退①
    std::function<std::optional<int64_t>()> unavailable =
        [] { return std::optional<int64_t>(std::nullopt); };
    DOCTEST_CHECK(overdueBubbleText(unavailable).empty());
    // 桥未注入（空 std::function）→ 同样空串回退，气泡绝不出「你有 条…」残句
    std::function<std::optional<int64_t>()> none;
    DOCTEST_CHECK(overdueBubbleText(none).empty());
    return true;
});

// 内容源①：静态语料逐条装气泡往返；语料退化（空表 / 空串 / 装不进气泡）直接红
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

// 内容源③：代理事件文案——mock ProxyEventGetter 返回各终态码；
// nullopt（无事件 / claude 关 / 过期）或桥未注入 → 空串
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
    PetBubble bubble;
    bubble.show(tOk, 3.0);
    DOCTEST_CHECK(bubble.visible);
    DOCTEST_CHECK_EQ(bubble.text, tOk);
    bubble.tick(3.5);
    DOCTEST_CHECK(!bubble.visible);
    // 3xx 同样归转发完成，不出「出错」
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

// 内容源回退序 ③>②>①：petDataBubbleText 两者皆空时返回空串，由调用方出①静态语料
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
    const auto both = petDataBubbleText(proxyOk, overdue3);
    DOCTEST_CHECK_MESSAGE(both.first.find("代理") != std::string::npos, both.first);
    DOCTEST_CHECK_MESSAGE(both.first.find("逾期") == std::string::npos, both.first);
    DOCTEST_CHECK(both.second == 3.0);
    const auto onlyOverdue = petDataBubbleText(proxyNone, overdue3);
    DOCTEST_CHECK_MESSAGE(onlyOverdue.first.find("逾期") != std::string::npos,
                          onlyOverdue.first);
    DOCTEST_CHECK(onlyOverdue.second == 4.0);
    std::function<std::optional<int>()> proxyErr = [] {
        return std::optional<int>(500);
    };
    const auto proxyErrWins = petDataBubbleText(proxyErr, overdue3);
    DOCTEST_CHECK_MESSAGE(proxyErrWins.first.find("500") != std::string::npos,
                          proxyErrWins.first);
    const auto empty = petDataBubbleText(proxyNone, overdueNone);
    DOCTEST_CHECK(empty.first.empty());
    std::function<std::optional<int>()> noProxy;
    std::function<std::optional<int64_t>()> noOverdue;
    DOCTEST_CHECK(petDataBubbleText(noProxy, noOverdue).first.empty());
    return true;
});

// 气泡调度纯函数：固定 dt / 状态 / roll 输入，断言「弹哪一级 + 停多久」与各边界
REGISTER_TEST(pet_bubble_schedule_pure,
              "pet: 气泡调度纯函数（来源/停留/门控/边界）", {
    using namespace module::pet;
    // dt = 1s 时的两个阈值：数据槽位 1/150，共享槽位上界 1/60
    const double tData = 1.0 / kPetDataSlotSec;
    const double tAny = 1.0 / kPetAnyBubbleSlotSec;
    // 数据槽位停留 = kPetBubbleHoldSec
    const PetBubblePlan data = petBubbleSchedule(1.0, false, true, 0.0);
    DOCTEST_CHECK(data.source == PetBubbleSource::Data);
    DOCTEST_CHECK_EQ(data.holdSec, 3.0);
    // 上界为严格小于：恰好压在上界 → 落①静态语料
    DOCTEST_CHECK(petBubbleSchedule(1.0, false, true, tData - 1e-9).source ==
                  PetBubbleSource::Data);
    DOCTEST_CHECK(petBubbleSchedule(1.0, false, true, tData).source ==
                  PetBubbleSource::Static);
    // 两桥皆无 → 空串，调用方走①兜底；兜底停留取计划 holdSec（0 会让气泡立消）
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

// 节拍推导：dt = 1s 上按整数网格统计命中占比（全固定算术、无随机源）。
//   数据槽位 = 1/150 → 1000/150000 点；静态 = 1/60 − 1/150 = 1/100 → 1500 点。
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
    DOCTEST_CHECK_EQ(staticHits, 1500);  // 1/100（~100s）
    // 按计划停留时长装气泡，到点消失
    PetBubble bubble;
    bubble.show("按调度弹一句", kPetBubbleHoldSec);
    DOCTEST_CHECK(bubble.visible);
    bubble.tick(kPetBubbleHoldSec + 0.01);
    DOCTEST_CHECK(!bubble.visible);
    return true;
});

REGISTER_TEST(pet_skin_parse_m2_states, "pet: pet.json sleep/typing 帧解析", {
    using namespace module::pet;
    auto r = parsePetSkinJson(kM2SkinJson);
    DOCTEST_CHECK(r.isOk());
    const PetSkin& skin = r.value();
    DOCTEST_CHECK_EQ(skin.sleepFrames.size(), static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(skin.typingFrames.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(skin.sleepFrames[2], "s2.png");
    DOCTEST_CHECK_EQ(skin.typingFrames[0], "t0.png");
    // 状态帧查表：四态各自命中；缺帧状态回落 idle
    DOCTEST_CHECK_EQ(&stateFrames(skin, PetAnimState::Sleep), &skin.sleepFrames);
    DOCTEST_CHECK_EQ(&stateFrames(skin, PetAnimState::Typing),
                     &skin.typingFrames);
    DOCTEST_CHECK_EQ(&stateFrames(skin, PetAnimState::Idle), &skin.idleFrames);
    const PetSkin plainSkin = parsePetSkinJson(kValidSkinJson).value();
    DOCTEST_CHECK_EQ(&stateFrames(plainSkin, PetAnimState::Sleep),
                     &plainSkin.idleFrames);  // 无 sleep 帧 → idle
    return true;
});

REGISTER_TEST(pet_default_skin_m2_frames, "pet: 随包 robot 四态帧齐备可加载", {
    using namespace module::pet;
    const PetsDirEnv pets("defaultskin_m2");
    const std::filesystem::path& root = pets.root;
    test_support::writeShippedSkinFixture(root, "robot", kRobotFixtureJson);
    const auto r = ensureDefaultSkin();
    DOCTEST_CHECK_MESSAGE(r.isOk(), r.isFail() ? r.errorMessage() : "");
    const PetSkin& skin = r.value();
    DOCTEST_CHECK_EQ(skin.sleepFrames.size(), static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(skin.typingFrames.size(), static_cast<size_t>(2));
    DOCTEST_CHECK(!skin.phrases.empty());
    for (size_t i = 0; i < skin.sleepFrames.size(); ++i) {
        DOCTEST_CHECK(pngLooksValid(skin.dir / skin.sleepFrames[i], 128, 128));
    }
    for (size_t i = 0; i < skin.typingFrames.size(); ++i) {
        DOCTEST_CHECK(pngLooksValid(skin.dir / skin.typingFrames[i], 128, 128));
    }
    // 用户替换皮肤（source 非随包标记）→ 候选优先命中 petsDir，原样加载不动；帧数不足也如实返回
    {
        std::ofstream out(root / "robot" / "pet.json", std::ios::trunc);
        out << "{\"name\":\"robot\",\"source\":\"artist\","
               "\"states\":{\"idle\":[\"a0.png\"]}}";
    }
    const auto kept = ensureDefaultSkin();
    DOCTEST_CHECK(kept.isOk());
    DOCTEST_CHECK(kept.value().sleepFrames.empty());
    DOCTEST_CHECK_EQ(kept.value().idleFrames.size(), static_cast<size_t>(1));
    return true;
});

REGISTER_TEST(pet_builtin_skins, "pet: 随包 robot/ghost 两款皮肤（四态帧/发现/短语表/替换保留）", {
    using namespace module::pet;
    const PetsDirEnv pets("builtin_skins");
    const std::filesystem::path& root = pets.root;
    test_support::writeShippedSkinFixture(root, "robot", kRobotFixtureJson);
    test_support::writeShippedSkinFixture(root, "ghost", kGhostFixtureJson);
    // discoverSkins 发现全部 2 款
    const std::vector<std::string> found = discoverSkins();
    DOCTEST_CHECK_EQ(found.size(), static_cast<size_t>(2));
    for (const char* name : {"robot", "ghost"}) {
        DOCTEST_CHECK(std::find(found.begin(), found.end(), name) != found.end());
    }
    // 两款皮肤均可载入
    std::vector<PetSkin> skins;
    for (const char* name : {"robot", "ghost"}) {
        auto r = loadSkin(root / name);
        DOCTEST_CHECK_MESSAGE(r.isOk(), r.isFail() ? r.errorMessage() : name);
        if (r.isOk()) {
            skins.push_back(std::move(r.value()));
        }
    }
    DOCTEST_CHECK_EQ(skins.size(), static_cast<size_t>(2));
    // 每款：四态帧齐全 + 128×128 + 全帧 PNG 头合法 + 短语表非空
    for (const PetSkin& skin : skins) {
        DOCTEST_CHECK_EQ(skin.idleFrames.size(), static_cast<size_t>(6));
        DOCTEST_CHECK_EQ(skin.pokedFrames.size(), static_cast<size_t>(2));
        DOCTEST_CHECK_EQ(skin.sleepFrames.size(), static_cast<size_t>(3));
        DOCTEST_CHECK_EQ(skin.typingFrames.size(), static_cast<size_t>(2));
        DOCTEST_CHECK_EQ(skin.frameWidth, 128);
        DOCTEST_CHECK_EQ(skin.frameHeight, 128);
        DOCTEST_CHECK(!skin.phrases.empty());
        for (const std::vector<std::string>* frames :
             {&skin.idleFrames, &skin.pokedFrames, &skin.sleepFrames, &skin.typingFrames}) {
            for (const std::string& frame : *frames) {
                DOCTEST_CHECK(pngLooksValid(skin.dir / frame, 128, 128));
            }
        }
    }
    // 两款短语表互异
    DOCTEST_CHECK(skins[0].phrases != skins[1].phrases);
    // robot 换成美术包（source 非随包标记）→ 候选优先命中 petsDir
    {
        std::ofstream out(root / "robot" / "pet.json", std::ios::trunc);
        out << "{\"name\":\"robot\",\"source\":\"artist\","
               "\"states\":{\"idle\":[\"a0.png\"]}}";
    }
    const auto replaced = ensureDefaultSkin();
    DOCTEST_CHECK(replaced.isOk());
    DOCTEST_CHECK_EQ(replaced.value().idleFrames.size(), static_cast<size_t>(1));
    return true;
});


REGISTER_TEST(pet_discover_skins, "pet: 皮肤发现（下拉/换肤菜单的数据源）", {
    using namespace module::pet;
    const PetsDirEnv pets("discover");
    const std::filesystem::path& root = pets.root;
    // 空目录 → 无皮肤
    std::filesystem::create_directories(root);
    DOCTEST_CHECK(discoverSkins().empty());
    // 手工放第二个皮肤包 → 一并被发现
    test_support::writeShippedSkinFixture(root, "robot", kRobotFixtureJson);
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
    // 无 pet.json 与非法名目录不进清单
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

    // 候选里是否含「安装目录」那条（exe 旁 assets/pets）。exe 目录解析失败时本就没有
    // 该候选，此时不评（返回 true，与 skinRootCandidates 的跳过语义一致）
static bool rootsContainPackaged(const std::vector<std::filesystem::path>& roots) {
    const std::filesystem::path exeDir = core::paths::executableDir();
    if (exeDir.empty()) return true;
    const std::filesystem::path packaged = exeDir / "assets" / "pets";
    for (const std::filesystem::path& root : roots) {
        if (root == packaged) return true;
    }
    return false;
}

// 契约：候选纯派生（数据根 + 安装目录），DEVDESK_PETS_DIR 只搬 petsDir() 的位置。
// 设了 env 也必须保留安装目录候选，不得提前 return。
REGISTER_TEST(pet_skin_roots_env_independent,
              "pet: 皮肤根候选不随 DEVDESK_PETS_DIR 变化（CP-04）", {
    using namespace module::pet;
    const PetsDirEnv pets("roots_env");
    const std::vector<std::filesystem::path> withEnv = skinRootCandidates();
    DOCTEST_REQUIRE(!withEnv.empty());
    // 设着 env 时 petsDir 仍是第一条候选，也是换肤写盘处
    DOCTEST_CHECK_EQ(withEnv[0], pets.root);

    // 清掉 env（空串 = 删除变量）→ 候选形态不变
    test_support::setEnvVar("DEVDESK_PETS_DIR", "");
    const std::vector<std::filesystem::path> withoutEnv = skinRootCandidates();
    DOCTEST_CHECK_EQ(withEnv.size(), withoutEnv.size());
    // 设了 env 也不能丢掉安装目录候选
    DOCTEST_CHECK(rootsContainPackaged(withEnv));
    DOCTEST_CHECK(rootsContainPackaged(withoutEnv));
    return true;
})

REGISTER_TEST(pet_apply_skin_and_reset, "pet: applyPetSkin 校验/持久化 + 位置重置", {
    using namespace module::pet;
    const PetsDirEnv pets("applyskin");
    const std::filesystem::path& root = pets.root;
    test_support::writeShippedSkinFixture(root, "robot", kRobotFixtureJson);
    std::filesystem::create_directories(root / "pixel-dog");
    {
        std::ofstream out(root / "pixel-dog" / "pet.json", std::ios::trunc);
        out << "{\"name\":\"pixel-dog\",\"states\":{\"idle\":[\"d0.png\"]}}";
    }
    // 预置配置：已知位置 + 默认皮肤（窗口未开 → 只落库）
    {
        PetConfigStore store(::core::appDatabase());
        PetConfig cfg;
        cfg.skinName = "robot";
        cfg.x = 500;
        cfg.y = 600;
        DOCTEST_CHECK(store.save(cfg).isOk());
    }
    // 未发现的皮肤名被拒绝，配置不动
    applyPetSkin("no-such-skin");
    {
        const PetConfig cfg = PetConfigStore(::core::appDatabase()).load();
        DOCTEST_CHECK_EQ(cfg.skinName, "robot");
    }
    // 合法皮肤名 → 持久化生效，位置不动（窗口未开）
    applyPetSkin("pixel-dog");
    {
        const PetConfig cfg = PetConfigStore(::core::appDatabase()).load();
        DOCTEST_CHECK_EQ(cfg.skinName, "pixel-dog");
        DOCTEST_CHECK_EQ(cfg.x, 500);
        DOCTEST_CHECK_EQ(cfg.y, 600);
    }
    // 位置重置 → 坐标清回 -1（未设置哨兵），皮肤不被动
    resetPetPosition();
    {
        const PetConfig cfg = PetConfigStore(::core::appDatabase()).load();
        DOCTEST_CHECK_EQ(cfg.x, -1);
        DOCTEST_CHECK_EQ(cfg.y, -1);
        DOCTEST_CHECK_EQ(cfg.skinName, "pixel-dog");
    }
    return true;
});

// GLFW 上报的光标位置是窗口相对的：窗口被拖拽回调移动后，同样的屏幕移动产生反向
// 偏差的读数。锚点方案——首个拖拽事件捕获锚点（窗口原地起步），此后逐事件 1:1 跟
// 光标屏幕位移。改回 delta 累加会在此坐标模型下振荡。

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
    // 光标轨迹步进：单调右移 + 下移
    const int stepX[5] = {8, 5, 5, 7, 6};
    const int stepY[5] = {0, 3, 4, 2, 5};
    for (int i = 0; i < 5; ++i) {
        screenX += stepX[i];
        screenY += stepY[i];
        // GLFW 上报的窗口内光标位（窗口移动后参考系已变）
        const double relX = screenX - static_cast<double>(winX);
        const double relY = screenY - static_cast<double>(winY);
        const auto target =
            module::pet::petDragMove(anchor, winX, winY, relX, relY);
        if (i == 0) {
            // 首个拖拽事件：锚点捕获，窗口原地起步（不跳变）
            DOCTEST_CHECK_EQ(target.first, 500);
            DOCTEST_CHECK_EQ(target.second, 400);
        } else {
            // 1:1 跟随：窗口位移 == 光标屏幕位移（delta 累加在此轨迹上会振荡）
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


REGISTER_TEST(pet_pos_clamp_work_area, "pet: 已知坐标钳进工作区", {
    // 常规位置不动
    const auto normal =
        module::pet::clampPetPosToWorkArea(100, 200, 0, 0, 1920, 1080, 128, 128);
    DOCTEST_CHECK_EQ(normal.first, 100);
    DOCTEST_CHECK_EQ(normal.second, 200);
    // 负坐标 → 钳到工作区原点；落盘须能区分真实负位与 -1「未设置」哨兵
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

    // 帧名穿越拒绝判定；辅助函数避免多处断言重复拼 JSON
static bool frameNameRejected(const char* frameName) {
    const std::string json =
        std::string("{\"states\":{\"idle\":[\"") + frameName + "\"]}}";
    return module::pet::parsePetSkinJson(json).isFail();
}

REGISTER_TEST(pet_skin_frame_name_traversal, "pet: 帧名路径穿越拒绝", {
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
    DOCTEST_CHECK(pokedResult.value().pokedFrames.empty());
    DOCTEST_CHECK_EQ(pokedResult.value().frames(true).size(),
                     static_cast<size_t>(1));
    // phrases 是展示文本不是路径：含路径字符不拖垮解析
    const std::string phraseJson =
        "{\"states\":{\"idle\":[\"f.png\"]},"
        "\"phrases\":[\"走之前去C:/Downloads拿东西\",\"嗯……/../没什么\"]}";
    const auto phraseResult = module::pet::parsePetSkinJson(phraseJson);
    DOCTEST_CHECK(phraseResult.isOk());
    DOCTEST_CHECK_EQ(phraseResult.value().phrases.size(),
                     static_cast<size_t>(2));
    return true;
});


REGISTER_TEST(pet_exit_confirm_menu, "pet: 退出确认菜单文本结构", {
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

REGISTER_TEST(pet_exit_confirm_decision, "pet: 只有「确认退出」路径触发退出", {
    using module::pet::petExitConfirmChosen;
    DOCTEST_CHECK(petExitConfirmChosen(std::vector<int>{2}));
    DOCTEST_CHECK(!petExitConfirmChosen(std::vector<int>{3}));
    DOCTEST_CHECK(!petExitConfirmChosen(std::vector<int>{0}));
    DOCTEST_CHECK(!petExitConfirmChosen(std::vector<int>{1}));
    DOCTEST_CHECK(!petExitConfirmChosen(std::vector<int>{2, 1}));
    DOCTEST_CHECK(!petExitConfirmChosen(std::vector<int>{}));
    return true;
})


namespace {

// 主屏 + 左侧负坐标区副屏
std::vector<module::pet::PetWorkArea> dualMonitorLeft() {
    return {{0, 0, 1920, 1080}, {-1920, 0, 1920, 1080}};
}

// 主屏 + 右侧正坐标副屏
std::vector<module::pet::PetWorkArea> dualMonitorRight() {
    return {{0, 0, 1920, 1080}, {1920, 0, 1920, 1080}};
}

// 仅主屏（无副屏基准）
std::vector<module::pet::PetWorkArea> primaryOnly() {
    return {{0, 0, 1920, 1080}};
}

} // namespace

REGISTER_TEST(pet_multi_monitor_clamp, "pet: 多显示器按位置选屏钳制", {
    using module::pet::clampPetPosToMonitors;
    // 副屏内合法负坐标不动（只按主屏钳会跳回主屏）
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
    // 两屏都不含的远处 → 取最近屏钳制（MONITOR_DEFAULTTONEAREST 语义）
    const auto farRight = clampPetPosToMonitors(99999, 300, dualMonitorRight(), 128, 128);
    DOCTEST_CHECK_EQ(farRight.first, 3712);
    DOCTEST_CHECK_EQ(farRight.second, 300);
    // 仅主屏枚举时负坐标钳回主屏原点（无副屏用户行为不变）
    const auto legacy = clampPetPosToMonitors(-50, -80, primaryOnly(), 128, 128);
    DOCTEST_CHECK_EQ(legacy.first, 0);
    DOCTEST_CHECK_EQ(legacy.second, 0);
    // 空枚举（无头兜底）→ 原样
    const auto headless = clampPetPosToMonitors(-50, -60, std::vector<module::pet::PetWorkArea>{}, 128, 128);
    DOCTEST_CHECK_EQ(headless.first, -50);
    DOCTEST_CHECK_EQ(headless.second, -60);
    // 无效块（宽高 0）不参与选屏：[无效, 主屏] 布局下仍按主屏钳
    std::vector<module::pet::PetWorkArea> withInvalid = {{5000, 5000, 0, 0},
                                                         {0, 0, 1920, 1080}};
    const auto skipInvalid = clampPetPosToMonitors(99999, 300, withInvalid, 128, 128);
    DOCTEST_CHECK_EQ(skipInvalid.first, 1792);
    return true;
});

REGISTER_TEST(pet_negative_coord_persist, "pet: 副屏负坐标落盘往返", {
    // pet 配置存 app_config 表，经 core::appDatabase() 取连接；未注册则 save() 如实失败。
    test_support::TestEnv cfgDbEnv("devdesk_test_pet_", "petcfg2", /*registerAppDb=*/true);
    using namespace module::pet;
    const std::filesystem::path file =
        test_temp::tempPath("devdesk_test_pet_config_neg.json");
    std::error_code ec;
    std::filesystem::remove(file, ec);
    PetConfigStore store(::core::appDatabase());
    // 左副屏坐标落盘 → 读回原值（负坐标不得被吞成「未设置」）
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

// compose 入口唯一的读值是 isPetRenderPaused()——true 即整帧 return，不重建成 /
// 不推动画 / 不跑计时器；电源回调写入方与该读取方共用同一标志。
// WM_POWERBROADCAST → 回调 → 标志的真实链路属真机点验（EUI-NEO power_events 只在
// Windows 消息泵触发）。
REGISTER_TEST(pet_sleep_render_pause, "pet: Sleep 零渲染暂停门", {
    using module::pet::isPetRenderPaused;
    using module::pet::setPetRenderPaused;
    DOCTEST_CHECK(!isPetRenderPaused());
    setPetRenderPaused(true);
    DOCTEST_CHECK(isPetRenderPaused());
    setPetRenderPaused(false);
    DOCTEST_CHECK(!isPetRenderPaused());
    // 重复挂起 / 唤醒幂等：两种电源通知先后到也只收敛到 false
    setPetRenderPaused(true);
    setPetRenderPaused(true);
    setPetRenderPaused(false);
    setPetRenderPaused(false);
    DOCTEST_CHECK(!isPetRenderPaused());
    return true;
});

// 跨模块不变量：settings 不 include pet 头也不 link pet，桌宠卡初值走
// currentSkinName 只读函数（盘即真值——apply 路径双写运行时 + 配置表）。
// 不要为了取皮肤名把 pet 依赖引进 settings。
REGISTER_TEST(pet_bridge_readonly_accessors,
              "pet: currentSkinName 只读桥（设置页解耦）", {
    PetsDirEnv env("bridge");
    using namespace module::pet;

    // 无配置 → PetConfig 默认值
    DOCTEST_CHECK_EQ(currentSkinName(), std::string(kDefaultSkinName));

    // 换皮肤名走 store（applyPetSkin 需 discoverSkins 命中）
    PetConfig cfg;
    cfg.skinName = "bridge-skin";
    DOCTEST_CHECK(!PetConfigStore(::core::appDatabase()).save(cfg).isFail());
    DOCTEST_CHECK_EQ(currentSkinName(), std::string("bridge-skin"));
    return true;
});

// 失败注入约定：把临时文件路径 <目标>.devdesk-tmp 用目录占住（打开必失败），
// 不 sleep、不杀进程，可顺带断言「失败时旧内容一字未变」。

namespace {


std::filesystem::path tmpPathOf(const std::filesystem::path& p) {
    return std::filesystem::path(p.string() + ".devdesk-tmp");
}

} // namespace

REGISTER_TEST(pet_config_save_failure_keeps_old, "pet: 配置原子写失败不破坏旧文件（THS-03）", {
    // pet 配置存 app_config 表，经 core::appDatabase() 取连接；未注册则 save() 如实失败。
    test_support::TestEnv cfgDbEnv("devdesk_test_pet_", "petcfg3", /*registerAppDb=*/true);
    using namespace module::pet;
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "devdesk_test_pet_config_atomic.json";
    const std::filesystem::path tmp = tmpPathOf(file);
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::remove(file, ec);

    PetConfigStore store(::core::appDatabase());
    PetConfig cfg;
    cfg.skinName = "my-cat";
    cfg.x = 1600;
    cfg.y = 800;
    DOCTEST_REQUIRE(store.save(cfg).isOk());
    // 一次成功落库的证据是该行原文，且须是完整配置（skinName/x/y/scale 四键齐全）
    const std::string row = test_support::readRawConfigRow(
        ::core::appDatabase(), module::pet::kPetConfigKey);
    DOCTEST_CHECK(!row.empty());
    {
        auto parsed = platform::json::parse(row);
        DOCTEST_REQUIRE(parsed.isOk());
        DOCTEST_REQUIRE(parsed.value().isObject());
        auto o = parsed.value().asObject();
        DOCTEST_CHECK(o.has("skinName"));
        DOCTEST_CHECK(o.has("x"));
        DOCTEST_CHECK(o.has("y"));
        DOCTEST_CHECK(o.has("scale"));
    }

    // save 失败（库未就绪）时，上一次成功落库的配置必须原样可读。整份存一行 =
    // 单条 UPSERT，本身即原子。
    PetConfigStore noDb(nullptr);
    PetConfig changed;
    changed.skinName = "robot";
    changed.x = 0;
    changed.y = 0;
    DOCTEST_CHECK(noDb.save(changed).isFail());

    PetConfigStore reopened(::core::appDatabase());
    const PetConfig loaded = reopened.load();
    DOCTEST_CHECK_EQ(loaded.skinName, std::string("my-cat"));
    DOCTEST_CHECK_EQ(loaded.x, 1600);
    DOCTEST_CHECK_EQ(loaded.y, 800);

    std::filesystem::remove(tmp, ec);
    std::filesystem::remove(file, ec);
    return true;
});

// 随机源只能锁「活着」：64 次抽样须出 ≥2 种短语，且每个结果都落在短语表内
//（不越界、不空串）。跨进程不同序列在本用例断言不到。
REGISTER_TEST(pet_phrase_randomized, "pet: 气泡短语随机化（64 次抽样出多种）", {
    using namespace module::pet;
    auto parsed = parsePetSkinJson(kM2SkinJson);
    DOCTEST_REQUIRE(parsed.isOk());
    const PetSkin& skin = parsed.value();
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

// 无头：handle 恒为 nullptr（onWindowCreated 只在主循环里回填），故锁的是
// 「已装配过的会话 → showPetWindow 重新入队一条开窗请求，几何沿用 rt.config」
REGISTER_TEST(pet_show_window_reenqueues,
              "pet: showPetWindow 重新入队开窗请求（PAB-13）", {
    const PetsDirEnv pets("showwin");
    (void)app::consumeWindowRequests();  // 清残留
    test_support::writeShippedSkinFixture(pets.root, "robot", kRobotFixtureJson);  // 默认皮肤 = 随包 robot

    // 先装配一次（= main 模块门控通过；同时把 windowRequested 置位）
    module::pet::startPetWindow(module::pet::PetWindowConfig{"", 120, 130});
    const std::vector<app::DslWindowRequest> first = app::consumeWindowRequests();
    DOCTEST_REQUIRE_EQ(first.size(), static_cast<size_t>(1));

    // 重新显示：应再入队一条（皮肤/位置沿用 rt.config）
    DOCTEST_CHECK(module::pet::showPetWindow() ==
                  module::pet::PetShowOutcome::Shown);
    const std::vector<app::DslWindowRequest> second = app::consumeWindowRequests();
    DOCTEST_REQUIRE_EQ(second.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(second[0].pageId, "pet");
    DOCTEST_CHECK_EQ(second[0].width, 128);
    // 窗口高 = 帧高 + 气泡让位条（pet_bubble.h）
    DOCTEST_CHECK_EQ(second[0].height,
                     128 + static_cast<int>(module::pet::kBubbleStripH));
    DOCTEST_CHECK(second[0].positionSet);
    DOCTEST_CHECK_EQ(second[0].x, 120);
    DOCTEST_CHECK_EQ(second[0].y, 130);
    return true;
});

// showPetWindow 四态互斥：settings 侧不得把 AlreadyOpen 误报成「未启用，重启后生效」
REGISTER_TEST(pet_show_window_outcomes,
              "pet: showPetWindow 四态各归各位（Bug B）", {
    using Outcome = module::pet::PetShowOutcome;
    (void)app::consumeWindowRequests();  // 清残留

    // ① NotAssembled：装配标志只置不清，进程级单例回厂只能经测试缝——即「启动时
    //    开关就是关的」会话的真实形态。拒绝凭空开窗，零请求入队
    module::pet::resetPetRuntimeForTests();
    DOCTEST_CHECK(module::pet::showPetWindow() == Outcome::NotAssembled);
    DOCTEST_CHECK(app::consumeWindowRequests().empty());

    // ② StartFailed：已装配但皮肤不可用（空皮肤根 + 测试环境无随包资产 → 回退默认
    //    皮肤也失败），同样零请求入队
    module::pet::resetPetRuntimeForTests();
    {
        const PetsDirEnv pets("showwin_empty");
        module::pet::startPetWindow(module::pet::PetWindowConfig{});
        DOCTEST_CHECK(app::consumeWindowRequests().empty());  // 装配就没开成窗
        DOCTEST_CHECK(module::pet::showPetWindow() == Outcome::StartFailed);
        DOCTEST_CHECK(app::consumeWindowRequests().empty());
    }

    // ③ 隐藏后重开 = Shown：夹具皮肤装配 → 消费首条开窗请求 → 再 show 再入队
    module::pet::resetPetRuntimeForTests();
    {
        const PetsDirEnv pets("showwin_shown");
        test_support::writeShippedSkinFixture(pets.root, "robot",
                                              kRobotFixtureJson);
        DOCTEST_REQUIRE(module::pet::startPetWindow(
            module::pet::PetWindowConfig{"", 120, 130}));
        (void)app::consumeWindowRequests();
        DOCTEST_CHECK(module::pet::showPetWindow() == Outcome::Shown);
        DOCTEST_CHECK_EQ(app::consumeWindowRequests().size(),
                         static_cast<size_t>(1));
    }

    // ④ AlreadyOpen：无头环境 handle 只能经请求自带的 onWindowCreated 回填（生产里
    // 这是主循环职责）——不得再落进「未启用，重启生效」
    module::pet::resetPetRuntimeForTests();
    {
        const PetsDirEnv pets("showwin_open");
        test_support::writeShippedSkinFixture(pets.root, "robot",
                                              kRobotFixtureJson);
        module::pet::startPetWindow(module::pet::PetWindowConfig{});
        const std::vector<app::DslWindowRequest> reqs =
            app::consumeWindowRequests();
        DOCTEST_REQUIRE_EQ(reqs.size(), static_cast<size_t>(1));
        DOCTEST_REQUIRE(reqs[0].onWindowCreated);
        reqs[0].onWindowCreated(
            reinterpret_cast<core::window::Handle>(0x1));  // 假句柄即可判空
        DOCTEST_CHECK(module::pet::showPetWindow() == Outcome::AlreadyOpen);
        DOCTEST_CHECK(app::consumeWindowRequests().empty());  // 已开=不再入队
    }

    // 收尾回厂：不留装配态污染后续用例（进程级 pet 运行时是共享状态）
    module::pet::resetPetRuntimeForTests();
    (void)app::consumeWindowRequests();
    return true;
});

// 几何断言：气泡矩形与精灵帧矩形**不相交**。petBubbleRect / petSpriteRect 是
// compose 侧同一数据源（pet_window.cpp 直接消费这两个纯函数），纯函数层断言成立
// = 渲染几何成立。
REGISTER_TEST(pet_bubble_layout_disjoint,
              "pet: 气泡矩形与精灵帧矩形不相交（上移不遮挡）", {
    using namespace module::pet;
    // 常量口径：间距落在 4-8px 需求带，条高 = 气泡高 + 间距
    DOCTEST_CHECK_EQ(kBubbleHeight, 28.0f);
    DOCTEST_CHECK(kBubbleGap >= 4.0f && kBubbleGap <= 8.0f);
    DOCTEST_CHECK_EQ(kBubbleStripH, kBubbleHeight + kBubbleGap);
    DOCTEST_CHECK_EQ(petWindowHeight(128), 162);
    DOCTEST_CHECK_EQ(petWindowHeight(512), 546);

    // 覆盖典型窗口宽 × 气泡宽（含 80 下限、280 上限、超窗宽 600——负 x 被窗口裁剪）
    // 全组合：垂直分离（气泡底 + 间距 = 帧顶）⇒ 两矩形恒不相交，与水平位置无关
    const float windowWidths[] = {16.0f, 128.0f, 136.0f, 360.0f, 512.0f};
    const float bubbleWidths[] = {80.0f, 136.0f, 280.0f, 600.0f};
    const float windowHeights[] = {static_cast<float>(petWindowHeight(128)),
                                   static_cast<float>(petWindowHeight(512))};
    for (const float winW : windowWidths) {
        for (const float winH : windowHeights) {
            const PetRect sprite = petSpriteRect(winW, winH);
            // 帧占满让位条以下全部空间（compose 侧同款：帧高 = 窗高 - 条高）
            DOCTEST_CHECK_EQ(sprite.y, kBubbleStripH);
            DOCTEST_CHECK_EQ(sprite.h, winH - kBubbleStripH);
            for (const float bw : bubbleWidths) {
                const PetRect bubble = petBubbleRect(winW, bw);
                DOCTEST_CHECK_EQ(bubble.y, 0.0f);
                DOCTEST_CHECK_EQ(bubble.h, kBubbleHeight);
                // 间距钉死：气泡底沿到帧顶 = kBubbleGap（4-8px 需求带）
                DOCTEST_CHECK_EQ(sprite.y - (bubble.y + bubble.h), kBubbleGap);
                const bool overlap =
                    bubble.x < sprite.x + sprite.w &&
                    bubble.x + bubble.w > sprite.x &&
                    bubble.y < sprite.y + sprite.h &&
                    bubble.y + bubble.h > sprite.y;
                DOCTEST_CHECK(!overlap);
            }
        }
    }

    // 气泡不越过屏幕工作区：整窗高参与钳制（调用方传 petWindowHeight）后，
    // 窗口顶 = 气泡条上沿不会低于工作区上缘——钳到贴上缘时气泡完整可见
    {
        const std::vector<PetWorkArea> areas = {PetWorkArea{0, 0, 1920, 1020}};
        const auto [cx, cy] = clampPetPosToMonitors(
            500, -999, areas, 128, petWindowHeight(128));
        DOCTEST_CHECK_EQ(cy, 0);  // 越界上移被回贴到工作区上缘（气泡不被裁）
    }
    return true;
});


// scaledFrameSize：上界按**等比单因子** min(scale, 512/w, 512/h) 钳制，长宽比恒等帧
// 原始值——360×480 大档得 384×512 零裁切。改成逐维钳会破比例（512×512 被 Cover
// 中心裁掉头脚各 12.5%）。下界不钳：16 是皮肤解析界，16×0.75=12 小窗无害。
REGISTER_TEST(pet_scale_frame_size,
              "pet: 显示大小档位→帧尺寸换算（等比钳上界 512）", {
    using namespace module::pet;
    // 档位表口径：三档、默认中档
    DOCTEST_CHECK_EQ(kPetScalePresetCount, 3);
    DOCTEST_CHECK_EQ(kPetScaleDefaultIndex, 1);
    DOCTEST_CHECK_EQ(kPetScalePresets[0], 0.75);
    DOCTEST_CHECK_EQ(kPetScalePresets[1], 1.0);
    DOCTEST_CHECK_EQ(kPetScalePresets[2], 1.5);
    DOCTEST_CHECK_EQ(kMinFrameSize, 16);
    DOCTEST_CHECK_EQ(kMaxFrameSize, 512);

    // 换算：中档 1:1；大档 ×1.5；小档 ×0.75（128×128 夹具基准）
    const auto mid = scaledFrameSize(128, 128, 1.0);
    DOCTEST_CHECK_EQ(mid.first, 128);
    DOCTEST_CHECK_EQ(mid.second, 128);
    const auto big = scaledFrameSize(128, 128, 1.5);
    DOCTEST_CHECK_EQ(big.first, 192);
    DOCTEST_CHECK_EQ(big.second, 192);
    const auto small = scaledFrameSize(128, 128, 0.75);
    DOCTEST_CHECK_EQ(small.first, 96);
    DOCTEST_CHECK_EQ(small.second, 96);
    // 非方形皮肤（whale-girl 328×212）×1.5
    const auto whale = scaledFrameSize(328, 212, 1.5);
    DOCTEST_CHECK_EQ(whale.first, 492);
    DOCTEST_CHECK_EQ(whale.second, 318);
    // 上界等比钳制：512×1.5=768 → f=1.0 → 512×512（方形无失真）；
    // 360×480×1.5 → f=512/480 → 384×512，长宽比 0.75 保持
    const auto atMax = scaledFrameSize(512, 512, 1.5);
    DOCTEST_CHECK_EQ(atMax.first, 512);
    DOCTEST_CHECK_EQ(atMax.second, 512);
    const auto kirari = scaledFrameSize(360, 480, 1.5);
    DOCTEST_CHECK_EQ(kirari.first, 384);
    DOCTEST_CHECK_EQ(kirari.second, 512);
    // 下界不钳（等比优先）：16×0.75=12 原样缩（16 是皮肤解析界非窗口界）
    const auto atMin = scaledFrameSize(16, 16, 0.75);
    DOCTEST_CHECK_EQ(atMin.first, 12);
    DOCTEST_CHECK_EQ(atMin.second, 12);
    // 等比不变式：缩放后宽高比恒等帧原始宽高比（round 误差 ±1px 内，偏差 < 0.02）
    const struct AscCase {
        int w;
        int h;
        double s;
    } ascCases[] = {
        {128, 128, 1.5}, {128, 128, 0.75}, {128, 128, 1.0}, {360, 480, 1.5},
        {512, 512, 1.5}, {16, 16, 0.75},   {328, 212, 1.5}, {192, 208, 0.75},
        {136, 168, 1.0},
    };
    for (const AscCase& c : ascCases) {
        const auto [sw, sh] = scaledFrameSize(c.w, c.h, c.s);
        const double before = static_cast<double>(c.w) / c.h;
        const double after = static_cast<double>(sw) / sh;
        DOCTEST_CHECK(std::abs(after - before) < 0.02);
    }
    // 非法档位值防御：sanitize 白名单外收敛默认中档（防手改 pet_config.json）
    DOCTEST_CHECK_EQ(sanitizePetScale(0.9), 1.0);
    DOCTEST_CHECK_EQ(sanitizePetScale(2.0), 1.0);
    DOCTEST_CHECK_EQ(sanitizePetScale(0.75), 0.75);
    DOCTEST_CHECK_EQ(petScaleIndexOf(1.5), 2);
    DOCTEST_CHECK_EQ(petScaleIndexOf(0.123), kPetScaleDefaultIndex);
    return true;
});

// pet_config.json scale 键读写往返 + 桥只读/切档（窗口未开 = 只持久化路径）
REGISTER_TEST(pet_scale_config_roundtrip,
              "pet: scale 持久化往返 + 档位桥读写", {
    using namespace module::pet;
    const PetsDirEnv pets("scalecfg");
    {
        PetConfigStore store(::core::appDatabase());
        PetConfig cfg;
        cfg.scale = 1.5;
        DOCTEST_CHECK(store.save(cfg).isOk());
        const PetConfig loaded = store.load();
        DOCTEST_CHECK_EQ(loaded.scale, 1.5);
        // 白名单外值经 sanitized 收敛默认中档
        PetConfig bad;
        bad.scale = 0.9;
        DOCTEST_CHECK_EQ(PetConfigStore::sanitized(bad).scale, 1.0);
    }
    // 桥：无窗口路径——切档只写盘，读回一致；越界下标拒绝不动
    DOCTEST_CHECK_EQ(currentPetScaleIndex(), 2);  // 上面存的 1.5
    applyPetScaleIndex(0);                        // 1.5 → 0.75
    DOCTEST_CHECK_EQ(PetConfigStore(::core::appDatabase()).load().scale, 0.75);
    applyPetScaleIndex(7);                        // 越界：配置不动
    DOCTEST_CHECK_EQ(PetConfigStore(::core::appDatabase()).load().scale, 0.75);
    applyPetScaleIndex(1);                        // 回默认中档
    DOCTEST_CHECK_EQ(PetConfigStore(::core::appDatabase()).load().scale, 1.0);
    DOCTEST_CHECK_EQ(currentPetScaleIndex(), 1);
    // 桥文案：三档与下标一一对应（单源 = pet_config.h，设置页只消费）
    const std::vector<std::string> labels = petScalePresetLabels();
    DOCTEST_REQUIRE_EQ(labels.size(), static_cast<size_t>(3));
    DOCTEST_CHECK(labels[0] == kPetScalePresetLabels[0]);
    DOCTEST_CHECK(labels[2] == kPetScalePresetLabels[2]);
    return true;
});

// 开窗请求逐档断言（无头）：窗口尺寸 = scaledFrameSize(帧尺寸, 档位) + 气泡让位条。
REGISTER_TEST(pet_scale_window_request,
              "pet: 显示大小档位→开窗尺寸换算（无头）", {
    const PetsDirEnv pets("scalewin");
    (void)app::consumeWindowRequests();  // 清残留
    test_support::writeShippedSkinFixture(pets.root, "robot", kRobotFixtureJson);

    // 大档 1.5×：128×128 → 192×(192+条高)
    module::pet::startPetWindow(
        module::pet::PetWindowConfig{"", 50, 60, 1.5});
    std::vector<app::DslWindowRequest> big = app::consumeWindowRequests();
    DOCTEST_REQUIRE_EQ(big.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(big[0].width, 192);
    DOCTEST_CHECK_EQ(big[0].height,
                     192 + static_cast<int>(module::pet::kBubbleStripH));

    // 小档 0.75×：→ 96×(96+条高)
    module::pet::startPetWindow(
        module::pet::PetWindowConfig{"", 50, 60, 0.75});
    std::vector<app::DslWindowRequest> small = app::consumeWindowRequests();
    DOCTEST_REQUIRE_EQ(small.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(small[0].width, 96);
    DOCTEST_CHECK_EQ(small[0].height,
                     96 + static_cast<int>(module::pet::kBubbleStripH));

    // 非法档位 0.9 → sanitize 中档 1.0× → 128×(128+条高)
    module::pet::startPetWindow(
        module::pet::PetWindowConfig{"", 50, 60, 0.9});
    std::vector<app::DslWindowRequest> mid = app::consumeWindowRequests();
    DOCTEST_REQUIRE_EQ(mid.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(mid[0].width, 128);
    DOCTEST_CHECK_EQ(mid[0].height,
                     128 + static_cast<int>(module::pet::kBubbleStripH));
    return true;
});
