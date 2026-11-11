// md_viewer_window.cpp — F-04 独立只读查看器窗口实现（session 注册表 + 窗口装配 +
// compose）。渲染走 U2 抽出的块流渲染核 mdDrawBlockFlowCore（与编辑器预览单一
// 通路，零双实现）；F-05 搜索消费渲染核「区间→rect」查询缝（pendingRangeQuery /
// onRangeRect 须成对）与 ui.setScrollOffset 程序化滚动通路。
#include "md_viewer_window.h"

#include "markdown_module_internal.h"
#include "md_file_open.h"  // readFileUtf8：UTF-8 路径宽打开（BUG-1 修复缝）
#include "md_block_model.h"
#include "md_viewer_search.h"

#include <components/input.h>
#include <components/markdown.h>
#include <components/scrollview.h>
#include <components/theme.h>
#include <core/component_theme.h>
#include <core/input/input_types.h>
#include <core/theme.h>
#include <eui/dsl_app.h>  // openWindow/requestUpdate 声明（定义在 EUI-NEO 库 glfw_app_main TU，链接期解析；禁含 detail/dsl_app_impl.h——非 inline 定义重复链接撞）

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <system_error>
#include <vector>

namespace module::markdown {
namespace {

// ── ViewerSession：一窗一 session（注册表静态持有，核/compose 只读消费）──
// 生命周期不变量：session 在 onWindowDestroyed（销毁前回调，覆盖全部销毁路径）
// 中从注册表注销 → compose 闭包捕获的裸指针与窗口同寿，无悬垂。
struct ViewerSession {
    std::string path;         // 原文件绝对路径（只读语义：读后不回写）
    std::string title;        // 窗口标题 = 文件名（中段省略号已处理，永不含路径）
    std::string pageId;       // 窗口 pageId（同名双开序号后缀，见 openViewerWindow）
    std::string content;      // 读入全文（只读，进程内不再修改）
    std::string loadError;    // 非空 = 流程 B7 错误占位文案（窗口照开照关）
    void* handle = nullptr;   // onWindowCreated 记句柄（onWindowDestroyed 注销）

    // 渲染状态（MdBlockFlowDeps 所有权在 session；渲染核零成员态只读消费）
    MdBlockModel blockModel;
    MdPreviewCodeCache codeCache;
    MdSyncMap syncMap;
    MdPreviewHoverState hover;
    uint64_t textRev = 0;
    uint64_t contentHash = 0;  // 内容指纹：装载时算一次（内容不可变；contentKey 逐帧引用）

    // 滚动态（F-05 跳转通路，md_module_preview.cpp 先例同款三件）
    float scrollOffset = 0.0f;        // 用户滚动回写（onChange）
    bool programmaticScroll = false;  // 回声抑制标志（:525-527 同款；compose 开头兜底清零）
    float viewportH = 0.0f;           // 最近一帧视口高（跳转「视口垂直居中」算式用）
    size_t pendingJumpBlock = kViewerSearchNoMatch;  // 待跳转块（build 后消费——
        // 一号 POC 判据：setScrollOffset 必须在 scrollView build 之后落地（init-only
        // 守卫 + 仓库时序约束 :464）；onChange 回声经 programmaticScroll 抑制不误标
        // 用户滚动态。三件契约写死在此，改动任一件需重跑 U4 POC。）

    // 搜索态（F-05 状态机本体；appliedQuery = 已参与 matches 计算的词，避免逐帧重算）
    ViewerSearchState search;
    std::string appliedQuery;
    bool releaseSearchInputPending = false;  // 清除后的 input 状态换代释放点
                                             //（liveInputStateFresh_ 同款，防旧词残留）

