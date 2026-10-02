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
//      · 详情动作区在全部页签下 **actY 完全相等**（用户主诉：页签下方区域
//        高度不对 / 分隔线位置不一致；2026-09-30 batch-fix B 起五页签），
//        且动作区不越出详情面板底
//   ③ P2：最近调用行数纯按可用高推（无 10 行硬上限），actions 不越界
//   ④ batch-fix C（2026-09-30）：7 键操作组最小窗键宽实测（裁决 ⑥——
//      单行等分键宽 ≥ 四字文案宽 + 呼吸，不达标即切两行的红灯）
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

#include "probes/probe_harness.h"  // GL/GLFW 头由 harness 给（glad 路线；见 settings_cards_probes.cpp:43 同款说明）
#include "test_support.h"
#include "app_registry.h"
#include "core/module_interface.h"
#include "core/render/render_backend.h"  // --dump：真渲染 + 读回后备缓冲
#include "core/theme.h"

#include <fstream>

#include "module_claude_proxy/claude_service.h"
#include "provider_config.h"   // ⑦ 槽位表 / models_config 造数 + 序列化

#include <algorithm>
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
    // advanced-json-editor §8：片段 = 高级属性 JSON 预设（v11 后不再携带 env 键）
    for (int i = 0; i < 8 && seedOk; ++i) {
        auto f = env.svc->createSharedConfig(
            "片段" + std::to_string(static_cast<char>('A' + i)),
            std::string("{\"hook") + static_cast<char>('A' + i) +
                "\":{\"on\":" + (i % 2 == 0 ? "true" : "false") + "}}");
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
    // profile 0：高级设置 JSON + 绑定片段 A（详情「高级配置」pane 键来源标注
    // 与表单高级配置页 JSON 框的落图数据源，advanced-json-editor §7/§8.5）
    if (seedOk && !ids.empty()) {
        module::claude::Profile p = env.svc->getProfile(ids[0]).value();
        p.extraSettingsJson =
            R"({"model": "probe-model", "statusLine": {"type": "command"}, "permissions": {"allow": ["Bash"]}})";
        p.sharedConfigId = env.svc->listSharedConfigs().value()[0].id;
        // 批次 F（2026-10-02）：模型映射 1M 开关三态造数（探针 ⑦ 的判别力来源）
        //   def   支持 + 串尾 [1M]     → 勾上（回显）
        //   opus  支持 + 无标记        → 未勾（滑块在左端）
        //   haiku 不支持 + 串尾 [1M]   → 禁用（灰轨道）+ 不勾（双重条件）
        module::claude::ModelsConfig mc;
        mc.def.model = "probe-model[1M]";
        mc.opus.model = "probe-opus";
        mc.haiku.model = "probe-haiku[1M]";
        auto mcText = module::claude::serializeModelsConfig(mc);
        if (mcText.isOk()) {
            p.modelsConfigJson = mcText.value();
        } else {
            seedOk = false;
        }
        if (seedOk) seedOk = env.svc->updateProfile(p).isOk();
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
    // 真实命中分发：press + release 排队 → 下一 composeFrame 内 update 消费
    //（settings_dropdown_z_probe 同款；⑤ 下拉 z 序用例走真命中路径）
    auto clickPoint = [&](float cx, float cy) {
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Press, {});
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Release, {});
    };
    auto clickElement = [&](const core::dsl::Element& element) {
        clickPoint(element.frame.x + element.frame.width * 0.5f,
                   element.frame.y + element.frame.height * 0.5f);
    };
    auto floatEq = [](float a, float b) {
        return std::fabs(a - b) < 0.5f;
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

        // ⑤ 2026-09-30 共享列表限高 2 条：条目区（scrollView）可视区恰 2 整行
        //    （128 = 2×(卡 56 + 行距 8)，区块未被极矮窗压扁时），第 3 条起在视口
        //    外要滚才可见，且 8 片段确有可滚余量（「+ 新建片段」固定尾部由②锁）。
        //    变异判别：改回随条目数涨 → 8 条时区块自然高 588、可视区 512 ≠ 128
        //    → 红
        const core::dsl::Element* sharedScroll =
            ui != nullptr ? ui->find("claude.shared.scroll") : nullptr;
        if (sharedScroll == nullptr) {
            std::cerr << "claude_probes[" << tag
                      << "]: 'claude.shared.scroll' not found\n";
            passed = false;
        } else {
            if (block->frame.height >= 203.5f &&
                std::fabs(sharedScroll->frame.height - 128.0f) > 0.5f) {
                std::cerr << "claude_probes[" << tag
                          << "]: shared scroll viewport "
                          << sharedScroll->frame.height
                          << " != 128 (2 行；bug shape: 区块自然高随条目数涨)\n";
                passed = false;
            }
            const core::dsl::Element* card2 =
                ui != nullptr ? ui->find("claude.shared.card.2.bg") : nullptr;
            if (card2 == nullptr) {
                std::cerr << "claude_probes[" << tag
                          << "]: 'claude.shared.card.2.bg' not found\n";
                passed = false;
            } else if (card2->frame.y <
                       sharedScroll->frame.y + sharedScroll->frame.height - 0.5f) {
                std::cerr << "claude_probes[" << tag << "]: card 2 top "
                          << card2->frame.y << " 在可视区内（底 "
                          << (sharedScroll->frame.y + sharedScroll->frame.height)
                          << "）—— offset 0 下可视区超过 2 条\n";
                passed = false;
            }
            if (!(sharedScroll->scrollMaxOffset > 0.0f)) {
                std::cerr << "claude_probes[" << tag
                          << "]: 8 片段但 scrollMaxOffset == 0（超出 2 条却无"
                             "可滚余量）\n";
                passed = false;
            }
        }

        // ③ P1 锚定不变量：详情动作区在五个页签下 actY 完全相等，且不越出面板底
        //（用户主诉：页签下方区域高度不对 / 分隔线位置不一致；tab 3 = 2026-09-30
        // batch-fix B 新增「高级配置」，最近调用顺移 tab 4 —— 既有 hit.N 导航锚
        // 按索引导航，循环上界同步 +1）
        float actY0 = 0.0f;
        for (int tab = 0; tab < 5; ++tab) {
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
                          << actY0 << "（五页签未锚同一条水平线）\n";
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

        // ④ batch-C C-3：动作行由 7 键收为 4 键（启用/测试连通/编辑/运行）后的
        // **最小窗右栏**键宽实测。单行等分每键宽 = (w0 − 3×12) / 4（gap=12）；
        // 达标判据 = 键宽 ≥ 最长文案宽（四字键「测试连通」× fontSizeSmall）
        // + 两侧 ≥4px 呼吸。三键（删除/建副本/复制配置）已从动作行删除，节点
        // 不得再现（残留 = 回归）。
        {
            const core::dsl::Element* actRow =
                ui != nullptr ? ui->find("claude.detail.actions") : nullptr;
            const core::dsl::Element* run =
                ui != nullptr ? ui->find("claude.act.run.bg") : nullptr;
            const core::dsl::Element* en =
                ui != nullptr ? ui->find("claude.act.enable.bg") : nullptr;
            if (actRow == nullptr || run == nullptr || en == nullptr) {
                std::cerr << "claude_probes[" << tag
                          << "]: 'claude.detail.actions' / 'claude.act.run.bg' / "
                             "'claude.act.enable.bg' not found\n";
                passed = false;
            } else {
                for (const char* gone : {"claude.act.del.bg",
                                         "claude.act.dup.bg",
                                         "claude.act.copyconfig.bg"}) {
                    if (ui->find(gone) != nullptr) {
                        std::cerr << "claude_probes[" << tag << "]: '" << gone
                                  << "' 应已从动作行删除（batch-C C-3）\n";
                        passed = false;
                    }
                }
                const float keyW = run->frame.width;
                const float longestText =
                    4.0f * core::theme::current().sizes.fontSizeSmall;
                const float need = longestText + 8.0f;
                if (keyW < need) {
                    std::cerr << "claude_probes[" << tag << "]: 4 键 actW "
                              << keyW << " < 文案宽+呼吸 " << need
                              << "（四字键 @fontSizeSmall）—— 需切两行按钮组\n";
                    passed = false;
                } else {
                    std::cout << "claude_probes[" << tag << "]: 4 键 actW="
                              << keyW << " ≥ 文案宽+呼吸 " << need
                              << "（单行达标）\n";
                }
                // 等分 + 右缘对齐（与无头几何断言同口径，最小窗档也核）
                const float rowRight = actRow->frame.x + actRow->frame.width;
                const float runRight = run->frame.x + run->frame.width;
                if (std::fabs(rowRight - runRight) > 0.5f) {
                    std::cerr << "claude_probes[" << tag << "]: 末键右缘 "
                              << runRight << " != 动作行右缘 " << rowRight
                              << "（等分溢出）\n";
                    passed = false;
                }
            }
        }

        // ⑤ 表单高级页双下拉 z 序（2026-09-30，与 settings 桌宠卡同类等值 z
        // 碰撞；「弹窗内下拉」场景代表）：dialect.ddwrap(y122) / fallback.
        // ddwrap(y192) / shared.ddwrap(y262) 三连下拉同挂在对话框内容容器下，
        // 子树上界同为字段档 20 → 后绘的下方行盖住上方展开弹层。修 =
        // EUI-NEO dropdown open 态 popup 子树 900 档（patch 生产线）。判据：
        //   a) assertDrawnAbove：dialect popup 与 fallback 字段的 LCA 处，
        //      popup 侧子级 subtreeMaxZ(900) > 字段侧(20)；
        //   b) 重叠区真命中路由：点「dialect 弹层首项 ∩ fallback 字段」交集
        //      中心（3 项弹层与 fallback 行相交——dialog 局部几何，窗尺寸无
        //      关），点它必须选中方言并收起，fallback 下拉不得被误开。
        {
            composeFrame();
            const core::dsl::Element* card =
                ui != nullptr ? ui->find("claude.profile.0.bg") : nullptr;
            if (card == nullptr || card->onClick == nullptr) {
                std::cerr << "claude_probes[" << tag
                          << "]: 'claude.profile.0.bg' not found (⑤)\n";
                passed = false;
            } else {
                card->onClick();
                composeFrame();
                // 逐帧重查：compose 重建元素树，跨 compose 持有的指针作废
                //（指针复用表现为 onClick 空 → bad_function_call）
                if (auto* edit = ui->find("claude.act.edit.bg");
                    edit != nullptr && edit->onClick != nullptr) {
                    edit->onClick();
                    composeFrame();
                }
                if (auto* tab3 = ui->find("claude.form.tabs.hit.3");
                    tab3 != nullptr && tab3->onClick != nullptr) {
                    tab3->onClick();
                    composeFrame();
                }
                {
                    const std::string ddDialect =
                        "claude.form.advanced.dialect.dd";
                    const std::string ddFallback =
                        "claude.form.basic.fallback.dd";
                    const core::dsl::Element* dialectField =
                        ui != nullptr ? ui->find(ddDialect + ".field") : nullptr;
                    const core::dsl::Element* fallbackField =
                        ui != nullptr ? ui->find(ddFallback + ".field") : nullptr;
                    if (dialectField == nullptr || fallbackField == nullptr) {
                        std::cerr << "claude_probes[" << tag
                                  << "]: dialect/fallback field missing (⑤)\n";
                        passed = false;
                    } else {
                        // 开方言下拉（真命中，与生产点击路径一致）
                        clickElement(*dialectField);
                        composeFrame();
                        // recompose 重建元素树 → 开前捕获的指针作废，重查
                        const core::dsl::Element* popup =
                            ui != nullptr
                                ? ui->find(ddDialect + ".popup")
                                : nullptr;
                        const core::dsl::Element* item0 =
                            ui != nullptr
                                ? ui->find(ddDialect + ".item.0")
                                : nullptr;
                        const core::dsl::Element* fallbackFieldNow =
                            ui != nullptr
                                ? ui->find(ddFallback + ".field")
                                : nullptr;
                        if (popup == nullptr || item0 == nullptr ||
                            fallbackFieldNow == nullptr) {
                            std::cerr << "claude_probes[" << tag
                                      << "]: dialect popup/item0 missing after "
                                         "open (⑤)\n";
                            passed = false;
                        } else {
                            if (!floatEq(popup->opacity, 1.0f)) {
                                std::cerr << "claude_probes[" << tag
                                          << "]: dialect popup opacity="
                                          << popup->opacity << " (⑤)\n";
                                passed = false;
                            }
                            // 前提守卫：弹层首项 ∩ fallback 字段（几何漂移
                            // 先响这里，防 z 判据落空）
                            if (!probe::detail::framesIntersect(*item0,
                                                                *fallbackFieldNow)) {
                                std::cerr << "claude_probes[" << tag
                                          << "]: 前提失效：dialect 弹层首项"
                                             "与 fallback 字段不相交 (⑤)\n";
                                passed = false;
                            }
                            if (!probe::assertDrawnAbove(
                                    *ui, ddDialect + ".popup",
                                    ddFallback + ".field")) {
                                std::cerr << "claude_probes[" << tag
                                          << "]: 绘制序断言失败：dialect 弹层"
                                             "未压过下方 fallback 行 (⑤)\n";
                                passed = false;
                            }
                            // 重叠区真命中：点「弹层首项 ∩ fallback 字段」
                            // 交集中心（项中心可能在交集外，那样命中序根本没
                            // 被考验）→ 方言选中并收起，fallback 不得被误开
                            //（pre-fix 被其字段截获）
                            const float ox = std::max(item0->frame.x,
                                                      fallbackFieldNow->frame.x);
                            const float oy = std::max(item0->frame.y,
                                                      fallbackFieldNow->frame.y);
                            const float oRight = std::min(
                                item0->frame.x + item0->frame.width,
                                fallbackFieldNow->frame.x +
                                    fallbackFieldNow->frame.width);
                            const float oBottom = std::min(
                                item0->frame.y + item0->frame.height,
                                fallbackFieldNow->frame.y +
                                    fallbackFieldNow->frame.height);
                            clickPoint((ox + oRight) * 0.5f,
                                       (oy + oBottom) * 0.5f);
                            composeFrame();
                            const core::dsl::Element* dialectHit =
                                ui != nullptr ? ui->find(ddDialect + ".popup.hit")
                                              : nullptr;
                            const core::dsl::Element* fallbackHit =
                                ui != nullptr
                                    ? ui->find(ddFallback + ".popup.hit")
                                    : nullptr;
                            if (dialectHit == nullptr || !dialectHit->disabled) {
                                std::cerr << "claude_probes[" << tag
                                          << "]: dialect 弹层未收起 (⑤)\n";
                                passed = false;
                            }
                            if (fallbackHit == nullptr || !fallbackHit->disabled) {
                                std::cerr << "claude_probes[" << tag
                                          << "]: fallback 下拉被误开（重叠区"
                                             "点击被下方字段截获）(⑤)\n";
                                passed = false;
                            }
                        }
                    }
                    // 收尾：取消关弹窗（不落任何改动），状态归零供下一尺寸档
                    if (auto* cancel = ui->find("claude.dialog.form.cancel.bg");
                        cancel != nullptr && cancel->onClick != nullptr) {
                        cancel->onClick();
                        composeFrame();
                    }
                }
            }
        }
        // ⑥ batch-C C-2：全屏 JSON 覆盖层的三键布局与「完成」删除——**默认跑**
        //（不在 --dump 守卫内：门禁勾选不能建立在一条默认不执行的断言上）。
        // 编辑 profile 0 → 表单 tab3 → 「全屏编辑」→ 三键同行/不溢出/
        // claude.jsonfs.done 不得再现 → 「放弃修改」真命中（真指针）关覆盖层。
        {
            composeFrame();
            if (auto* card = ui != nullptr ? ui->find("claude.profile.0.bg") : nullptr;
                card != nullptr && card->onClick != nullptr) {
                card->onClick();
                composeFrame();
            }
            if (auto* edit = ui != nullptr ? ui->find("claude.act.edit.bg") : nullptr;
                edit != nullptr && edit->onClick != nullptr) {
                edit->onClick();
                composeFrame();
            }
            if (auto* tab3 =
                    ui != nullptr ? ui->find("claude.form.tabs.hit.3") : nullptr;
                tab3 != nullptr && tab3->onClick != nullptr) {
                tab3->onClick();
                composeFrame();
            }
            core::dsl::Element* fsBtn =
                ui != nullptr ? ui->find("claude.form.advanced.json.fs.bg")
                              : nullptr;
            if (fsBtn == nullptr || fsBtn->onClick == nullptr) {
                std::cerr << "claude_probes[" << tag
                          << "]: 'claude.form.advanced.json.fs.bg' not found (⑥)\n";
                passed = false;
            } else {
                fsBtn->onClick();
                composeFrame();
                const core::dsl::Element* fmt = ui->find("claude.jsonfs.format.bg");
                const core::dsl::Element* save = ui->find("claude.jsonfs.apply.bg");
                const core::dsl::Element* disc = ui->find("claude.jsonfs.discard.bg");
                const core::dsl::Element* bg = ui->find("claude.jsonfs.bg");
                if (fmt == nullptr || save == nullptr || disc == nullptr ||
                    bg == nullptr) {
                    std::cerr << "claude_probes[" << tag
                              << "]: 全屏遮罩/三键缺节点（格式化/保存/放弃修改）(⑥)\n";
                    passed = false;
                } else {
                    if (!(floatEq(fmt->frame.y, save->frame.y) &&
                          floatEq(save->frame.y, disc->frame.y))) {
                        std::cerr << "claude_probes[" << tag
                                  << "]: 全屏三键不在同一行 (⑥)\n";
                        passed = false;
                    }
                    if (!(save->frame.x >=
                              fmt->frame.x + fmt->frame.width - 0.5f &&
                          disc->frame.x >=
                              save->frame.x + save->frame.width - 0.5f)) {
                        std::cerr << "claude_probes[" << tag
                                  << "]: 全屏三键重叠或顺序错 (⑥)\n";
                        passed = false;
                    }
                    if (disc->frame.x + disc->frame.width > frameW - 27.0f) {
                        std::cerr << "claude_probes[" << tag << "]: 「放弃修改」右缘 "
                                  << (disc->frame.x + disc->frame.width)
                                  << " 溢出覆盖层（窗宽 " << frameW
                                  << "，内缩 28）(⑥)\n";
                        passed = false;
                    }
                    if (ui->find("claude.jsonfs.done.bg") != nullptr) {
                        std::cerr << "claude_probes[" << tag
                                  << "]: 'claude.jsonfs.done' 应已删除"
                                     "（batch-C C-2）(⑥)\n";
                        passed = false;
                    }
                    // 「放弃修改」真命中（真指针 press+release → 下一帧消费）
                    clickElement(*disc);
                    composeFrame();
                    if (ui->find("claude.jsonfs.root") != nullptr) {
                        std::cerr << "claude_probes[" << tag
                                  << "]: 点「放弃修改」后全屏未关 (⑥)\n";
                        passed = false;
                    }
                }
            }
            // 收尾：取消关表单（不落任何改动），状态归零供下一尺寸档
            if (auto* cancel =
                    ui != nullptr ? ui->find("claude.dialog.form.cancel.bg")
                                  : nullptr;
                cancel != nullptr && cancel->onClick != nullptr) {
                cancel->onClick();
                composeFrame();
            }
        }
        // ⑦ 批次 F（2026-10-02）：模型映射 pane 的「1M」开关几何 ——
        //   a) 每行控件**在行内不错位**：开关命中区完全落在槽位行框内，且与
        //      模型名值文本不相交（控件漏定位 / 漏留宽 → val 文本压到开关下）
        //   b) **禁用态可辨识**：不支持 1M 的槽位（haiku）轨道色 ≠ 支持槽位
        //      （def）轨道色（灰），且勾态回显（def 串尾 [1M] → 滑块在右端；
        //      opus 无标记 → 滑块在左端）
        // 造数（seeding 段）：档0 的 models_config = def[1M] / opus / haiku[1M]
        //（haiku 不支持槽位但串里带标记 —— 双重条件的判别力来源）
        // 收尾：回第 0 页签（下一档起点一致）
        {
            composeFrame();
            if (auto* card =
                    ui != nullptr ? ui->find("claude.profile.0.bg") : nullptr;
                card != nullptr && card->onClick != nullptr) {
                card->onClick();   // 选中（详情 shown 取选中，其次激活）
                composeFrame();
            }
            if (auto* tab1 =
                    ui != nullptr ? ui->find("claude.detail.tabs.hit.1") : nullptr;
                tab1 != nullptr && tab1->onClick != nullptr) {
                tab1->onClick();
                composeFrame();
            }
            const size_t slotCount = module::claude::modelSlotTable().size();
            for (size_t i = 0; i < slotCount; ++i) {
                const std::string idx = std::to_string(i);
                const core::dsl::Element* row =
                    ui != nullptr ? ui->find("claude.slot.bg." + idx) : nullptr;
                const core::dsl::Element* val =
                    ui != nullptr ? ui->find("claude.slot.val." + idx) : nullptr;
                const core::dsl::Element* hit =
                    ui != nullptr ? ui->find("claude.slot.onem." + idx + ".hit")
                                  : nullptr;
                const core::dsl::Element* track =
                    ui != nullptr ? ui->find("claude.slot.onem." + idx + ".track")
                                  : nullptr;
                if (row == nullptr || val == nullptr || hit == nullptr ||
                    track == nullptr) {
                    std::cerr << "claude_probes[" << tag << "]: 槽位行 " << idx
                              << " 缺节点（row/val/onem.hit/onem.track）(⑦)\n";
                    passed = false;
                    continue;
                }
                // a) 控件在行内：左右不越行框
                const float rowL = row->frame.x;
                const float rowR = row->frame.x + row->frame.width;
                const float hitL = hit->frame.x;
                const float hitR = hit->frame.x + hit->frame.width;
                if (hitL < rowL - 0.5f || hitR > rowR + 0.5f) {
                    std::cerr << "claude_probes[" << tag << "]: 槽位 " << idx
                              << " 1M 开关越出行框（hit "
                              << hitL << ".." << hitR << " vs row "
                              << rowL << ".." << rowR << "）(⑦)\n";
                    passed = false;
                }
                // a2) 与模型名值文本不相交（估宽有误差，取 -0.5 容差）
                const float valR = val->frame.x + val->frame.width;
                if (valR > hitL + 0.5f) {
                    std::cerr << "claude_probes[" << tag << "]: 槽位 " << idx
                              << " 值文本右缘 " << valR << " 压进 1M 开关左缘 "
                              << hitL << "（未留列宽）(⑦)\n";
                    passed = false;
                }
                // a3) 轨道垂直居中于行（32 高行内）
                const float trackMid =
                    track->frame.y + track->frame.height * 0.5f;
                const float rowMid = row->frame.y + row->frame.height * 0.5f;
                if (std::fabs(trackMid - rowMid) > 1.0f) {
                    std::cerr << "claude_probes[" << tag << "]: 槽位 " << idx
                              << " 1M 轨道未垂直居中（track 中 " << trackMid
                              << " vs 行中 " << rowMid << "）(⑦)\n";
                    passed = false;
                }
            }
            // b) 勾态回显 + 禁用态色差（三态造数见 seeding）
            const core::dsl::Element* trackDef =
                ui != nullptr ? ui->find("claude.slot.onem.0.track") : nullptr;
            const core::dsl::Element* knobDef =
                ui != nullptr ? ui->find("claude.slot.onem.0.knob") : nullptr;
            const core::dsl::Element* trackOpus =
                ui != nullptr ? ui->find("claude.slot.onem.2.track") : nullptr;
            const core::dsl::Element* knobOpus =
                ui != nullptr ? ui->find("claude.slot.onem.2.knob") : nullptr;
            const core::dsl::Element* trackHaiku =
                ui != nullptr ? ui->find("claude.slot.onem.3.track") : nullptr;
            if (trackDef == nullptr || knobDef == nullptr ||
                trackOpus == nullptr || knobOpus == nullptr ||
                trackHaiku == nullptr) {
                std::cerr << "claude_probes[" << tag
                          << "]: 1M 开关三态节点缺失 (⑦)\n";
                passed = false;
            } else {
                const float defOffset = knobDef->frame.x - trackDef->frame.x;
                const float opusOffset = knobOpus->frame.x - trackOpus->frame.x;
                // 勾态 = 滑块相对轨道的偏移；未勾也有 margin 偏移（非 0）→
                // 用相对比较，不写绝对阈值
                if (!(defOffset > opusOffset + 5.0f)) {
                    std::cerr << "claude_probes[" << tag
                              << "]: def 槽位串尾 [1M] 未回显为勾上（滑块偏移 "
                              << defOffset << " vs opus 未勾 " << opusOffset
                              << "）(⑦)\n";
                    passed = false;
                }
                const bool sameTrack =
                    trackHaiku->color.r == trackDef->color.r &&
                    trackHaiku->color.g == trackDef->color.g &&
                    trackHaiku->color.b == trackDef->color.b &&
                    trackHaiku->color.a == trackDef->color.a;
                if (sameTrack) {
                    std::cerr << "claude_probes[" << tag
                              << "]: 不支持 1M 的槽位轨道色与支持槽位相同"
                                 "（禁用态不可辨识）(⑦)\n";
                    passed = false;
                }
            }
            if (auto* back =
                    ui != nullptr ? ui->find("claude.detail.tabs.hit.0") : nullptr;
                back != nullptr && back->onClick != nullptr) {
                back->onClick();
                composeFrame();
            }
        }
    }

    // ---- 可视化落图（--dump <prefix>，about_probe --manual 同款）：真渲染 +
    // glReadPixels → PPM（P6），不依赖系统截屏（OpenGL 窗口 screencapture 有
    // 旧帧坑，锁屏/满屏应用下也不可用）。⚠️ 本模式只点**非剪贴板**动作——
    // 绝不点 claude.act.copyconfig.bg：真窗口后端下 setClipboardText 会真写
    // 运行者系统剪贴板（spec §4.5 风险 3 / 头部②同款旁路口径）
    std::string dumpPrefix;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--dump" && i + 1 < argc) {
            dumpPrefix = argv[++i];
        }
    }
    if (!dumpPrefix.empty()) {
        // 断言循环用的逻辑尺寸与探针窗口解耦；落图按窗口真实客户区来
        frameW = static_cast<float>(args.windowW);
        frameH = static_cast<float>(args.windowH);
        auto* glfwWin = static_cast<GLFWwindow*>(window);
        int fbW = 0, fbH = 0;
        glfwGetFramebufferSize(glfwWin, &fbW, &fbH);
        float scaleX = 1.0f, scaleY = 1.0f;
        glfwGetWindowContentScale(glfwWin, &scaleX, &scaleY);
        const float renderScale = scaleX > 0.0f ? scaleX : 1.0f;
        glfwShowWindow(glfwWin);
        glfwFocusWindow(glfwWin);
        auto renderBackend = core::render::createRenderBackend(window);
        if (renderBackend == nullptr || !renderBackend->initialize()) {
            // 请求了 --dump 却一张都拍不出来 = 视觉证据链为空，同「静默跳过 +
            // 报成功」是同一类假绿，置红（末尾 kExpected 核对会再列一遍）
            std::cerr << "claude_probes: 渲染后端不可用，--dump 一张都拍不出\n";
            passed = false;
            renderBackend.reset();
        }
        const eui::Color clearColor = core::theme::current().colors.background;
        auto renderAndDump = [&](const std::string& path) {
            if (renderBackend == nullptr) return false;
            core::render::ScopedRenderBackend scoped(*renderBackend);
            renderBackend->beginFrame(core::render::RenderSurface{
                window, core::window::nativeWindowInfo(window), fbW, fbH,
                renderScale});
            runtime.render(fbW, fbH, renderScale, clearColor);
            // dump 必须在 present() 之前（swap 后备缓冲内容未定义，about_probe
            // 实测教训）；glReadPixels 同款逐语句窄抑制（macOS GL deprecation）
            std::vector<unsigned char> px(
                static_cast<std::size_t>(fbW) * static_cast<std::size_t>(fbH) * 3u);
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, fbW, fbH, GL_RGB, GL_UNSIGNED_BYTE, px.data());
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
            std::ofstream ofs(path, std::ios::binary);
            bool dumped = true;
            if (!ofs) {
                // 打开失败（路径不可写/磁盘满）不能静默假成功——视觉证据
                // 没落盘就得置红，否则审计会拿到「看起来生成了」的假象
                std::cerr << "claude_probes: dump 打开失败: " << path << "\n";
                passed = false;
                dumped = false;
            } else {
                ofs << "P6\n" << fbW << " " << fbH << "\n255\n";
                for (int row = fbH - 1; row >= 0; --row) {  // GL 原点左下 → 翻转
                    ofs.write(reinterpret_cast<const char*>(
                                  px.data() + static_cast<std::size_t>(row) *
                                                  static_cast<std::size_t>(fbW) * 3u),
                              static_cast<std::streamsize>(fbW) * 3);
                }
                ofs.close();  // 显式 close 后判流状态：写失败/flush 失败在这里暴露
                if (!ofs) {
                    std::cerr << "claude_probes: dump 写入失败: " << path << "\n";
                    passed = false;
                    dumped = false;
                }
            }
            renderBackend->present();
            return dumped;
        };
        // 状态切换后 EUI 过渡动画（弹窗 opacity fade / tabs 指示器位移）需要
        // 若干帧收敛 —— 单帧即拍会抓到半透明中间态（实测 form_adv 半透明）。
        // 40 帧 ≈ 0.67s（pollOnce dt = 1/60），覆盖最长的弹窗 fade
        //
        // 2026-09-30（merge-review R2）：--dump 是**视觉证据链**，缺元素静默
        // 跳过 = probe 报绿但证据链断（少几张图没人知道）。故 dump 路径一律
        // **缺元素即失败**：require 找不到元素 / 找不到所需回调 → 打印缺哪个
        // id、置红、该图不拍。非 dump 断言路径的容错保持原样（那里的
        // `!= nullptr &&` 服务于交互流程推进，不是视觉证据）。
        std::vector<std::string> dumped;   // 实际落盘的图名（末尾核对用）
        auto dumpState = [&](const std::string& name) {
            for (int i = 0; i < 40; ++i) {
                composeFrame();
            }
            if (renderAndDump(dumpPrefix + "_" + name + ".ppm")) {
                dumped.push_back(name);
            }
        };
        // 目标元素守卫：缺元素 / 缺回调 → 打印「缺哪个 id + 哪一步」并置红，
        // 返回 nullptr 让调用方短路（该分支不拍图，末尾核对再报一次）。
        auto requireEl = [&](const std::string& step,
                             const std::string& id,
                             const char* callback) {
            core::dsl::Element* el = ui != nullptr ? ui->find(id) : nullptr;
            if (el == nullptr) {
                std::cerr << "claude_probes: --dump " << step << " 目标元素 '"
                          << id << "' 未找到——该图不拍，视觉证据链断\n";
                passed = false;
                return static_cast<core::dsl::Element*>(nullptr);
            }
            const bool hasClick = el->onClick != nullptr;
            const bool hasCtx = el->onContextMenu != nullptr;
            if ((std::string(callback) == "onClick" && !hasClick) ||
                (std::string(callback) == "onContextMenu" && !hasCtx)) {
                std::cerr << "claude_probes: --dump " << step << " 目标元素 '"
                          << id << "' 缺 " << callback << " 回调——该图不拍，"
                          << "视觉证据链断\n";
                passed = false;
                return static_cast<core::dsl::Element*>(nullptr);
            }
            return el;
        };
        // dump 目标图清单：末尾逐项核对，缺一张即红（与 require 互为双保险：
        // require 拦的是「进不去」，这里是「图没落地」）
        const std::vector<std::string> expected = {
            "actions4", "detail_adv", "form_adv", "json_fs",
            "shared_dialog", "ctx_menu"};
        composeFrame();
        // 选中 profile 0（详情 shown 优先用户选中——键来源标注 / JSON 框预填
        // 的落图前提）
        if (auto* card = requireEl("选中 profile 0", "claude.profile.0.bg",
                                  "onClick")) {
            card->onClick();
            composeFrame();
        }
        // ① 详情 tab 0：动作行 4 键（batch-C C-3 后：启用/测试连通/编辑/运行）
        dumpState("actions4");
        // ② 详情 tab 3：高级配置 pane（batch-fix B）
        if (auto* hit = requireEl("切详情 tab3", "claude.detail.tabs.hit.3",
                                  "onClick")) {
            hit->onClick();
            dumpState("detail_adv");
            if (auto* hit0 = requireEl("复位详情 tab0", "claude.detail.tabs.hit.0",
                                      "onClick")) {
                hit0->onClick();
            }
        }
        // ③ 表单 tab 3：高级配置页（batch-fix B；编辑 profile 0）
        if (auto* card = requireEl("重选 profile 0", "claude.profile.0.bg",
                                  "onClick")) {
            card->onClick();
            composeFrame();
            if (auto* edit = requireEl("打开编辑表单", "claude.act.edit.bg",
                                      "onClick")) {
                edit->onClick();
                composeFrame();
                if (auto* tab3 = requireEl("切表单 tab3", "claude.form.tabs.hit.3",
                                          "onClick")) {
                    tab3->onClick();
                    dumpState("form_adv");
                    // ⑥ 高级设置 JSON 全屏编辑覆盖层（2026-09-30 json-fullscreen）：
                    //    「全屏编辑」→ 覆盖层（z=1050 盖住表单 dialog）→ 底部三键
                    //    = 格式化/保存/放弃修改（batch-C C-2：原「完成」已删）
                    //    →「放弃修改」回表单（不改任何值），再走既有取消
                    if (auto* fs = requireEl("进 JSON 全屏", "claude.form.advanced.json.fs.bg",
                                            "onClick")) {
                        fs->onClick();
                        dumpState("json_fs");
                        // 按钮行几何/「完成」已删的断言在 **⑥ 默认段**（不在
                        // --dump 守卫内，ctest -L probe 真跑）；此处只做落图导航
                        if (auto* disc = requireEl("JSON 全屏放弃修改",
                                                  "claude.jsonfs.discard.bg",
                                                  "onClick")) {
                            disc->onClick();
                            composeFrame();
                        }
                    }
                    if (auto* cancel = requireEl("关表单", "claude.dialog.form.cancel.bg",
                                                "onClick")) {
                        cancel->onClick();   // 取消，不落任何改动
                    }
                }
            }
        }
        // ⑤ 共享片段编辑弹窗（advanced-json-editor §8.1 JSON 化：name + 高级
        // 设置 JSON 编辑框 + 格式化）
        if (auto* card = requireEl("打开共享片段", "claude.shared.card.0.bg",
                                  "onClick")) {
            card->onClick();
            dumpState("shared_dialog");
            if (auto* cancel = requireEl("关共享弹窗", "claude.dialog.shared.cancel.bg",
                                        "onClick")) {
                cancel->onClick();   // 取消，不落任何改动
            }
        }
        // ④ 列表右键菜单（五项，batch-fix C）
        if (auto* wrap = requireEl("开右键菜单", "claude.profile.0.wrap",
                                  "onContextMenu")) {
            eui::PointerEvent pe;
            eui::Rect rect;
            pe.x = 600.0;
            pe.y = 300.0;
            wrap->onContextMenu(pe, rect);
            dumpState("ctx_menu");
        }
        // 全部目标拍完后只打一次成功日志；任一图缺失 → 报红（不静默放过）
        for (const std::string& name : expected) {
            if (std::find(dumped.begin(), dumped.end(), name) == dumped.end()) {
                std::cerr << "claude_probes: --dump 目标图 '" << name
                          << "' 未落盘（" << dumpPrefix << "_" << name
                          << ".ppm）——视觉证据链断\n";
                passed = false;
            }
        }
        if (dumped.size() == expected.size()) {
            std::cout << "claude_probes: --dump 目标 " << dumped.size() << "/"
                      << expected.size() << " 张全部落盘:";
            for (const std::string& name : dumped) {
                std::cout << ' ' << dumpPrefix << '_' << name << ".ppm";
            }
            std::cout << "\n";
        }
    }

    // ---- 退出前落一张「当前状态图」（--screenshot <路径>，2026-10-03 帧捕获批次）----
    // 与上面 --dump（多张 P6 PPM、需逐个目标 id 在场）互补：本开关是通用的
    // 「跑一次 probe 就拿到此刻界面长什么样」，任何 probe 都能用，不依赖元素 id。
    // 不传该开关时 captureIfRequested 立刻返回，行为与从前逐字一致。
    probe::captureIfRequested(args, window, runtime, &passed);

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