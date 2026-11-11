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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <system_error>
#include <vector>

#include <core/window/window_backend.h>  // setClipboardText：选中即自动复制（need264）

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
    float viewerMeasureWidth = 0.0f;  // 几何表当前测量宽（= 最近一帧渲染 contentWidth）
    // contentHeightProvider 每帧被两种宽度各调一次（全宽判定滚动 + 窄宽定总高），
    // 列宽 16px 量化下窗宽抖动会引入 2 个 pvW 档 × 2 个减滚动条档 = 4 个乒乓宽度；
    // 4 槽（键 = 样式指纹×宽度）→总高缓存让稳态帧（含 resize 乒乓稳态）零重建
    // （旧单路径每帧全量重测，bug259-C 实测单帧 ~1s 的主根因之一）。样式指纹
    // （复核 P2）：字号档/gap/主题度量变化后仅凭宽度命中会回旧总高短路重建 →
    // 底部内容以新形态复发，故键必须含影响排版高度的样式字段。
    struct ProviderSlot {
        uint64_t styleKey = 0;
        float width = -1.0f;
        float total = 0.0f;
    };
    ProviderSlot providerSlots[4];
    int providerSlotNext = 0;
    uint64_t providerStyleKey = 0;  // 每帧由 compose 依当前 style 重算（FNV-1a）

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

    // 性能诊断资产（bug259）：DEVDESK_MDVIEWER_PERF=1 时启用（见 viewerCompose）
    std::vector<long long> perfFrameUs;   // 5s 窗口内逐帧 compose 耗时（P95/max）
    int perfLastBuiltBlocks = 0;          // 最近一帧实际建元块数（视口裁剪生效量）
    // 5s 聚合器本体（need267-3 改会话成员态）：旧 static 跨会话共享把多窗的
    // 帧数/耗时混进同一条 [viewer-perf]（多窗混计、单窗口径失真）。
    int perfWindowFrames = 0;
    long long perfWindowComposeUs = 0;
    std::chrono::steady_clock::time_point perfWindowStart =
        std::chrono::steady_clock::now();

    // 文本选择（need264）：锚点/焦点 = (块下标, 块显示文本码点偏移)。码点域与
    // 搜索 matches 同域（MarkdownBuilder::displayTextOf）；反查走渲染同源
    // MarkdownBuilder::displayCpForPoint。抬起时非空选区写剪贴板（选中即自动
    // 复制，老板 2026-11-11 原话），Ctrl+C 作兼容通道（viewerHandleKey）。
    // selStyle 每帧 compose 回填：指针回调在帧间派发，禁捕 compose 栈上
    // style（bug259 onHover 同纪律——只捕 session 成员指针）。
    struct ViewerSelPos {
        size_t block = 0;
        size_t cp = 0;
        bool operator==(const ViewerSelPos& o) const {
            return block == o.block && cp == o.cp;
        }
    };
    ViewerSelPos selAnchor{};
    ViewerSelPos selFocus{};
    bool selDragging = false;  // 按下→抬起之间（拖选进行中）
    bool selHasText = false;   // 非空选区（拖选中实时高亮 + 抬起后保留）
    components::MarkdownStyle selStyle{};

    // 命中矩形收集：onRangeRect 与 afterHitOverlay 在同一块包裹内先后配对，
    // 同帧同块缓冲（渲染核顺序保证见 mdDrawBlockFlowCore 查询缝注释）
    struct FrameRect {
        MdHitRect rect;
        bool selection = false;  // need264 选区矩形（info 蓝低透明，画在搜索高亮之下）
        bool current = false;    // 当前命中 = 普通黄底之上再叠主色绿（设计 §3.4）
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
// 同步应用时本函数幂等跳过）。搜索域 = 块显示文本（bug259-G），缝注入 EUI 真实现。
const ViewerDisplayTextFn kViewerDisplayTextOf = [](const std::string& btext) {
    return components::MarkdownBuilder::displayTextOf(btext);
};

void viewerEnsureMatchesFresh(ViewerSession& s) {
    if (s.search.phase != ViewerSearchPhase::Active) return;
    if (s.appliedQuery == s.search.query) return;
    viewerSearchApplyQuery(s.search, s.search.query, s.blockModel.blocks(),
                           s.content, kViewerDisplayTextOf);
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

// ── 文本选择（need264）：命中反查 / 文本构建 / 自动复制 ──────────────────

// 块源码切片（s.content 内 [start,end)）。
std::string viewerBlockSource(const ViewerSession& s, size_t k) {
    const auto& blocks = s.blockModel.blocks();
    return s.content.substr(blocks[k].start, blocks[k].end - blocks[k].start);
}

// 显示文本码点 → 字节偏移（切片用）：lead byte 步进与 EUI countCodepoints 同表
// 且钳位到串尾（截断尾序列不越界）——与 md_viewer_search byteToCp 互逆。
size_t viewerSelCpToByte(const std::string& text, size_t cp) {
    size_t i = 0;
    for (size_t done = 0; done < cp && i < text.size(); ++done) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        i += std::min<size_t>(c >= 0xF0U ? 4 : c >= 0xE0U ? 3 : c >= 0xC0U ? 2 : 1,
                              text.size() - i);
    }
    return i;
}

// 指针事件（按压块局部坐标）→ 选区锚点/焦点。跨块拖拽时事件坐标仍在按压块
// 局部系：先经几何表回内容系 y 再定位目标块——屏外（视口裁剪不建树）块照常
// 定位（几何表全量在；inline 布局按需惰性建，与 hitRects 同一缓存通路）。
ViewerSession::ViewerSelPos viewerSelHitTest(const ViewerSession& s, size_t k,
                                             float x, float y) {
    using P = ViewerSession::ViewerSelPos;
    const auto& blocks = s.blockModel.blocks();
    if (blocks.empty()) return P{};
    k = std::min(k, blocks.size() - 1);
    const float contentY = std::max(0.0f, s.syncMap.blockTopY(k) + y);
    size_t target = blocks.size() - 1;
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (contentY < s.syncMap.blockTopY(i) + s.syncMap.blockHeight(i)) {
            target = i;
            break;
        }
    }
    const float localY = std::max(0.0f, contentY - s.syncMap.blockTopY(target));
    const float localX = std::clamp(x, 0.0f, s.viewerMeasureWidth);
    const auto hit = components::MarkdownBuilder::displayCpForPoint(
        viewerBlockSource(s, target), s.viewerMeasureWidth, s.selStyle,
        localX, localY);
    return P{target, hit.cp};
}