    // 命中矩形收集：onRangeRect 与 afterHitOverlay 在同一块包裹内先后配对，
    // 同帧同块缓冲（渲染核顺序保证见 mdDrawBlockFlowCore 查询缝注释）
    struct FrameRect {
        MdHitRect rect;
        bool current = false;  // 当前命中 = 普通黄底之上再叠主色绿（设计 §3.4）
    };
    std::vector<FrameRect> curBlockRects;
};

std::vector<std::unique_ptr<ViewerSession>>& viewerSessions() {
    static std::vector<std::unique_ptr<ViewerSession>> sessions;
    return sessions;
}

// 注销（onWindowDestroyed 唯一调用点）：防悬垂的不变量收口
void viewerSessionDestroyed(ViewerSession* s) {
    auto& v = viewerSessions();
    for (auto it = v.begin(); it != v.end(); ++it) {
        if (it->get() == s) {
            v.erase(it);
            return;
        }
    }
}

ViewerSession* findSessionByPath(const std::string& path) {
    for (auto& s : viewerSessions()) {
        if (s->path == path) return s.get();
    }
    return nullptr;
}

// matches 与 query 保持一致（activate 后首帧 / 注入缝后统一走这里；onChange 已
// 同步应用时本函数幂等跳过）
void viewerEnsureMatchesFresh(ViewerSession& s) {
    if (s.search.phase != ViewerSearchPhase::Active) return;
    if (s.appliedQuery == s.search.query) return;
    viewerSearchApplyQuery(s.search, s.search.query, s.blockModel.blocks(),
                           s.content);
    s.appliedQuery = s.search.query;
}

void viewerClearSearch(ViewerSession& s) {
    viewerSearchClear(s.search);           // 回阅读态：matches/cur 清、query 保留
    s.appliedQuery.clear();
    s.releaseSearchInputPending = true;    // 下一帧 compose 释放 input 状态（防旧词残留）
    s.pendingJumpBlock = kViewerSearchNoMatch;
}

void viewerJumpTo(ViewerSession& s, size_t matchIndex) {
    if (matchIndex == kViewerSearchNoMatch) return;  // 0/0 Enter：无操作（PRD 硬边界）
    s.pendingJumpBlock = s.search.matches[matchIndex].block;  // build 后消费（时序契约见成员注释）
}

// ── F-05 键路由（窗口级 onKeyEvent，不进主窗 Router）───────────────────
// 输入框聚焦时 Enter/Esc 由 input.onKeyEvent 消费（见 drawSearchOverlay），
// 到达本 handler 的 Enter/Esc 一定是阅读态按键——两侧互斥不双跳。
void viewerHandleKey(ViewerSession& s, const eui::KeyEvent& key) {
    if (!key.isDown()) return;
    // Ctrl+F / Cmd+F 同一修饰判据（main.cpp:707-715 Ctrl+B 先例：shortcut() 按
    // 平台映射 Ctrl/Super，shift/alt 必须不在场）
    const bool shortcut = key.modifiers.shortcut() && !key.modifiers.shift &&
                          !key.modifiers.alt;
    if (shortcut && key.key == eui::InputKey::F) {
        // 错误/空文件占位窗无搜索面：内容不可变恒零命中，且占位分支不建搜索条，
        // 激活即成不可见僵尸态 → 不进状态机
        if (!s.loadError.empty() || mdViewerContentBlank(s.content)) return;
        viewerSearchActivate(s.search);  // 再唤起保留关键词 + 重聚焦
        app::requestUpdate();
        return;
    }
    if (s.search.phase != ViewerSearchPhase::Active) return;
    if (key.key == eui::InputKey::Enter) {
        // 阅读态（输入框未聚焦）同 C3 口径：Enter 下一处 / Shift+Enter 上一处，
        // 方向判据与输入框 onKeyEvent 一致
        viewerJumpTo(s, key.modifiers.shift ? viewerSearchPrev(s.search)
                                            : viewerSearchNext(s.search));
        app::requestUpdate();
    } else if (key.key == eui::InputKey::Escape) {
        viewerClearSearch(s);
        app::requestUpdate();
    }
}

// ── 搜索浮层（Active 才建；右上浮层不进布局流，唤起/收起零回流，设计 §1.1）──
void drawSearchOverlay(ViewerSession& s, eui::Ui& ui, const eui::Screen& screen,
                       const core::theme::Tokens& t,
                       const components::theme::ThemeColorTokens& componentTheme) {
    // 设计 §3.4 token：浮层 360×40（6 内边距 ×2 + 28 输入框）、top 12 / right 16、
    // 圆角 10 / 边框 borderDefault / 阴影 medium（offsetY 4 blur 12 黑 6%）/
    // 出现动画 150ms ease-out；输入框 220×28 圆角 6 字号 13（settings 卡同参）。
    constexpr float kBarW = 360.0f;
    constexpr float kBarH = 40.0f;
    constexpr float kBarTop = 12.0f;
    constexpr float kBarRight = 16.0f;
    const float kX = std::max(0.0f, screen.width - kBarW - kBarRight);
    ui.stack("md.viewer.search")
        .position(kX, kBarTop)
        .size(kBarW, kBarH)
        .zIndex(20)
        .transition(0.15f, core::Ease::OutCubic)  // 150ms ease-out（属性级
            // opacity/translateY 微差以 transition 档近似，视觉归 qa 截图对照）
        .content([&] {
            ui.rect("md.viewer.search.bg")
                .size(kBarW, kBarH)
                .color(t.colors.surfaceElevated)
                .radius(10.0f)
                .border(1.0f, t.colors.borderDefault)
                .shadow(12.0f, 4.0f,
                        components::theme::withAlpha(
                            eui::Color{0.0f, 0.0f, 0.0f, 1.0f}, 0.06f))
                .build();
            components::input(ui, kViewerSearchInputId)
                .position(6.0f, 6.0f)
                .size(220.0f, 28.0f)
                .value(s.search.query)  // Idle 保留的词经 state 快照回填（再唤起保留）
                .placeholder("查找")
                .multiline(false)
                .fontSize(13.0f)  // 设计 13px 字面值（settings 卡同参，不随字号档）
                .theme(componentTheme)
                .onChange([&s](const std::string& v) {
                    viewerSearchApplyQuery(s.search, v, s.blockModel.blocks(),
                                           s.content);
                    s.appliedQuery = v;
                    app::requestUpdate();
                })
                // 输入框聚焦时的键消费面（窗口 handler 只收「输入框未消费」的键）
                .onKeyEvent([&s](const core::KeyEvent& key) {
                    if (!key.isDown()) return false;
                    if (key.key == eui::InputKey::Enter) {
                        viewerJumpTo(s, key.modifiers.shift
                                            ? viewerSearchPrev(s.search)
                                            : viewerSearchNext(s.search));
                        app::requestUpdate();
                        return true;   // 消费：窗口级 Enter 兜底不再触发（单跳保证）
                    }
                    if (key.key == eui::InputKey::Escape) {
                        viewerClearSearch(s);
                        app::requestUpdate();
                        return true;
                    }
                    return false;
                })
                .build();
            // 计数 n/m（12px textSecondary 右对齐；无命中 = 0/0）
            char buf[16];
            const size_t n =
                s.search.matches.empty() ? 0 : s.search.cur + 1;
            std::snprintf(buf, sizeof buf, "%zu/%zu", n,
                          s.search.matches.size());
            ui.text("md.viewer.search.count")
                .position(234.0f, 6.0f)
                .size(44.0f, 28.0f)
                .text(buf)
                .fontSize(t.sizes.fontSizeSmall)  // 12px
                .lineHeight(28.0f)
                .horizontalAlign(eui::HorizontalAlign::Right)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textSecondary)
                .build();
            if (!s.search.query.empty() && s.search.matches.empty()) {
                ui.text("md.viewer.search.nomatch")
                    .position(282.0f, 6.0f)
                    .size(46.0f, 28.0f)
                    .text("无匹配")
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(28.0f)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textTertiary)
                    .build();
            }
            // 清除 ×（24×24 命中区，悬停底 hover；点击清除回阅读态）
            ui.rect("md.viewer.search.clear")
                .position(330.0f, 8.0f)
                .size(24.0f, 24.0f)
                .radius(6.0f)
                .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                .hoverColor(t.colors.hover)
                .onClick([&s] {
                    viewerClearSearch(s);
                    app::requestUpdate();
                })
                .build();
            ui.text("md.viewer.search.clear.glyph")
                .position(330.0f, 8.0f)
                .size(24.0f, 24.0f)
                .text("✕")
                .fontSize(12.0f)
                .lineHeight(24.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textTertiary)
                .build();
        })
        .build();
    // 聚焦请求须在输入框已进本帧元素树后发（帧末消费时树里无该元素 → 请求丢弃）
    if (s.search.hasFocusPending) {
        ui.requestFocus(kViewerSearchInputId);
        s.search.hasFocusPending = false;
    }
}

