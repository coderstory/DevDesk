// claude_probe.cpp — Claude 切换器页截图探针（W0.1，UI 整改改前基线存档）
//
// 运行：claude_probe.exe [--screenshot <路径>] [--size WxH]
// 不带 --screenshot = 纯 compose 自检（零开销，不写文件）。
// 用途：UI 整改 W0.2 改前基线截图存档——改前/改后对照锚点。仿 home_probes 骨架
// （init→TestDb→create_module→onStart→compose→captureIfRequested），不带几何审计组。

#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>
#include "test_support.h"
#include "core/module_interface.h"
#include "core/component_theme.h"
#include "core/theme.h"
#include "module_claude_proxy/claude_ui_detail.h"
#include "module_claude_proxy/claude_ui_form.h"
#include "module_claude_proxy/claude_ui_list.h"
#include "module_claude_proxy/claude_ui_state.h"
#include "module_claude_proxy/profile_repository.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

// app:: 层桩（probe 不建应用级 UI，compose 走 probe::composeModuleFrame）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_claude_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

extern "C" ::core::Module* create_claude_module();

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    std::string shotProfiles;
    std::string shotForm;
    std::string shotDetail;  // bug175/176 详情面板「环境变量」页签截图路径
    std::string formState;   // fail = 预检失败态文案
    bool lightTheme = false;
    float fontScale = 1.0f;  // need236：字号档（0.80~1.30）
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--shot-profiles" && i + 1 < argc) {
            shotProfiles = argv[++i];
        } else if (arg == "--shot-form" && i + 1 < argc) {
            shotForm = argv[++i];
        } else if (arg == "--shot-detail" && i + 1 < argc) {
            shotDetail = argv[++i];
        } else if (arg == "--form-state" && i + 1 < argc) {
            formState = argv[++i];
        } else if (arg == "--light") {
            lightTheme = true;
        } else if (arg == "--font-scale" && i + 1 < argc) {
            // need236：字号档（0.80~1.30，缺省 1.0）。徽标下界钳制需在放大档
            // 验证——100% 档下 chip 顶恰好贴名字行底，容差内区分不出回归。
            fontScale = std::stof(argv[++i]);
        }
    }
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("claude_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    // 空迁移库（改前基线 = 空 profile 列表状态，合理）
    test_support::TestDb tdb("cdprobe_", "probe");
    test_support::ScopedAppDatabase scopedDb(tdb.handle());

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    // need236：字号档在 compose 前生效（settings 模块 onStart 会按配置文件回写档位，
    // 故本探针在它之后覆盖，与本模块 UI 几何无关——只影响全局 scaled()）
    if (fontScale != 1.0f) {
        core::theme::setFontScale(fontScale);
    }

    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    bool passed = true;

    core::dsl::Ui* ui = nullptr;
    auto composeFrame = [&]() {
        probe::composeModuleFrame(
            runtime, window, "claude", w, h,
            [&](eui::Ui& u, const eui::Screen& screen) {
                ui = &u;
                mod->compose(u, screen);
            });
    };

    // 稳帧：首帧建树，第二帧让布局/transition 收敛
    composeFrame();
    composeFrame();

    // 自检：compose 跑过了（ui 非空 = 树建出来了，不是空树→黑图）
    if (ui == nullptr) {
        std::cerr << "claude_probe: FAIL——compose 未建出 UI 树"
                     "（create_claude_module/onStart/compose 链路断？）\n";
        passed = false;
    }

    // —— tooltip 锚定取证（bug177）：问号钮 hover 后气泡须钉页头下方 ±60px ——
    // 气泡 320 宽、指针尖在锚点正下（气泡内 x≈206 右偏），中心与按钮中心天然差
    // ~46px；阈值 60 容忍之、仍能抓住「钉死容器左上」的跑位（修前偏差 ~800px）。
    if (ui != nullptr) {
        if (const core::dsl::Element* hoverBtn =
                ui->find("claude.title.help.bg")) {
            const float cx = hoverBtn->frame.x + hoverBtn->frame.width * 0.5f;
            const float cy = hoverBtn->frame.y + hoverBtn->frame.height * 0.5f;
            core::queuePointerButton(window, cx, cy,
                                     core::PointerButton::Left,
                                     core::PointerAction::Press, {});
            core::queuePointerButton(window, cx, cy,
                                     core::PointerButton::Left,
                                     core::PointerAction::Release, {});
            composeFrame();
            composeFrame();
            std::cout << "== tooltip 锚定 ==\n";
            auto dumpTip = [&](const char* id) {
                if (const core::dsl::Element* e = ui->find(id)) {
                    std::cout << "  frame " << id << " = (" << e->frame.x
                              << "," << e->frame.y << ") " << e->frame.width
                              << "x" << e->frame.height << "\n";
                } else {
                    std::cout << "  frame " << id << " = <缺失>\n";
                }
            };
            dumpTip("claude.title.help");
            dumpTip("claude.title.help.tooltip");
            const core::dsl::Element* tip =
                ui->find("claude.title.help.tooltip");
            const core::dsl::Element* btn = ui->find("claude.title.help");
            if (tip != nullptr && btn != nullptr) {
                const float btnCx = btn->frame.x + btn->frame.width * 0.5f;
                const float tipCx = tip->frame.x + tip->frame.width * 0.5f;
                std::cout << "  中心偏差 = " << std::fabs(tipCx - btnCx)
                          << "\n";
                if (tip->frame.y < btn->frame.y + btn->frame.height - 2.0f ||
                    std::fabs(tipCx - btnCx) > 60.0f) {
                    std::cerr << "bug177: 气泡未锚定在问号钮下方 ±60px（跑到"
                                 "别处）\n";
                    passed = false;
                }
            } else {
                std::cerr << "bug177: claude.title.help(.tooltip) 缺帧\n";
                passed = false;
            }
        }
    }

    // --screenshot 落图（未传 = 零开销不写文件；传了失败→passed=false）
    probe::captureIfRequested(args, window, runtime, &passed);

    // --shot-profiles：profile 列表 item 的样式取证。
    // 模块 compose 走服务端，探针环境服务端未就绪 → 列表恒空，item 样式无从取样；
    // 这里注入假数据直调 drawProfileListPanel（同一个绘制入口）。
    if (!shotProfiles.empty()) {
        std::vector<module::claude::Profile> profiles;
        profiles.push_back({1, "生产环境", "https://api.anthropic.com"});
        // emoji 开头的名字：旧版头像首字取「第一个 UTF-8 码点」→ 字体无该字形
        // → ensureGlyph 失败 → 整段不渲染 → 头像圆白着（用户反馈「文字丢了」）
        profiles.push_back({5, "\xF0\x9F\x9A\x80 \xE7\x94\x9F\xE4\xBA\xA7", "https://api.anthropic.com"});
        profiles.push_back({2, "Personal", "https://api.anthropic.com"});
        profiles.push_back({3, "测试环境 · 备用节点", "https://api.anthropic.com"});
        profiles.push_back({4, "a", "https://api.anthropic.com"});
        // bug174 长名对照项：激活项挂长名，对照「使用中」标记挤占名字宽度的截断差异
        profiles.push_back({9, "生产环境 · 华东主节点 · 双活集群",
                            "https://api.anthropic.com"});
        module::claude::ProfileListState st;
        st.cachedHasActive_ = true;
        st.cachedActive_ = profiles.size() > 1 ? profiles.back()
                                               : module::claude::Profile();
        module::claude::ProfileListActions actions;
        if (lightTheme) {
            core::theme::setLight();
            std::cerr << "claude_probe: lightTheme on, isDark="
                      << (core::theme::current().isDark ? 1 : 0) << "\n";
        }
        const auto& t = core::theme::current();
        const auto themeTokens = core::theme::componentTokensWeChat(t);
        for (int f = 0; f < 4; ++f) {
            probe::composeModuleFrame(
                runtime, window, "claude", w, h,
                [&](eui::Ui& u, const eui::Screen& screen) {
                    module::claude::drawProfileListPanel(
                        u, 320.0f, screen.height, t, themeTokens, st, profiles,
                        std::string(), actions);
                });
        }
        if (!probe::captureFramePng(window, runtime, shotProfiles)) {
            std::cerr << "claude_probe: profile 列表落图失败 " << shotProfiles << "\n";
            passed = false;
        }

        // need236 布局守卫：「使用中」悬浮徽标不得遮住 profile 名字。
        // 背景：need174 把徽标改悬浮后曾与名字行 100% 重叠，长名尾字被 0.78α
        // 绿底压住；need236 把徽标下移到副标题行带并加放大档下界钳制。
        // 判据 = 徽标顶 ≥ 名字行底（垂直不相交即不遮挡）且徽标底 ≤ 卡底。
        // ⚠️ 必须在**真实布局**（composeModuleFrame 已跑完整帧）后取 frame：
        // headless 纯 compose 下 frame 恒 0，断言会变恒真（本条目踩过）。
        if (ui != nullptr) {
            const std::string cardId =
                "claude.profile." + std::to_string(profiles.size() - 1);
            const auto* chip = ui->find(cardId + ".active.chip");
            const auto* name = ui->find(cardId + ".name");
            const auto* wrap = ui->find(cardId + ".wrap");
            if (chip == nullptr || name == nullptr || wrap == nullptr) {
                std::cerr << "claude_probe: need236 FAIL 元素缺失 chip="
                          << (chip != nullptr) << " name=" << (name != nullptr)
                          << " wrap=" << (wrap != nullptr) << "\n";
                passed = false;
            } else {
                const float nameBottom = name->frame.y + name->frame.height;
                const float chipTop = chip->frame.y;
                const float chipBottom = chip->frame.y + chip->frame.height;
                const float wrapBottom = wrap->frame.y + wrap->frame.height;
                const bool noOverlap = chipTop + 0.5f >= nameBottom;
                const bool inCard = chipBottom <= wrapBottom + 0.5f;
                if (!noOverlap || !inCard) {
                    std::cerr << "claude_probe: need236 FAIL 徽标遮挡名字 "
                              << "chip.y=" << chipTop << " chip底=" << chipBottom
                              << " name底=" << nameBottom
                              << " 卡底=" << wrapBottom
                              << "（不重叠=" << noOverlap
                              << " 卡内=" << inCard << "）\n";
                    passed = false;
                } else {
                    std::cout << "claude_probe: need236 PASS 徽标不遮名字（"
                              << cardId << " chip.y=" << chipTop
                              << " name底=" << nameBottom << "）\n";
                }
            }
        }
    }

    // --shot-form：导入卡（新建 Profile 表单弹窗）取证（need185 before/after）。
    // 模块内 state_.form 不可直触，构造同构 FormState 直调 drawProfileFormDialog
    // （同一绘制入口）；--form-state fail 驱动预检失败文案态。
    if (!shotForm.empty()) {
        const auto& t = core::theme::current();
        const auto themeTokens = core::theme::componentTokensWeChat(t);
        module::claude::FormState f;
        module::claude::openCreateForm(f);
        f.formName_ = "生产环境";
        f.formUrl_ = "https://api.anthropic.com";
        f.formKey_ = "sk-ant-test-0001";
        if (formState == "fail") {
            f.formError_ =
                "预检未通过：凭证无效——上游返回 HTTP 401（API Key 被拒绝）";
        } else if (formState == "precheck") {
            f.formPrechecking_ = true;   // 预检进行中：状态槽进度文案 + 确认钮转「预检中…」
        }
        for (int fr = 0; fr < 3; ++fr) {
            probe::composeModuleFrame(
                runtime, window, "claude", w, h,
                [&](eui::Ui& u, const eui::Screen& screen) {
                    module::claude::FormSubmitHooks hooks;  // 截图态不点击，空缝
                    module::claude::drawProfileFormDialog(u, screen, t,
                                                          themeTokens, f, hooks);
                });
        }
        if (!probe::captureFramePng(window, runtime, shotForm)) {
            std::cerr << "claude_probe: 表单弹窗落图失败 " << shotForm << "\n";
            passed = false;
        }
    }

    // --shot-detail：详情面板「环境变量」页签取证（bug175/176 before/after）。
    // 模块 compose 走服务端，探针环境详情无数据 → 构造同构 DetailView 直调
    // drawDetailPanel（同一绘制入口），detailTab_=2 落在环境变量页。
    if (!shotDetail.empty()) {
        const auto& t = core::theme::current();
        const auto themeTokens = core::theme::componentTokensWeChat(t);
        module::claude::Profile p;
        p.id = 7;
        p.name = "生产环境";
        p.baseUrl = "https://api.anthropic.com";
        p.apiKeyHint = "sk-ant-***0001";
        p.extraEnvJson =
            "{\"ANTHROPIC_AUTH_TOKEN\":\"sk-ant-test-0001-abcdef123456\","
            "\"ANTHROPIC_SMALL_FAST_MODEL\":\"claude-3-5-haiku-20241022\","
            "\"HTTP_PROXY\":\"http://127.0.0.1:7890\","
            "\"API_TIMEOUT_MS\":\"600000\"}";
        module::claude::DetailState st;
        st.detailTab_ = 2;  // 环境变量
        module::claude::DetailView v;
        v.shown = &p;
        v.nameValue = p.name;
        v.urlValue = p.baseUrl;
        v.tokenValue = p.apiKeyHint;
        module::claude::DetailActions a;  // 截图态不点击，空缝
        for (int f = 0; f < 4; ++f) {
            probe::composeModuleFrame(
                runtime, window, "claude", w, h,
                [&](eui::Ui& u, const eui::Screen& screen) {
                    module::claude::drawDetailPanel(u, w - 40.0f, screen.height,
                                                    20.0f, t, themeTokens, st,
                                                    v, a);
                });
        }
        if (!probe::captureFramePng(window, runtime, shotDetail)) {
            std::cerr << "claude_probe: 详情面板落图失败 " << shotDetail << "\n";
            passed = false;
        }
    }

    mod->onStop();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();

    if (!passed) {
        std::cerr << "FAIL\n";
        return 1;
    }
    std::cout << "claude_probe: PASS（" << args.windowW << "x"
              << args.windowH << "）\n";
    return 0;
}