// 归一化到阅读序的选区 [b0/cp0, b1/cp1)；空选区（锚=焦）返回 false。
bool viewerSelNormalized(const ViewerSession& s, size_t* b0, size_t* cp0,
                         size_t* b1, size_t* cp1) {
    const ViewerSession::ViewerSelPos& a = s.selAnchor;
    const ViewerSession::ViewerSelPos& f = s.selFocus;
    if (a == f) return false;
    if (a.block < f.block || (a.block == f.block && a.cp < f.cp)) {
        *b0 = a.block; *cp0 = a.cp; *b1 = f.block; *cp1 = f.cp;
    } else {
        *b0 = f.block; *cp0 = f.cp; *b1 = a.block; *cp1 = a.cp;
    }
    return true;
}

// 选中文本（显示文本域，无 markdown 标记符）：块切片按阅读序 '\n' 连接；
// 空显示文本块（如 Divider）与空切片跳过，不产空行（硬验收：CJK/emoji 不得
// 半字——码点域切片天然对齐）。
std::string viewerSelText(const ViewerSession& s) {
    size_t b0 = 0, cp0 = 0, b1 = 0, cp1 = 0;
    if (!viewerSelNormalized(s, &b0, &cp0, &b1, &cp1)) return {};
    const size_t blockCount = s.blockModel.blocks().size();
    std::string out;
    bool first = true;
    for (size_t k = b0; k <= b1 && k < blockCount; ++k) {
        const std::string display =
            components::MarkdownBuilder::displayTextOf(viewerBlockSource(s, k));
        if (display.empty()) continue;
        const size_t byteBegin =
            k == b0 ? viewerSelCpToByte(display, cp0) : 0;
        const size_t byteEnd =
            k == b1 ? viewerSelCpToByte(display, cp1) : display.size();
        if (byteBegin >= byteEnd) continue;
        if (!first) out += '\n';
        out += display.substr(byteBegin, byteEnd - byteBegin);
        first = false;
    }
    return out;
}