// ── compose：外壳固定、单一内容区滚动（设计 §1.1）──────────────────────
void viewerCompose(ViewerSession& s, eui::Ui& ui, const eui::Screen& screen) {
    const core::theme::Tokens t = core::theme::current();
    const auto componentTheme = core::theme::componentTokensWeChat(t);
    // 程序化滚动残留标志兜底（:556-557 同款：漏清残留会把下帧用户滚轮误判成程序化）
    s.programmaticScroll = false;
    if (s.releaseSearchInputPending) {
        // 状态换代释放点须在重建前（id 状态残留会让下次唤起带着已清掉的词）
        ui.releaseStateScope(kViewerSearchInputId);
        s.releaseSearchInputPending = false;
    }
    viewerEnsureMatchesFresh(s);

    // 全幅底色：正文区底 = background token（设计 §3.2）
    ui.rect("md.viewer.bg")
        .position(0.0f, 0.0f)
        .size(screen.width, screen.height)
        .color(t.colors.background)
        .build();

    // 错误占位（流程 B7）/ 空文件占位（§八）：居中一行文字，无弹窗无按钮（设计 §3.3）
    if (!s.loadError.empty() || mdViewerContentBlank(s.content)) {
        ui.text("md.viewer.placeholder")
            .position(0.0f, std::max(0.0f, (screen.height - 24.0f) * 0.5f))
            .size(screen.width, 24.0f)
            .text(s.loadError.empty() ? std::string("（空文件）")
                                      : "无法打开：" + s.loadError)
            .fontSize(t.sizes.fontSizeBody)  // 15px
            .lineHeight(24.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(s.loadError.empty() ? t.colors.textTertiary
                                       : t.colors.errorText)
            .build();
        return;
    }

    // 正文列宽：窗宽−80 自适应（2026-11-11 用户拍板：全屏后正文须随窗宽缩放，
    // 旧 760 定宽在最大化下留大片空白，池 bug#4）；滚动区=该列本身（编辑器
    // 预览同款 pvX/pvW 形态：测量 pass 与渲染同宽，滚动条贴列右缘）
    const float pvW = std::max(0.0f, screen.width - 80.0f);
    const float pvX = std::max(0.0f, (screen.width - pvW) * 0.5f);
    const components::MarkdownStyle style = scaledPreviewStyle(componentTheme);
    // 几何表重建（rebuildSyncMap 同式：测量 = MarkdownBuilder::estimateHeight，
    // 与渲染通路同源；块间隙 = style.blockGap 与渲染核内 blocks 列 gap 同值）
    s.syncMap.rebuild(
        s.content, s.blockModel.blocks(), s.textRev, pvW, style.blockGap,
        [&](const MdBlock& b) {
            return components::MarkdownBuilder::estimateHeight(
                s.content.substr(b.start, b.end - b.start), pvW, style);
        });

    MdBlockFlowDeps deps{s.blockModel.blocks(), s.content, s.codeCache,
                         s.syncMap, s.hover,
                         /*drawCodeBlock=*/{}};  // 空 = CodeFence 走默认 markdown
                                                 // 渲染（查看器无高亮自绘形态）
    components::scrollView(ui, kViewerScrollId)
        .position(pvX, 0.0f)
        .size(pvW, screen.height)
        .gap(style.blockGap)
        .step(56.0f)  // 滚轮步进 56px（与预览 scrollView 同值，设计 §3.2）
        .theme(componentTheme)
        .offset(s.scrollOffset)
        .contentKey("md.viewer.scroll." + s.path + "." +
                    std::to_string(s.contentHash))
        .onChange([&s](float v) {
            if (s.programmaticScroll) {
                // 程序化来源（跳转落地 echo）：只记账，不当作用户滚动态
                s.programmaticScroll = false;
                s.scrollOffset = v;
                return;
            }
            s.scrollOffset = v;  // 用户滚动回写（跨帧保持）
        })
        .content([&](eui::Ui& inner, float contentWidth, float viewportHeight) {
            s.viewportH = viewportHeight;
            s.curBlockRects.clear();
            BlockFlowHooks hooks;
            // F-05 高亮主通路：渲染核内算矩形（每块内容建完后向本回调取待查区间）
            hooks.pendingRangeQuery =
                [&s](size_t k) -> std::vector<MdRangeSpan> {
                std::vector<MdRangeSpan> spans;
                if (s.search.phase != ViewerSearchPhase::Active ||
                    s.search.query.empty()) {
                    return spans;
                }
                for (const ViewerSearchMatch& m : s.search.matches) {
                    if (m.block == k) spans.push_back({m.begin, m.end});
                }
                return spans;
            };
            hooks.onRangeRect = [&s](size_t k, const MdRangeSpan& span,
                                     const MdHitRect& rect) {
                const bool current =
                    !s.search.matches.empty() &&
                    s.search.matches[s.search.cur].block == k &&
                    s.search.matches[s.search.cur].begin == span.begin &&
                    s.search.matches[s.search.cur].end == span.end;
                s.curBlockRects.push_back({rect, current});
            };
            // 绘制通道（复用 afterHitOverlay 缝）：块内矩形叠层，普通 = warning
            // 低透明度、当前 = 之上再叠 primary（两档透明度见设计 §3.4）
            hooks.afterHitOverlay = [&s, &t](eui::Ui& u2, size_t k,
                                             const std::string& hitId) {
                (void)hitId;
                const bool light = core::theme::currentThemeName() != "Dark";
                const float normalAlpha = light ? 0.22f : 0.26f;
                const float curAlpha = light ? 0.30f : 0.38f;
                for (size_t i = 0; i < s.curBlockRects.size(); ++i) {
                    const ViewerSession::FrameRect& fr = s.curBlockRects[i];
                    // id 含块序号：不同块的命中矩形同帧共存，元素 id 不得跨块撞名
                    const std::string base = "md.viewer.hit." + std::to_string(k) +
                                             "." + std::to_string(i);
                    u2.rect(base)
                        .position(fr.rect.x, fr.rect.y)
                        .size(fr.rect.w, fr.rect.h)
                        .color(components::theme::withAlpha(t.colors.warning,
                                                            normalAlpha))
                        .build();
                    if (fr.current) {
                        u2.rect(base + ".cur")
                            .position(fr.rect.x, fr.rect.y)
                            .size(fr.rect.w, fr.rect.h)
                            .color(components::theme::withAlpha(t.colors.primary,
                                                                curAlpha))
                            .build();
                    }
                }
                s.curBlockRects.clear();
            };
            mdDrawBlockFlowCore(inner, contentWidth, t, style, "md.viewer",
                                /*onBlockTap=*/{}, deps, hooks);
        })
        .build();
    // 直写实例 offset（绕开 .offset() init-only 守卫；编辑器 Preview 同款 :221-223，
    // 滚轮已同值回写、程序化跳转在下方以最新值覆写）
    ui.setScrollOffset(kViewerScrollId, s.scrollOffset);
    // 跳转消费——必须在 scrollView build 之后（仓库时序约束，一号 POC 判据；
    // 命中处滚至视口垂直居中，设计 §3.4）
    if (s.pendingJumpBlock != kViewerSearchNoMatch) {
        const size_t k = s.pendingJumpBlock;
        s.pendingJumpBlock = kViewerSearchNoMatch;
        const float topY = s.syncMap.blockTopY(k);
        const float blockH = s.syncMap.blockHeight(k);
        float target = topY - std::max(0.0f, s.viewportH - blockH) * 0.5f;
        // 上限：元素 scrollMaxOffset 优先；取不到（缺失/0）退几何表全高−视口，
        // 跳转目标恒不出内容末端（元素字段滞后期内的兜底）
        float maxOff = 0.0f;
        if (const auto* el = ui.find(kViewerScrollId)) {
            maxOff = el->scrollMaxOffset;
        }
        if (maxOff <= 0.0f) {
            maxOff = std::max(0.0f, s.syncMap.totalHeight() - s.viewportH);
        }
        target = std::clamp(target, 0.0f, maxOff);
        s.scrollOffset = target;
        s.programmaticScroll = true;  // onChange 回声抑制（同帧消费，下帧开头兜底清）
        ui.setScrollOffset(kViewerScrollId, target);
    }

    if (s.search.phase == ViewerSearchPhase::Active) {
        drawSearchOverlay(s, ui, screen, t, componentTheme);
    }
}

} // namespace

