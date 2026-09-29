// tests/probes/claude_probes.cpp — DevDesk probes（类 7(b)：共享配置区块不盖列表
// 视口 + 2026-09-28 组 B 锚定不变量）
// 目标：
//   ① 共享配置区块（钉面板底的脚注）必须整体位于 virtualList 视口 clip 之外——
//      virtualList 视口底部 ≤ 共享区块顶部（修复让出 12px 间距）。
//      回归（claude_ui_list.cpp drawProfileListPanel）：修复前视口按 scrollH 全高、
//      区块只压自己的绝对 Y，后建区块叠在带 clip 的视口底带上 → ≥5 profile 时末
//      1-2 张卡被盖、滚到底显示不全、点击误开片段编辑。修复 = 视口高 listH 让出
//      区块高（clamp 保底 96px）。
//   ② 组 B P1/P4 锚定不变量（2026-09-28）：贴底元素锚容器底 ——
//      · 「+ 新建片段」底边 == 底部按钮行上沿 − 12（区块**钉面板底**，
//        用户实测反馈「新建片段的按钮原本底部对齐的，现在没了」）
//      · 区块整体落在面板内（blockTop + sharedH ≤ bodyH）
//      · 详情动作区在四个页签下 **actY 完全相等**（用户主诉：四页签下方区域
//        高度不对 / 分隔线位置不一致），且动作区不越出详情面板底
//   ③ P2：最近调用行数纯按可用高推（无 10 行硬上限），actions 不越界
//
// 断言机制（几何换算同无头测试 test_claude_ui.cpp
// claude_shared_block_outside_viewport / claude_detail_actions_anchor_four_tabs）：
//   viewportBottom = claude.profiles.scroll.frame.y + frame.height
//   blockTop       = claude.shared.block.frame.y
//   断言 blockTop - viewportBottom >= 11.5（钉底后恒为 12px 间距；回退
//   listH = scrollH 后区块叠上视口 → 红；spec §3-probe-full 类 7(b) P1）
// 尺寸矩阵 {1200×720 最小窗, 1200×1000 默认, 1262×801 真实 macOS 页面}
// 三档各跑一遍（--size 只影响探针窗口，断言用的逻辑尺寸逐档传入）。
// 1262×801 = 本机 macOS 真实页面尺寸（窗口 1470×801 − 侧边栏 208）——「真实
// 窗口尺寸下的区域比例」类问题在无头侧唯一能覆盖的方式（方案 §5 全局项要求）。

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

    // 造数：5 profile + 8 共享片段（spec §3-probe-full 类 7(b) 搭法 + 组 B P4：
    // 8 个片段是「区块自然高 > 可显高」的档位——旧实现在这里会把「+ 新建片段」
    // 推到面板外/被底部按钮压住；现在区块钉底 + 卡片在块内滚）
    ProbeEnv env;
    std::vector<int64_t> ids;
    bool seedOk = true;
    for (int i = 0; i < 8 && seedOk; ++i) {
        auto f = env.svc->createSharedConfig(
            "片段" + std::to_string(static_cast<char>('A' + i)),
            std::string("{\"MY_") + static_cast<char>('A' + i) +
                "\":\"v\"}");
        if (f.isFail()) seedOk = false;
    }
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
    // 30 条调用记录（P2：行数按可用高推的判别力来源——旧实现恒锁 10 行）
    if (seedOk && !ids.empty()) {
        const std::time_t base = std::time(nullptr);
        for (int i = 0; i < 30 && seedOk; ++i) {
            module::claude::ClaudeService::CallLogEntry e;
            e.profileId = ids[0];
            e.method = "POST";
            e.path = "/v1/messages";
            e.statusCode = (i % 7 == 0) ? 429 : 200;
            e.latencyMs = 90 + i;
            e.startedAt = base + i;
            if (env.svc->logCall(e).isFail()) seedOk = false;
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
    bool passed = true;

    core::dsl::Ui* ui = nullptr;
    // 单帧标准单元：pollOnce（输入派发 + 交互）→ compose（recompose + layout，
    // 帧即有效）。逻辑尺寸逐档传入（探针窗口大小与断言尺寸解耦）
    float frameW = static_cast<float>(args.windowW);
    float frameH = static_cast<float>(args.windowH);
    auto composeFrame = [&]() {
        probe::composeModuleFrame(runtime, window, pageId, frameW, frameH,
                                  [&](eui::Ui& u, const eui::Screen& screen) {
                                      ui = &u;
                                      mod->compose(u, screen);
                                  });
    };

    // 尺寸矩阵：最小窗 / 默认 / **真实 macOS 页面尺寸 1262×801**
    const std::vector<std::pair<float, float>> sizes = {
        {1200.0f, 720.0f}, {1200.0f, 1000.0f}, {1262.0f, 801.0f}};

    for (const auto& [w, h] : sizes) {
        frameW = w;
        frameH = h;
        composeFrame();
        const std::string tag = std::to_string(static_cast<int>(w)) + "x" +
                                std::to_string(static_cast<int>(h));

        const core::dsl::Element* scroll =
            ui != nullptr ? ui->find("claude.profiles.scroll") : nullptr;
        const core::dsl::Element* block =
            ui != nullptr ? ui->find("claude.shared.block") : nullptr;
        if (scroll == nullptr || block == nullptr) {
            std::cerr << "claude_probes[" << tag << "]: 'claude.profiles.scroll' / "
                         "'claude.shared.block' not found (scroll="
                      << (scroll != nullptr ? "x" : "-") << ", block="
                      << (block != nullptr ? "x" : "-") << ")\n";
            passed = false;
            continue;
        }

        // ① virtualList 视口底 ≤ 共享区块顶（钉底后间距恒 12px；回退
        // scrollH 全高后区块叠在视口底带上 → viewportBottom > blockTop → 红）
        const float viewportBottom = scroll->frame.y + scroll->frame.height;
        const float blockTop = block->frame.y;
        if (blockTop - viewportBottom < 11.5f) {
            std::cerr << "claude_probes[" << tag << "]: shared block top "
                      << blockTop << " overlaps virtualList viewport bottom "
                      << viewportBottom << " (gap "
                      << (blockTop - viewportBottom)
                      << " < 11.5; bug shape: listH = scrollH full height)\n";
            passed = false;
        }

        const core::dsl::Element* btnNew =
            ui != nullptr ? ui->find("claude.profiles.new") : nullptr;
        if (btnNew == nullptr) {
            std::cerr << "claude_probes[" << tag
                      << "]: 'claude.profiles.new' not found\n";
            passed = false;
        } else {
            const float blockBottom = block->frame.y + block->frame.height;
            if (blockBottom > btnNew->frame.y + 0.5f) {
                std::cerr << "claude_probes[" << tag
                          << "]: shared block bottom " << blockBottom
                          << " overlaps bottom buttons row y=" << btnNew->frame.y
                          << '\n';
                passed = false;
            }
        }

        // ② P4 锚定不变量：「+ 新建片段」底 == 底部按钮行上沿 − 12（恒等式，
        // 与片段数/窗口高无关），且区块整体落在面板内
        const core::dsl::Element* sharedNew =
            ui != nullptr ? ui->find("claude.shared.new") : nullptr;
        const core::dsl::Element* panel =
            ui != nullptr ? ui->find("claude.profiles") : nullptr;
        if (sharedNew == nullptr || btnNew == nullptr || panel == nullptr) {
            std::cerr << "claude_probes[" << tag
                      << "]: 'claude.shared.new' / 'claude.profiles' not found\n";
            passed = false;
        } else {
            const float sharedBtnBottom =
                sharedNew->frame.y + sharedNew->frame.height;
            const float blockBottom = block->frame.y + block->frame.height;
            if (std::fabs(sharedBtnBottom - blockBottom) > 0.5f) {
                std::cerr << "claude_probes[" << tag
                          << "]: '+ 新建片段' bottom " << sharedBtnBottom
                          << " != block bottom " << blockBottom
                          << " (按钮不再是区块的固定尾部)\n";
                passed = false;
            }
            if (std::fabs(sharedBtnBottom - (btnNew->frame.y - 12.0f)) > 0.5f) {
                std::cerr << "claude_probes[" << tag
                          << "]: '+ 新建片段' bottom " << sharedBtnBottom
                          << " != 底部按钮行上沿 − 12 (" << (btnNew->frame.y - 12.0f)
                          << ") —— 区块未钉面板底\n";
                passed = false;
            }
            const float panelBottom = panel->frame.y + panel->frame.height;
            if (blockBottom > panelBottom + 0.5f) {
                std::cerr << "claude_probes[" << tag << "]: shared block bottom "
                          << blockBottom << " 越出面板底 " << panelBottom << '\n';
                passed = false;
            }
        }

        // ③ P1 锚定不变量：详情动作区在四个页签下 actY 完全相等，且不越出面板底
        //（用户主诉：四页签下方区域高度不对 / 分隔线位置不一致）
        float actY0 = 0.0f;
        for (int tab = 0; tab < 4; ++tab) {
            if (tab > 0) {
                core::dsl::Element* hit = ui != nullptr
                    ? ui->find("claude.detail.tabs.hit." + std::to_string(tab))
                    : nullptr;
                if (hit == nullptr || hit->onClick == nullptr) {
                    std::cerr << "claude_probes[" << tag
                              << "]: detail tab hit " << tab << " not clickable\n";
                    passed = false;
                    break;
                }
                hit->onClick();
                composeFrame();
            }
            const core::dsl::Element* act =
                ui != nullptr ? ui->find("claude.detail.actions") : nullptr;
            const core::dsl::Element* actPanel =
                ui != nullptr ? ui->find("claude.detail") : nullptr;
            if (act == nullptr || actPanel == nullptr) {
                std::cerr << "claude_probes[" << tag
                          << "]: 'claude.detail.actions' / 'claude.detail' "
                             "not found (tab "
                          << tab << ")\n";
                passed = false;
                break;
            }
            if (tab == 0) {
                actY0 = act->frame.y;
            } else if (std::fabs(act->frame.y - actY0) > 0.5f) {
                std::cerr << "claude_probes[" << tag << "]: tab " << tab
                          << " actY=" << act->frame.y << " != tab0 actY="
                          << actY0 << "（四页签未锚同一条水平线）\n";
                passed = false;
            }
            const float detailBottom =
                actPanel->frame.y + actPanel->frame.height;
            if (act->frame.y + act->frame.height > detailBottom + 0.5f) {
                std::cerr << "claude_probes[" << tag << "]: tab " << tab
                          << " 动作区底 " << (act->frame.y + act->frame.height)
                          << " 越出详情面板底 " << detailBottom << '\n';
                passed = false;
            }
        }
        // 回到第 0 页签（下一档的起点一致）
        if (core::dsl::Element* hit0 =
                ui != nullptr ? ui->find("claude.detail.tabs.hit.0") : nullptr) {
            if (hit0->onClick != nullptr) {
                hit0->onClick();
                composeFrame();
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