// 本块与当前选区的相交区间（显示文本码点域；k 越界返回 false）。
bool viewerSelSpanForBlock(const ViewerSession& s, size_t k, size_t* begin,
                           size_t* end) {
    size_t b0 = 0, cp0 = 0, b1 = 0, cp1 = 0;
    if (!viewerSelNormalized(s, &b0, &cp0, &b1, &cp1)) return false;
    if (k < b0 || k > b1) return false;
    *begin = k == b0 ? cp0 : 0;
    *end = k == b1 ? cp1 : (std::numeric_limits<size_t>::max)();
    return true;
}

// 按下：清旧选区并落锚点（简单点击 = 空 selection，抬起时清零不写剪贴板）。
void viewerSelPress(ViewerSession& s, size_t k, const components::MouseEvent& ev) {
    s.selDragging = true;
    s.selHasText = false;
    s.selAnchor = s.selFocus = viewerSelHitTest(s, k, ev.x, ev.y);
    app::requestUpdate();
}

// 拖动：焦点实时跟随（非空即实时高亮；渲染走 pendingRangeQuery 选区间歇）。
void viewerSelDrag(ViewerSession& s, size_t k, const components::MouseEvent& ev) {
    if (!s.selDragging) return;
    s.selFocus = viewerSelHitTest(s, k, ev.x, ev.y);
    if (!(s.selAnchor == s.selFocus)) s.selHasText = true;
    app::requestUpdate();
}

// 写剪贴板 + 回读校验（need264 复核 P3）：setClipboardText 无返回值（void），
// 写后经 clipboardText 回读比对，失配 LOG_WARN（agent 模块复制先例同源；
// viewer 拖选为高频路径，成功面不打日志降噪，仅失败告警）。
void viewerCopyToClipboard(const ViewerSession& s, const std::string& text) {
    if (text.empty()) return;
    core::window::setClipboardText(text);
    if (s.handle != nullptr &&
        core::window::clipboardText(s.handle) != text) {
        LOG_WARN("md viewer: 剪贴板写入回读校验失败（期望 " +
                 std::to_string(text.size()) + " 字节，选区自动复制可能未生效）");
    }
}