void openViewerWindow(const std::string& path) {
    if (path.empty()) return;
    auto session = std::make_unique<ViewerSession>();
    session->path = path;
    session->title = mdViewerWindowTitle(path);
    // pageId 缺省 = title；同名双开加序号后缀（方案卡 §五.2 POC：childWindows 为
    // vector<unique_ptr<ManagedWindow>> 容器无键冲突，后缀只为 pageId 状态面隔离）
    session->pageId = session->title;
    for (int n = 2;; ++n) {
        bool clash = false;
        for (const auto& s : viewerSessions()) {
            if (s->pageId == session->pageId) {
                clash = true;
                break;
            }
        }
        if (!clash) break;
        session->pageId = session->title + " #" + std::to_string(n);
    }

    // 流程 B5/B7：读文件失败仍开窗，窗内错误占位；窗口照开照关。
    // 读取走 readFileUtf8 缝：std::ifstream 的 std::string 重载在 Windows 走窄
    // 字符 ANSI fopen，中文路径必败（BUG-1 根因）；缝内经 fs::path 宽打开。
    auto loaded = readFileUtf8(path);
    if (loaded.isFail()) {
        session->loadError =
            loaded.errorCode() == core::ErrorCode::FileNotFound
                ? "文件不存在或已被移动"
                : "无读取权限或打开失败";
    } else {
        session->content = loaded.value();
    }
    session->contentHash = mdBlockHash(session->content);
    if (session->loadError.empty() && !session->content.empty()) {
        session->blockModel.update(session->content);
        ++session->textRev;
    }

    ViewerSession* raw = session.get();
    viewerSessions().push_back(std::move(session));

    // 窗口规格（D-3 定稿）：标题 = 文件名 / 900×640 / 可缩放 / 系统默认位
    //（不调 windowPosition → positionSet=false）/ 关闭直接关无确认（只读无可保存态）
    app::DslWindowConfig config;
    config.title(raw->title)
        .pageId(raw->pageId)
        .windowSize(900, 640)
        .resizable(true)
        .focusOnShow(true)
        .clearColor(core::theme::current().colors.background)  // 首帧前底色即主题底
        .onWindowCreated([raw](core::window::Handle h) { raw->handle = h; })
        .onWindowDestroyed([raw](core::window::Handle) { viewerSessionDestroyed(raw); })
        .onKeyEvent([raw](const eui::KeyEvent& key) { viewerHandleKey(*raw, key); });
    app::openWindow(config, [raw](eui::Ui& ui, const eui::Screen& screen) {
        viewerCompose(*raw, ui, screen);
    });
    LOG_INFO("查看器开窗: " + path + "（pageId=" + raw->pageId + "）");
}

size_t viewerSessionCountForTest() { return viewerSessions().size(); }

std::string viewerSessionPageIdForTest(size_t index) {
    return index < viewerSessions().size() ? viewerSessions()[index]->pageId
                                           : std::string();
}

void resetViewerSessionsForTest() { viewerSessions().clear(); }

bool viewerSearchInjectForTest(const std::string& path,
                               const std::string& query) {
    ViewerSession* s = findSessionByPath(path);
    if (s == nullptr) return false;
    viewerSearchApplyQuery(s->search, query, s->blockModel.blocks(),
                           s->content);
    s->appliedQuery = query;
    return true;
}

} // namespace module::markdown
