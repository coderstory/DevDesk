// tests/probes/claude_probes.cpp — DevDesk probes（类 7(b)：共享配置区块不盖列表视口）
// 目标：共享配置区块（粘性脚注）必须整体位于 virtualList 视口 clip 之外——
// virtualList 视口底部 ≤ 共享区块顶部（修复让出 12px 间距）。
// 回归（claude_ui_list.cpp drawProfileListPanel）：修复前视口按 scrollH 全高、
// 区块只压自己的绝对 Y，后建区块叠在带 clip 的视口底带上 → ≥5 profile 时末
// 1-2 张卡被盖、滚到底显示不全、点击误开片段编辑。修复 = 视口高 listH 让出
// 区块高（clamp 保底 96px）。
//
// 断言机制（几何换算同无头测试 test_claude_ui.cpp
// claude_shared_block_outside_viewport）：
//   viewportBottom = claude.profiles.scroll.frame.y + frame.height
//   blockTop       = claude.shared.block.frame.y
//   断言 blockTop - viewportBottom >= 11.5（修复后恒为 12px 间距；回退 listH
//   = scrollH 后区块叠上视口 → 红；spec §3-probe-full 类 7(b) P1）
//   尺寸矩阵 {1200×720 最小, 1200×1000 默认} 两组各跑一遍（--size 参数化）。

#include "probes/probe_harness.h"
#include "test_support.h"
#include "app_registry.h"
#include "core/module_interface.h"

#include "module_claude_proxy/claude_service.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr const char* kDbPrefix = "devdesk_probe_claude_";
// 测试 Keychain service 名（与 test_claude_ui.cpp 隔离口径一致：不碰真实
// "DevDesk" 密钥，退出前逐个 cleanupProfileKey 清除）
constexpr const char* kKeychainService = "DevDeskTestClaudeProbe";

// RAII：临时库（test_support::TestDb）+ 注册全局 ClaudeService（claude 模块
// onStart / compose 经 ClaudeService::global() 取服务；析构时一定解除注册，
// 防悬垂指向析构后的 service —— test_claude_ui.cpp TestEnv 同款）
struct ProbeEnv : test_support::TestDb {
    std::unique_ptr<module::claude::ClaudeService> svc;

    ProbeEnv() : test_support::TestDb(kDbPrefix, "") {
        svc = std::make_unique<module::claude::ClaudeService>(
            handle(), kKeychainService);
        module::claude::ClaudeService::setGlobal(svc.get());
    }

    ~ProbeEnv() { module::claude::ClaudeService::setGlobal(nullptr); }
};

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("claude_probe", args.windowW,
                                    args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    // 配置隔离（probe 与机器本地 config.json 无关）
    auto cfg = test_support::tempConfigFile("pfx_", "cfg");
    test_support::ScopedConfigPath scopedCfg(cfg);

    // 造数：5 profile + ≥2 共享片段（spec §3-probe-full 类 7(b) 搭法；
    // 片段多 → 区块高 sharedH > 12 才有「区块盖视口」判别力）
    ProbeEnv env;
    std::vector<int64_t> ids;
    bool seedOk = true;
    auto f1 = env.svc->createSharedConfig("片段A", R"({"MY_A":"1"})");
    auto f2 = env.svc->createSharedConfig("片段B", R"({"MY_B":"2"})");
    if (f1.isFail() || f2.isFail()) seedOk = false;
    for (int i = 0; i < 5 && seedOk; ++i) {
        auto p = env.svc->createProfile(
            "档" + std::to_string(i),
            "https://probe" + std::to_string(i) + ".example.com",
            "sk-ant-probe-0" + std::to_string(i), "m");
        if (p.isFail()) {
            seedOk = false;
        } else {
            ids.push_back(p.value().id);
        }
    }
    if (!seedOk) {
        std::cerr << "claude_probes: seed data creation failed\n";
        for (int64_t id : ids) {
            test_support::cleanupProfileKey(kKeychainService, id);
        }
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        probe::terminateWindowBackend();
        return 1;
    }

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    const std::string pageId = "claude-proxy";
    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    bool passed = true;

    core::dsl::Ui* ui = nullptr;
    // 单帧标准单元：pollOnce（输入派发 + 交互）→ compose（recompose + layout，
    // 帧即有效）
    auto composeFrame = [&]() {
        probe::composeModuleFrame(runtime, window, pageId, w, h,
                                  [&](eui::Ui& u, const eui::Screen& screen) {
                                      ui = &u;
                                      mod->compose(u, screen);
                                  });
    };

    composeFrame();
    const core::dsl::Element* scroll =
        ui != nullptr ? ui->find("claude.profiles.scroll") : nullptr;
    const core::dsl::Element* block =
        ui != nullptr ? ui->find("claude.shared.block") : nullptr;
    if (scroll == nullptr || block == nullptr) {
        std::cerr << "claude_probes: 'claude.profiles.scroll' / "
                     "'claude.shared.block' not found (scroll="
                  << (scroll != nullptr ? "x" : "-") << ", block="
                  << (block != nullptr ? "x" : "-") << ")\n";
        passed = false;
    } else {
        // virtualList 视口底 ≤ 共享区块顶（修复后间距恒 12px；回退 scrollH
        // 全高后区块叠在视口底带上 → viewportBottom > blockTop → 红）
        const float viewportBottom = scroll->frame.y + scroll->frame.height;
        const float blockTop = block->frame.y;
        if (blockTop - viewportBottom < 11.5f) {
            std::cerr << "claude_probes: shared block top " << blockTop
                      << " overlaps virtualList viewport bottom "
                      << viewportBottom << " (gap " << (blockTop - viewportBottom)
                      << " < 11.5; bug shape: listH = scrollH full height)\n";
            passed = false;
        }

        // 区块整体在视口 clip 之外还不够：最小窗 720 下区块底不得叠到底部
        // 按钮行（新建/导入，bodyH-64）—— 同无头测试的口径
        const core::dsl::Element* btnNew =
            ui != nullptr ? ui->find("claude.profiles.new") : nullptr;
        if (btnNew == nullptr) {
            std::cerr << "claude_probes: 'claude.profiles.new' not found\n";
            passed = false;
        } else {
            const float blockBottom = block->frame.y + block->frame.height;
            if (blockBottom > btnNew->frame.y + 0.5f) {
                std::cerr << "claude_probes: shared block bottom "
                          << blockBottom << " overlaps bottom buttons row y="
                          << btnNew->frame.y << '\n';
                passed = false;
            }
        }
    }

    mod->onStop();
    for (int64_t id : ids) {
        test_support::cleanupProfileKey(kKeychainService, id);
    }
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!passed) std::cerr << "FAIL\n";
    return passed ? 0 : 1;
}