// 抬起：非空选区 = 保留高亮 + 写剪贴板（选中即自动复制）；空选择只清零。
void viewerSelRelease(ViewerSession& s, size_t k, const components::MouseEvent& ev) {
    if (!s.selDragging) return;  // 无在途按压的释放（异常时序）不动作
    s.selDragging = false;
    s.selFocus = viewerSelHitTest(s, k, ev.x, ev.y);
    if (s.selAnchor == s.selFocus) {
        s.selHasText = false;
    } else {
        s.selHasText = true;
        viewerCopyToClipboard(s, viewerSelText(s));
    }
    app::requestUpdate();
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
    // Ctrl/Cmd+C（need264 兼容通道）：有选区时复制——与拖选自动复制同一条
    // viewerSelText 文本构建通路（只读查看器无编辑态，无剪切语义）。
    if (shortcut && key.key == eui::InputKey::C && s.selHasText) {
        viewerCopyToClipboard(s, viewerSelText(s));
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
                                           s.content, kViewerDisplayTextOf);
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
    // 性能诊断资产（bug259 取证口径）：DEVDESK_MDVIEWER_PERF=1 时每 5s 一条
    // [viewer-perf]（compose 分位 / 建元块数 / 高度表热度），默认零开销。
    static const bool perfEnabled = [] {
        const char* v = std::getenv("DEVDESK_MDVIEWER_PERF");
        return v != nullptr && *v != '\0';
    }();
    const auto perfT0 = perfEnabled ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
    // 5s 聚合器 = session 成员（perfWindow*，need267-3）：旧 static 跨会话共享，
    // 多窗并存时帧数/耗时混计，单窗口径失真。env 开关读取保持 static（进程常量）。
    if (perfEnabled) ++s.perfWindowFrames;
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
    // 预览同款 pvX/pvW 形态）。列宽按 16px 步进量化（bug259-C）：拖拽 resize
    // 时相邻帧常落同一档，几何表 memo 与 inline 布局缓存持续命中，不再逐帧
    // 全量重测；列本身居中，量化不改变阅读体验。
    constexpr float kPvWStep = 16.0f;
    const float pvW =
        std::max(0.0f, std::floor(std::max(0.0f, screen.width - 80.0f) / kPvWStep) * kPvWStep);
    const float pvX = std::max(0.0f, (screen.width - pvW) * 0.5f);
    const components::MarkdownStyle style = scaledPreviewStyle(componentTheme);
    s.selStyle = style;  // 指针回调帧间消费的样式快照（禁捕栈上引用，见成员注释）
    // 样式指纹（复核 P2）：provider 槽键的样式半边。取所有影响排版高度的样式
    // 字段做 FNV-1a；字号档/gap/主题度量一动指纹即翻新，槽自然失配。
    {
        uint64_t h = 1469598103934665603ULL;
        auto mixF = [&h](float v) {
            static_assert(sizeof(float) == sizeof(uint32_t));
            uint32_t b = 0;
            std::memcpy(&b, &v, sizeof(b));
            h ^= b;
            h *= 1099511628211ULL;
        };
        mixF(style.bodySize);
        mixF(style.bodyLineHeight);
        mixF(style.h1Size);
        mixF(style.h2Size);
        mixF(style.h3Size);
        mixF(style.codeSize);
        mixF(style.blockGap);
        mixF(style.listIndent);
        mixF(style.codePadding);
        mixF(style.quotePadding);
        mixF(style.tableCellPadding);
        s.providerStyleKey = h;
    }
    // 几何表测量函数（= MarkdownBuilder::estimateHeight，与渲染通路同源；块间隙
    // = style.blockGap 与渲染核内 blocks 列 gap 同值）。重建时机移到 scrollview
    // contentHeightProvider / content 回调内、按【实际渲染宽 contentWidth】执行
    // （bug259-D/F 根因修复：旧实现在 pvW 上测量、在 contentWidth=pvW−滚动条占位
    // 上渲染，折行差一列 ⇒ 块包裹高偏短 ⇒ 相邻块互相侵入 + 底部内容到不了）。
    const MdSyncMap::HeightFn heightFn = [&s, &style](const MdBlock& b) {
        return components::MarkdownBuilder::estimateHeight(
            s.content.substr(b.start, b.end - b.start),
            s.viewerMeasureWidth, style);
    };
    const auto rebuildSyncMapAt = [&](float contentWidth) {
        s.viewerMeasureWidth = contentWidth;
        s.syncMap.rebuild(s.content, s.blockModel.blocks(), s.textRev,
                          contentWidth, style.blockGap, heightFn);
    };

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
        // 滚动总高供给缝（bug259-C）：以几何表 totalHeight 替代 scrollview 的
        // 整篇 scratch 测量 pass（旧路径每帧把全部块在 scratch Ui 上建一遍 +
        // layout，104 块实测单帧 ~1s）。与渲染同源 ⇒ 总高即真实排版高。
        // 4 槽缓存键 = (样式指纹, contentWidth)：同键调用直接回缓存值，稳态帧
        // 零重建；字号档/gap/主题度量变化使指纹翻新 → 旧槽自然失配不回旧总高
        // （复核 P2）。
        .contentHeightProvider([&](float contentWidth, float /*viewportHeight*/) {
            const uint64_t key = s.providerStyleKey;
            for (int i = 0; i < 4; ++i) {
                if (s.providerSlots[i].styleKey == key &&
                    s.providerSlots[i].width == contentWidth) {
                    return s.providerSlots[i].total;
                }
            }
            rebuildSyncMapAt(contentWidth);
            const float total = s.syncMap.totalHeight();
            const int slot = s.providerSlotNext;
            s.providerSlots[slot] = {key, contentWidth, total};
            s.providerSlotNext = (slot + 1) % 4;
            return total;
        })
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
            // 幂等：provider 已按同宽重建则零重算；宽度在此处才最终确定
            rebuildSyncMapAt(contentWidth);
            s.curBlockRects.clear();
            BlockFlowHooks hooks;
            // 视口裁剪（bug259-C）：只建可视区附近的块，余量上 1 屏 / 下 2 屏
            //（cullTop = offset−vh、cullBottom = offset+2vh；下偏重——向下滚动
            // 是主阅读方向，多留防快滚露白）。scrollOffset 用本帧回写值（滚轮
            // 事件先于 compose 落账）。
            const float cullVh = std::max(viewportHeight, 480.0f);
            const float cullTop = s.scrollOffset - cullVh;
            const float cullBottom = s.scrollOffset + cullVh * 2.0f;
            int builtBlocks = 0;  // bug259 性能诊断：本帧实际建元块数（视口裁剪生效量）
            hooks.blockVisible = [&s, cullTop, cullBottom, &builtBlocks](size_t k) {
                const float top = s.syncMap.blockTopY(k);
                const bool vis =
                    top + s.syncMap.blockHeight(k) >= cullTop &&
                    top <= cullBottom;
                if (vis) ++builtBlocks;
                return vis;
            };
            // F-05 高亮主通路：渲染核内算矩形（每块内容建完后向本回调取待查区间）
            hooks.pendingRangeQuery =
                [&s](size_t k) -> std::vector<MdRangeSpan> {
                std::vector<MdRangeSpan> spans;
                if (s.search.phase == ViewerSearchPhase::Active &&
                    !s.search.query.empty()) {
                    for (const ViewerSearchMatch& m : s.search.matches) {
                        if (m.block == k) spans.push_back({m.begin, m.end});
                    }
                }
                // need264 选区间歇：与本块显示文本相交的选区（selection 标记
                // → afterHitOverlay 归 info 蓝档，画在搜索高亮之下）
                size_t selBegin = 0, selEnd = 0;
                if (viewerSelSpanForBlock(s, k, &selBegin, &selEnd)) {
                    spans.push_back({selBegin, selEnd, /*selection=*/true});
                }
                return spans;
            };
            hooks.onRangeRect = [&s](size_t k, const MdRangeSpan& span,
                                     const MdHitRect& rect) {
                const bool current =
                    !span.selection &&
                    !s.search.matches.empty() &&
                    s.search.matches[s.search.cur].block == k &&
                    s.search.matches[s.search.cur].begin == span.begin &&
                    s.search.matches[s.search.cur].end == span.end;
                s.curBlockRects.push_back({rect, span.selection, current});
            };
            // 绘制通道（复用 afterHitOverlay 缝）：选区矩形（info 蓝低透明，与
            // 搜索琥珀/绿异色防混淆——need264）先画，其上搜索命中普通 = warning
            // 低透明度、当前 = 之上再叠 primary（两档透明度见设计 §3.4）
            hooks.afterHitOverlay = [&s, &t](eui::Ui& u2, size_t k,
                                             const std::string& hitId) {
                (void)hitId;
                const bool light = core::theme::currentThemeName() != "Dark";
                const float selAlpha = light ? 0.16f : 0.22f;
                const float normalAlpha = light ? 0.22f : 0.26f;
                const float curAlpha = light ? 0.30f : 0.38f;
                size_t selIdx = 0;
                size_t hitIdx = 0;
                for (size_t i = 0; i < s.curBlockRects.size(); ++i) {
                    const ViewerSession::FrameRect& fr = s.curBlockRects[i];
                    // id 含块序号：不同块的命中矩形同帧共存，元素 id 不得跨块撞名
                    if (fr.selection) {
                        u2.rect("md.viewer.sel." + std::to_string(k) + "." +
                                std::to_string(selIdx++))
                            .position(fr.rect.x, fr.rect.y)
                            .size(fr.rect.w, fr.rect.h)
                            .color(components::theme::withAlpha(t.colors.info,
                                                                selAlpha))
                            .build();
                        continue;
                    }
                    const std::string base = "md.viewer.hit." + std::to_string(k) +
                                             "." + std::to_string(hitIdx++);
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
            // need264 块指针缝：拖选三件（press 落锚 / drag 焦点跟随 / release
            // 自动复制）。捕获 session 指针（session 与窗口同寿，见注册表注释）。
            hooks.onBlockPress = [p = &s](size_t blk,
                                          const components::MouseEvent& ev) {
                viewerSelPress(*p, blk, ev);
            };
            hooks.onBlockDrag = [p = &s](size_t blk,
                                         const components::MouseEvent& ev) {
                viewerSelDrag(*p, blk, ev);
            };
            hooks.onBlockRelease = [p = &s](size_t blk,
                                            const components::MouseEvent& ev) {
                viewerSelRelease(*p, blk, ev);
            };
            mdDrawBlockFlowCore(inner, contentWidth, t, style, "md.viewer",
                                /*onBlockTap=*/{}, deps, hooks);
            s.perfLastBuiltBlocks = builtBlocks;  // 性能诊断（DEVDESK_MDVIEWER_PERF）
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

    if (perfEnabled) {
        const long long frameUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - perfT0)
                .count();
        s.perfWindowComposeUs += frameUs;
        s.perfFrameUs.push_back(frameUs);
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - s.perfWindowStart);
        if (elapsed.count() >= 5.0) {
            auto& us = s.perfFrameUs;
            long long maxUs = 0;
            long long p95Us = 0;
            if (!us.empty()) {
                const size_t idx95 =
                    std::min(us.size() - 1, (us.size() * 95 + 99) / 100 - 1);
                std::vector<long long> sorted(us);
                std::nth_element(sorted.begin(), sorted.begin() + idx95,
                                 sorted.end());
                p95Us = sorted[idx95];
                maxUs = *std::max_element(us.begin(), us.end());
            }
            LOG_INFO("[viewer-perf] frames=" +
                     std::to_string(s.perfWindowFrames) +
                     " compose_avg_us=" +
                     std::to_string(s.perfWindowComposeUs /
                                    (s.perfWindowFrames ? s.perfWindowFrames : 1)) +
                     " compose_p95_us=" + std::to_string(p95Us) +
                     " compose_max_us=" + std::to_string(maxUs) +
                     " elements=" + std::to_string(ui.elementCount()) +
                     " built_blocks=" + std::to_string(s.perfLastBuiltBlocks) +
                     "/" + std::to_string(s.syncMap.blockCount()) +
                     " rebuild_total=" + std::to_string(s.syncMap.rebuildCount()) +
                     " height_computes=" + std::to_string(s.syncMap.heightComputes()) +
                     " blocks=" + std::to_string(s.syncMap.blockCount()) +
                     " est_total_h=" +
                     std::to_string(s.syncMap.totalHeight()) +
                     " win_w=" + std::to_string(screen.width) +
                     " win_h=" + std::to_string(screen.height) +
                     " code_cache=" + std::to_string(s.codeCache.size()) +
                     " code_hits=" + std::to_string(s.codeCache.hits()) +
                     " code_misses=" + std::to_string(s.codeCache.misses()) +
                     " window_s=" + std::to_string(elapsed.count()));
            s.perfWindowFrames = 0;
            s.perfWindowComposeUs = 0;
            s.perfFrameUs.clear();
            s.perfWindowStart = std::chrono::steady_clock::now();
        }
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
                           s->content, kViewerDisplayTextOf);
    s->appliedQuery = query;
    return true;
}

bool viewerSelectionInjectForTest(const std::string& path, size_t blockA,
                                  size_t cpA, size_t blockB, size_t cpB) {
    ViewerSession* s = findSessionByPath(path);
    if (s == nullptr) return false;
    const size_t n = s->blockModel.blocks().size();
    if (blockA >= n || blockB >= n) return false;
    s->selAnchor = {blockA, cpA};
    s->selFocus = {blockB, cpB};
    s->selDragging = false;
    s->selHasText = !(s->selAnchor == s->selFocus);
    return true;
}

std::string viewerSelectionTextForTest(const std::string& path) {
    ViewerSession* s = findSessionByPath(path);
    if (s == nullptr) return {};
    return viewerSelText(*s);
}

} // namespace module::markdown
