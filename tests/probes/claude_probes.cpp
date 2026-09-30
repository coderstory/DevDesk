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

#include <GLFW/glfw3.h>  // 必须先于 harness：让 GLFW 默认拉平台 GL 头（--dump 用 glReadPixels）
#include "probes/probe_harness.h"
#include "test_support.h"
#include "app_registry.h"
#include "core/module_interface.h"
#include "core/render/render_backend.h"  // --dump：真渲染 + 读回后备缓冲
#include "core/theme.h"

#include <fstream>

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
        seedOk = env.svc->updateProfile(p).isOk();
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

        // ④ batch-fix C 裁决 ⑥：7 键操作组在**最小窗右栏**的键宽 probe 实测。
        // 单行等分每键宽 = (w0 − 6×12) / 7；达标判据 = 键宽 ≥ 最长文案宽
        //（四字键「测试连通/复制配置」× fontSizeSmall）+ 两侧 ≥4px 呼吸。
        // 不达标 → 按裁决切**两行按钮组**（kActionBlockH 单常量传播）；本探针
        // 红 = 布局回归，或键宽退化为需要两行的程度（提示重审该裁决）
        {
            const core::dsl::Element* actRow =
                ui != nullptr ? ui->find("claude.detail.actions") : nullptr;
            const core::dsl::Element* cc =
                ui != nullptr ? ui->find("claude.act.copyconfig.bg") : nullptr;
            const core::dsl::Element* run =
                ui != nullptr ? ui->find("claude.act.run.bg") : nullptr;
            if (actRow == nullptr || cc == nullptr || run == nullptr) {
                std::cerr << "claude_probes[" << tag
                          << "]: 'claude.detail.actions' / 'claude.act."
                             "copyconfig.bg' / 'claude.act.run.bg' not found\n";
                passed = false;
            } else {
                const float keyW = cc->frame.width;
                const float longestText =
                    4.0f * core::theme::current().sizes.fontSizeSmall;
                const float need = longestText + 8.0f;
                if (keyW < need) {
                    std::cerr << "claude_probes[" << tag << "]: 7 键 actW "
                              << keyW << " < 文案宽+呼吸 " << need
                              << "（四字键 @fontSizeSmall）—— 按裁决 ⑥ 需切"
                                 "两行按钮组\n";
                    passed = false;
                } else {
                    std::cout << "claude_probes[" << tag << "]: 7 键 actW="
                              << keyW << " ≥ 文案宽+呼吸 " << need
                              << "（单行达标，裁决 ⑥ 维持）\n";
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
            std::cerr << "claude_probes: 渲染后端不可用，--dump 跳过\n";
            renderBackend.reset();
        }
        const eui::Color clearColor = core::theme::current().colors.background;
        auto renderAndDump = [&](const std::string& path) {
            if (renderBackend == nullptr) return;
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
            ofs << "P6\n" << fbW << " " << fbH << "\n255\n";
            for (int row = fbH - 1; row >= 0; --row) {  // GL 原点左下 → 翻转
                ofs.write(reinterpret_cast<const char*>(
                              px.data() + static_cast<std::size_t>(row) *
                                              static_cast<std::size_t>(fbW) * 3u),
                          static_cast<std::streamsize>(fbW) * 3);
            }
            std::cout << "claude_probes: dump " << path << "\n";
            renderBackend->present();
        };
        // 状态切换后 EUI 过渡动画（弹窗 opacity fade / tabs 指示器位移）需要
        // 若干帧收敛 —— 单帧即拍会抓到半透明中间态（实测 form_adv 半透明）。
        // 40 帧 ≈ 0.67s（pollOnce dt = 1/60），覆盖最长的弹窗 fade
        auto dumpState = [&](const std::string& name) {
            for (int i = 0; i < 40; ++i) {
                composeFrame();
            }
            renderAndDump(dumpPrefix + "_" + name + ".ppm");
        };
        composeFrame();
        // 选中 profile 0（详情 shown 优先用户选中——键来源标注 / JSON 框预填
        // 的落图前提）
        if (auto* card = ui->find("claude.profile.0.bg");
            card != nullptr && card->onClick != nullptr) {
            card->onClick();
            composeFrame();
        }
        // ① 详情 tab 0：7 键操作组（batch-fix C）
        dumpState("actions7");
        // ② 详情 tab 3：高级配置 pane（batch-fix B）
        if (auto* hit = ui->find("claude.detail.tabs.hit.3");
            hit != nullptr && hit->onClick != nullptr) {
            hit->onClick();
            dumpState("detail_adv");
            if (auto* hit0 = ui->find("claude.detail.tabs.hit.0");
                hit0 != nullptr && hit0->onClick != nullptr) {
                hit0->onClick();
            }
        }
        // ③ 表单 tab 3：高级配置页（batch-fix B；编辑 profile 0）
        if (auto* card = ui->find("claude.profile.0.bg");
            card != nullptr && card->onClick != nullptr) {
            card->onClick();
            composeFrame();
            if (auto* edit = ui->find("claude.act.edit.bg");
                edit != nullptr && edit->onClick != nullptr) {
                edit->onClick();
                composeFrame();
                if (auto* tab3 = ui->find("claude.form.tabs.hit.3");
                    tab3 != nullptr && tab3->onClick != nullptr) {
                    tab3->onClick();
                    dumpState("form_adv");
                    if (auto* cancel = ui->find("claude.dialog.form.cancel.bg");
                        cancel != nullptr && cancel->onClick != nullptr) {
                        cancel->onClick();   // 取消，不落任何改动
                    }
                }
            }
        }
        // ⑤ 共享片段编辑弹窗（advanced-json-editor §8.1 JSON 化：name + 高级
        // 设置 JSON 编辑框 + 格式化）
        if (auto* card = ui->find("claude.shared.card.0.bg");
            card != nullptr && card->onClick != nullptr) {
            card->onClick();
            dumpState("shared_dialog");
            if (auto* cancel = ui->find("claude.dialog.shared.cancel.bg");
                cancel != nullptr && cancel->onClick != nullptr) {
                cancel->onClick();   // 取消，不落任何改动
            }
        }
        // ④ 列表右键菜单（五项，batch-fix C）
        if (auto* wrap = ui->find("claude.profile.0.wrap");
            wrap != nullptr && wrap->onContextMenu != nullptr) {
            eui::PointerEvent pe;
            eui::Rect rect;
            pe.x = 600.0;
            pe.y = 300.0;
            wrap->onContextMenu(pe, rect);
            dumpState("ctx_menu");
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