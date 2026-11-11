// src/module_terminal/terminal_page.cpp
#include "terminal_page.h"

#include <algorithm>
#include <ratio>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unordered_map> // bug248：textWidth 缓存
#include <utility>
#include <vector>
#include "core/logging.h"
#include "core/router.h"  // bug245：指针窗口坐标→页面坐标经 Router 页面原点换算
#include "core/platform/ime_bridge.h" // eui_ime_trace_log（ImeTrace 段1 与 EUI/win 段2/3 同落 ime-trace.log）
#include "core/render/text.h"
#include "core/component_theme.h"
#include "core/theme.h"
#include "platform/folder_picker.h"
#include "terminal_keyencoder.h"
#include <components/dropdown.h>
#include <components/input.h>
#include <components/scrollview.h>
#include <components/segmented.h>
#include <components/stepper.h>
#include <components/switch.h>
#include "ui_kit/card.h"
#include "ui_kit/glass.h"

namespace terminal {

using core::Color;

namespace {

// need250 面板几何（设计 §4.1）
constexpr float kPanelW = 520.0f;
constexpr float kPanelPad = 16.0f;
constexpr float kLabelW = 128.0f;
constexpr float kLabelGap = 12.0f;
constexpr float kRowBlockH = 44.0f;          // 横排行高
constexpr float kRowBlockHNarrow = 68.0f;    // 堆叠行高（label 行 20 + 4 + 控件行 44）
constexpr float kControlH = 32.0f;
constexpr float kGroupHeadH = 20.0f;
constexpr float kHintH = 20.0f;              // 行下微字/警示行高
constexpr uint32_t kCursorBlinkMs = 1100;    // 光标闪烁周期（阶跃，§4.2⑦）

// 字体/shell 选项表（值表来自平台文件；标签首项 = 默认语义）。静态单例：dropdown
// onChange 闭包跨帧存留，指针/引用捕获必须指稳对象。
struct OptionTable {
    std::vector<std::string> values;
    std::vector<std::string> labels;
};

OptionTable buildFontTable() {
    OptionTable t;
    t.values = terminalFontOptionValues();
    for (const std::string& v : t.values)
        t.labels.push_back(v.empty() ? "默认（内置等宽）" : v);
    return t;
}

OptionTable buildShellTable() {
    OptionTable t;
    t.values = terminalShellOptionValues();
    const std::string def = "默认（" + terminalDefaultShellName() + "）";
    for (const std::string& v : t.values)
        t.labels.push_back(v.empty() ? def : v);
    return t;
}

const OptionTable& fontOptionTable() {
    static const OptionTable t = buildFontTable();
    return t;
}

const OptionTable& shellOptionTable() {
    static const OptionTable t = buildShellTable();
    return t;
}

int indexOfValue(const OptionTable& t, const std::string& v) {
    for (size_t i = 0; i < t.values.size(); i++) {
        if (t.values[i] == v) return static_cast<int>(i);
    }
    return 0; // 枚举外回默认（首项 = 平台默认语义）
}

constexpr float kToolbarH = 44.0f;   // TAB 栏带高（设计 §2.1，全屏保留）
constexpr float kTabH = 32.0f;       // TAB 高（栏内垂直居中，上下各 6）
constexpr float kTabY = 6.0f;
constexpr float kTabGap = 4.0f;
constexpr float kTabMaxW = 200.0f;
constexpr float kTabMinW = 120.0f;       // 设计 §8：页面区 720–1199 档 TAB min 120
constexpr float kTabMinWNarrow = 100.0f; // 设计 §8：<720 窄窗档 TAB min 100（复审 P3 两档）
constexpr float kTabTitleMaxW = 120.0f;
constexpr float kActionGap = 8.0f;
constexpr float kRightPad = 12.0f;
constexpr uint32_t kFaXmark = 0xF00D;
// 设置钮用 sliders（0xF1DE）不用 gear（0xF013）——FA gear 在 16px 下形似「光芒小圆」
// 与亮暗切换图标混淆（老板 2026-11-05 实测，fontTools 实证字体含 sliders 字形）
constexpr uint32_t kFaSliders = 0xF1DE;
constexpr uint32_t kFaExpand = 0xF065;
constexpr uint32_t kFaCompress = 0xF066;
constexpr uint32_t kFaPlus = 0xF067;
constexpr uint32_t kFaCircleExclamation = 0xF06A;
constexpr uint32_t kFaCircleCheck = 0xF058;
constexpr uint32_t kFaTerminal = 0xF120;
const Color kTransparent{0.0f, 0.0f, 0.0f, 0.0f};

// ---- TermPerf 诊断（bug248 临时，UI 侧；删诊断时整块移除）：只累加计数，5s 一条
struct TermPerfUi {
    std::chrono::steady_clock::time_point lastLog{};
    std::chrono::steady_clock::time_point lastCompose{};
    bool hasLog = false;
    bool hasCompose = false;
    double pollMs = 0.0;
    double pollMaxMs = 0.0;
    double composeMs = 0.0;
    double composeMaxMs = 0.0;
    double gapMs = 0.0;
    double gapMaxMs = 0.0;
    int frames = 0;
};

TermPerfUi& termPerfUi() {
    static TermPerfUi s;
    return s;
}

// bug248：measureTextWidth（EUI text_measure）无缓存、每帧对同批字符串完整 shaping
// ——TAB 标题/固定文案（「新建」「终端 N」「恢复默认」等）逐帧重复测量。键空间小且
// 有界（TAB 数有限），超 64 键整体清空防膨胀。
// bug267-P3b：缓存键补字体族口径——现调用方都走默认族（空串），防御性入键，
// 防未来调用方传具名族时复用错族宽度（TAB 几何按错字体规划）。
float textWidth(const std::string& s, float size, const std::string& family = {}) {
    struct Key {
        float size;
        std::string family;
        std::string s;
        bool operator==(const Key& o) const {
            return size == o.size && family == o.family && s == o.s;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            return (std::hash<std::string>{}(k.s) ^
                    (std::hash<std::string>{}(k.family) * 0x9e3779b9ULL)) ^
                   (std::hash<float>{}(k.size) * 0xc2b2ae3dULL);
        }
    };
    static std::unordered_map<Key, float, KeyHash> cache;
    if (cache.size() >= 64) cache.clear();
    Key key{size, family, s};
    const auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    const float w = core::TextPrimitive::measureTextWidth(s, family, size, 400);
    cache.emplace(std::move(key), w);
    return w;
}

} // namespace

GridLayout TerminalPage::compose(
    eui::Ui& ui, const eui::Screen& screen, SessionController& controller,
    const std::function<void(const uint8_t*, int32_t)>& onKeyBytes,
    const std::function<void(const std::string&, bool paste)>& onTextInput,
    const std::function<void(const std::string&)>& onCopyText,
    TerminalSettings& settings, const std::function<void()>& onSettingsChanged) {
    // TermPerf（bug248）：compose 入口时间戳 + 与上次 compose 的帧间隔
    const auto composeT0 = std::chrono::steady_clock::now();
    TermPerfUi& pf = termPerfUi();
    if (pf.hasCompose) {
        const double gap =
            std::chrono::duration<double, std::milli>(composeT0 - pf.lastCompose).count();
        pf.gapMs += gap;
        pf.gapMaxMs = std::max(pf.gapMaxMs, gap);
    }
    pf.lastCompose = composeT0;
    pf.hasCompose = true;

    const auto& t = core::theme::current();
    GridLayout layout;
    layout.rows = 0;
    layout.cols = 0;

    if (!fullscreenInit_) {
        fullscreenInit_ = true;
        fullscreen_ = std::getenv("DEVDESK_TERMINAL_FULLSCREEN") != nullptr;
    }

    // need250：字体/字号来自设置面板（即时生效——度量链重算 → 量化 resize 全会话）
    metrics_ = terminalCellMetrics(static_cast<float>(settings.fontSize), settings.fontFamily);
    grid_.setTypography(settings.fontFamily, static_cast<float>(settings.fontSize));
    const float cellW = metrics_.cellW;
    const float cellH = metrics_.cellH;
    // ANSI 色板取哪套（跟随应用/独立锁定，§4.2③）：渲染面零新增，仅选表
    const bool gridDark = gridPaletteDark(t.isDark, settings.colorMode, settings.colorPalette);

    const auto pollT0 = std::chrono::steady_clock::now();
    controller.pollAll();
    const double pollMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pollT0)
            .count();
    pf.pollMs += pollMs; // TermPerf（bug248）：全会话 snapshot 总耗时
    pf.pollMaxMs = std::max(pf.pollMaxMs, pollMs);
    if (controller.activeIndex() != lastActiveIndex_) {
        // 切换会话：选区行列锚在旧会话可视网格上，跨会话无效；滚动余量同理。
        // tabScroll_ 不清（P3-3）：TAB 栏横滚是栏视口态非会话态，切会话保位
        lastActiveIndex_ = controller.activeIndex();
        selecting_ = false;
        hasSelection_ = false;
        scrollRemainder_ = 0.0f;
        // bug267-P3a：切会话清 resize 去抖态——pendingSince_ 锚在旧会话的 resize 帧，
        // 新会话若复用旧计时器，wantsResize() 直接视为已超时 → 去抖窗口缩水成 0ms，
        // ConPTY resize 退化回逐帧抖动（bug248 去抖本意失效）。清掉后下方 pending
        // 块对新会话重新武装完整 100ms 窗口。
        clearPendingResize();
    }
    Session* act = controller.active();
    const int sessionCount = controller.count();
    const int sb = controller.activeScrollbackCount();
    if (sessionCount < lastTabCount_) {
        // 关 TAB 后索引左移/越界，hover 记忆指向错 TAB（P3-6）：瞬态直接清除
        hoveredTabIndex_ = -1;
        hoveredCloseIndex_ = -1;
        // bug267-P3a：关会话同口径——激活索引可能不变但会话已换（关掉的就是激活 TAB
        // 时 activeIndex 原地顶移），旧 pendingSince_ 同样造成 0ms 去抖窗口
        clearPendingResize();
    }
    lastTabCount_ = sessionCount;

    // 全屏 = 侧栏 0 宽全隐（clue92 终裁，72px 折叠已作废），TAB 栏带保留 → gridTop 恒 44
    const float gridTop = kToolbarH;
    const float bodyH = std::max(0.0f, screen.height - gridTop);

    // ---- 右动作组几何（顺序写死 §4.2：分隔线→设置→全屏切换→新建；进/退全屏同槽位
    // 纯图标钮位置不换——老板 2026-11-05 裁决，设计 §4.2「两套视觉/退出胶囊」作废）
    const bool narrowWindow = screen.width < 720.0f;
    const float titleFs = core::theme::scaled(14.0f);
    const float detailFs = core::theme::scaled(12.0f);
    const float newLabelW = textWidth("新建", titleFs);
    const float newBtnW = narrowWindow ? 32.0f : 12.0f + 14.0f + 6.0f + newLabelW + 12.0f;
    float sepX = 0.0f, gearX = 0.0f, midX = 0.0f, rightX = 0.0f;
    {
        // 从右缘向左排（两态一致）：新建（right）→ 全屏切换 32 图标钮（mid）→ 设置 → 分隔线
        float x = screen.width - kRightPad;
        rightX = x - newBtnW;
        x = rightX - kActionGap;
        midX = x - 32.0f;
        x = midX - kActionGap;
        gearX = x - 32.0f;
        x = gearX - kActionGap;
        sepX = x - 1.0f;
    }
    const float tabAreaX = 8.0f;
    const float tabAreaW = std::max(0.0f, sepX - kActionGap - tabAreaX);

    // ---- TAB 几何规划（内容自适应 clamp[120,200]；溢出走横滚偏移）
    // bug248：标题字符串缓存——titleNumber 会话内稳定，仅会话数变化时重建
    if (static_cast<int>(tabTitles_.size()) != sessionCount) {
        tabTitles_.resize(static_cast<size_t>(sessionCount));
        for (int i = 0; i < sessionCount; i++) {
            const Session* s = controller.at(i);
            tabTitles_[static_cast<size_t>(i)] =
                "终端 " + std::to_string(s ? s->titleNumber : i + 1);
        }
    }
    std::vector<float> tabW(static_cast<size_t>(sessionCount), 0.0f);
    float tabsTotalW = 0.0f;
    for (int i = 0; i < sessionCount; i++) {
        const std::string& title = tabTitles_[static_cast<size_t>(i)];
        const float titleW = std::min(textWidth(title, titleFs) + 2.0f, kTabTitleMaxW);
        float w = 12.0f + 8.0f + 6.0f + titleW + 6.0f + 16.0f + 12.0f;
        w = std::clamp(w, narrowWindow ? kTabMinWNarrow : kTabMinW, kTabMaxW);
        tabW[static_cast<size_t>(i)] = w;
        tabsTotalW += w + (i > 0 ? kTabGap : 0.0f);
    }
    const float maxTabScroll = std::max(0.0f, tabsTotalW - tabAreaW);
    tabScroll_ = std::clamp(tabScroll_, 0.0f, maxTabScroll);

    const platform::TerminalFrame emptyFrame;
    const platform::TerminalFrame& frame = act ? act->frame : emptyFrame;
    const int totalLines = sb + frame.rows;

    ui.stack("term.root")
        .size(screen.width, screen.height)
        .content([&] {
            // 层 0：容器卡（唯一卡面，glassFill 单一来源；TAB 栏带=卡的上段，§3 一张卡两段式）
            ui.rect("term.bg")
                .size(screen.width, screen.height)
                .color(ui_kit::glassFill(t));

            // 层 0.5：网格底（文字区实色，可读性优先 §3 层2b）
            ui.rect("term.gridbg")
                .position(0.0f, gridTop)
                .size(screen.width, bodyH)
                .color(t.isDark ? t.colors.background : t.colors.surface);

            // 层 0.6：TAB 栏带/网格区分割线
            ui.rect("term.divider")
                .position(0.0f, gridTop - 1.0f)
                .size(screen.width, 1.0f)
                .color(t.colors.borderDefault);

            // 层 1：网格体（逐 run 定位，真实度量）
            ui.column("term.body")
                .width(screen.width)
                .height(bodyH)
                .margin(0.0f, gridTop, 0.0f, 0.0f)
                .content([&] {
                    grid_.compose(
                        ui, frame, sb, act ? act->scrollOffset : 0, screen.width, bodyH, gridDark,
                        metrics_,
                        [&controller](int idx, std::string& out) {
                            return controller.activeScrollbackLine(idx, out);
                        },
                        layout);
                });
            // resize 量化：激活会话与视口行列不一致时回填（切换会话也自动补 resize）；
            // bug248：pending 首次出现时起计时，由 wantsResize() 的 100ms 去抖统一放行
            if (act != nullptr && layout.rows > 0 &&
                (act->rows != layout.rows || act->cols != layout.cols)) {
                pendingRows_ = layout.rows;
                pendingCols_ = layout.cols;
                if (!pendingTimer_) {
                    pendingSince_ = std::chrono::steady_clock::now();
                    pendingTimer_ = true;
                }
            } else {
                pendingRows_ = 0;
                pendingCols_ = 0;
                pendingTimer_ = false;
            }

            // 行/列 → 逻辑像素（选区/IME/光标共用；真实 cell 度量——bug243/245 错位根因）。
            // bug245：折算后仍是窗口绝对坐标（含侧栏宽 + 标题条带），而 gridTop 是页面局部
            // 常量——先经 Router 页面原点（app_sidebar 每帧写入）落到页面坐标再进 rowOf/colOf
            //（与 todo 拖拽 windowToCompose 同缝）；读一次捕获值，勿在指针回调里现取
            const core::Router* router = ::core::appRouter();
            const float pageOriginX = router != nullptr ? router->pageOriginX() : 0.0f;
            const float pageOriginY = router != nullptr ? router->pageOriginY() : 0.0f;
            const float gridTopV = gridTop;
            const float cellHV = cellH;
            const auto rowOf = [gridTopV, cellHV](double pageY) {
                return static_cast<int>((pageY - gridTopV) / cellHV);
            };
            const auto colOf = [cellW](double pageX) {
                return std::max(0, static_cast<int>(pageX / cellW));
            };
            // Press/Release=物理（除 scale），Move=逻辑（input_types.h 契约）——统一折算
            // 到页面坐标再进 rowOf/colOf
            auto toPage = [pageOriginX, pageOriginY](const eui::PointerEvent& e) {
                const double s = e.scale > 0.0 ? static_cast<double>(e.scale) : 1.0;
                const bool physical = e.space != eui::PointerSpace::Logical;
                return std::pair<double, double>(
                    (physical ? e.x / s : e.x) - pageOriginX,
                    (physical ? e.y / s : e.y) - pageOriginY);
            };
            SessionController* ctrl = &controller;

            // 层 2：交互层（IBeam 文本光标，bug243 现象 1；拖选/滚轮/键盘/IME）
            ui.rect("term.input")
                .size(screen.width, bodyH)
                .margin(0.0f, gridTop, 0.0f, 0.0f)
                .color(Color{0, 0, 0, 0.01f})
                .onPress([this, toPage, rowOf, colOf](const eui::PointerEvent& e,
                                                     const eui::Rect&) {
                    auto [lx, ly] = toPage(e);
                    selecting_ = true;
                    hasSelection_ = false;
                    selARow_ = rowOf(ly);
                    selACol_ = colOf(lx);
                    selHRow_ = selARow_;
                    selHCol_ = selACol_;
                })
                .onMove([this, toPage, rowOf, colOf](const eui::PointerEvent& e, const eui::Rect&) {
                    if (!selecting_) return false;
                    auto [lx, ly] = toPage(e);  // Move 已是逻辑，仅补页面原点（bug245）
                    selHRow_ = rowOf(ly);
                    selHCol_ = colOf(lx);
                    hasSelection_ = true;
                    return true;
                })
                .onRelease([this, toPage, rowOf, colOf](const eui::PointerEvent& e,
                                                        const eui::Rect&) {
                    if (!selecting_) return;
                    selecting_ = false;
                    auto [lx, ly] = toPage(e);
                    selHRow_ = rowOf(ly);
                    selHCol_ = colOf(lx);
                    hasSelection_ = !(selARow_ == selHRow_ && selACol_ == selHCol_);
                })
                .onScroll([this, ctrl, cellHV](const core::ScrollEvent& e) {
                    Session* s = ctrl->active();
                    if (s == nullptr) return;
                    scrollRemainder_ += static_cast<float>(e.y);
                    const int lines = static_cast<int>(scrollRemainder_ / cellHV);
                    if (lines == 0) return;
                    scrollRemainder_ -= static_cast<float>(lines) * cellHV;
                    const int maxOffset = ctrl->activeScrollbackCount();
                    s->scrollOffset = std::clamp(s->scrollOffset - lines, 0, maxOffset);
                    // 滚动即清选区：选区行列锚在拖选时可视网格上（终端惯例）
                    hasSelection_ = false;
                    selecting_ = false;
                })
                .onKeyEvent([this, ctrl, onKeyBytes, onCopyText,
                             layout](const eui::KeyEvent& e) {
                    if (!e.isDown()) return false;
                    // Ctrl+C 二象性（技术卡 v2 决策 4/§5）：win 惯例且选区非空 → 复制并吞
                    // 事件；无选区/mac 惯例 → keyencoder 语义（\x03 中断）
                    if (e.modifiers.control && e.key == eui::InputKey::C) {
                        std::string bytes;
                        if (terminal::encodeCtrlC(platform::terminalKeyConventions(),
                                                  hasSelection_, e.modifiers.shift, bytes)) {
                            // P3-2：映射全用事件期现值——compose 捕获的 totalLines 在
                            // 新输出（scrollback push）后已过期，选区行号会被平移
                            Session* sNow = ctrl->active();
                            const int sbNow = ctrl->activeScrollbackCount();
                            const int totalNow = sbNow + (sNow ? sNow->frame.rows : 0);
                            const int startLine =
                                std::max(0, totalNow - layout.rows -
                                                (sNow ? sNow->scrollOffset : 0));
                            const int fR0 = selARow_ + startLine - sbNow;
                            const int fR1 = selHRow_ + startLine - sbNow;
                            std::string sel;
                            if (fR0 >= 0 && fR1 >= 0 && fR1 < layout.rows &&
                                ctrl->activeSelectionText(fR0, selACol_, fR1, selHCol_, sel)) {
                                onCopyText(sel);
                                return true;
                            }
                            bytes.assign(1, '\x03'); // 裁掉/提取失败 → 落回中断
                        }
                        if (!bytes.empty()) {
                            onKeyBytes(reinterpret_cast<const uint8_t*>(bytes.data()),
                                       static_cast<int32_t>(bytes.size()));
                            return true;
                        }
                        return false;
                    }
                    std::string bytes;
                    if (terminal::encodeKey(e.key, e.modifiers, bytes)) {
                        ctrl->writeToActive(reinterpret_cast<const uint8_t*>(bytes.data()),
                                            static_cast<int32_t>(bytes.size()));
                        return true;
                    }
                    return false;
                })
                .onTextInput([onTextInput](const eui::TextInputEvent& e) {
                    if (!e.text.empty()) onTextInput(e.text, false);
                    if (!e.pasteText.empty()) onTextInput(e.pasteText, true);
                })
                // IBeam 必须在全部 on* 绑定之后设置——dsl 的交互绑定会把 cursor 置
                // Hand（dsl.h on* 通道），后设覆盖先设（bug243 现象 1 根修）
                .cursor(eui::CursorShape::IBeam);

            // 层 3：选区高亮 rects（primary α0.30 暗 / α0.18 亮；列→像素按 width 折算）
            if (hasSelection_ && act != nullptr && !frame.cells.empty() && frame.cols > 0) {
                const int top = std::min(selARow_, selHRow_);
                const int bottom = std::max(selARow_, selHRow_);
                const float selAlpha = t.isDark ? 0.30f : 0.18f;
                const int startLine = std::max(0, totalLines - layout.rows - act->scrollOffset);
                for (int r = std::max(top, 0); r <= bottom && r < layout.rows; r++) {
                    const int c0 = (r == top) ? std::min(selACol_, selHCol_) : 0;
                    const int c1 =
                        (r == bottom) ? std::max(selACol_, selHCol_) : std::max(1, layout.cols);
                    if (c1 <= c0) continue;
                    const int frameRow = r + startLine - sb;
                    if (frameRow < 0 || frameRow >= frame.rows) continue;
                    const platform::TerminalCell* row =
                        &frame.cells[static_cast<size_t>(frameRow) * frame.cols];
                    ui.rect("term.sel." + std::to_string(r))
                        .position(cellXOf(row, frame.cols, c0, cellW),
                                  gridTop + static_cast<float>(r) * cellH)
                        .size(cellSpanOf(row, frame.cols, c0, c1, cellW), cellH)
                        .color(Color{t.colors.primary.r, t.colors.primary.g, t.colors.primary.b,
                                     selAlpha});
                }
            }

            // 层 3.5：光标（Running 且光标在可视区；块/线 + 闪烁 1100ms 阶跃，need250 §4.2⑦。
            // 静态锚位不变的是 IME imeRect 元素——term.ime.anchor 独立于本块）
            if (act != nullptr && act->state == SessionState::Running && !frame.cells.empty() &&
                frame.cols > 0 && layout.rows > 0) {
                const int visRow = (sb + frame.cursorRow) -
                                   std::max(0, totalLines - layout.rows - act->scrollOffset);
                if (visRow >= 0 && visRow < layout.rows && frame.cursorCol < frame.cols) {
                    const platform::TerminalCell* crow =
                        &frame.cells[static_cast<size_t>(
                                         std::min(frame.cursorRow, frame.rows - 1)) *
                                     frame.cols];
                    const bool blinkOn =
                        !settings.cursorBlink ||
                        (std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count() /
                         kCursorBlinkMs) %
                            2 ==
                            0;
                    if (blinkOn) {
                        const float curW =
                            settings.cursorStyle == TerminalCursorStyle::Bar ? 2.0f : cellW;
                        ui.rect("term.cursor")
                            .position(cellXOf(crow, frame.cols, frame.cursorCol, cellW),
                                      gridTop + static_cast<float>(visRow) * cellH)
                            .size(curW, cellH)
                            .color(Color{t.colors.textPrimary.r, t.colors.textPrimary.g,
                                         t.colors.textPrimary.b, 0.85f});
                    }
                }
            }

            // 层 4：IME 锚定：光标 cell 可视坐标上报 imeRect（真实度量，bug245）
            if (act != nullptr && !frame.cells.empty() && layout.rows > 0 && frame.cols > 0) {
                const int startLine = std::max(0, totalLines - layout.rows - act->scrollOffset);
                const int visRow = (sb + frame.cursorRow) - startLine;
                if (visRow >= 0 && visRow < layout.rows && frame.cursorCol < frame.cols) {
                    const int frameRow = visRow + startLine - sb;
                    const platform::TerminalCell* row =
                        frameRow >= 0 && frameRow < frame.rows
                            ? &frame.cells[static_cast<size_t>(frameRow) * frame.cols]
                            : nullptr;
                    const float ix = cellXOf(row, frame.cols, frame.cursorCol, cellW);
                    const float iy = gridTop + static_cast<float>(visRow) * cellH;
                    // ImeTrace 段1（bug245 临时诊断，≤30 条；光标变位或每 120 帧一条，
                    // 与段2/3 同落 ime-trace.log 对齐阅读）
                    {
                        static int imeTraceN = 0;
                        static int imeTraceFrame = 0;
                        static int lastCr = -1;
                        static int lastCc = -1;
                        static int lastSb = -1;
                        ++imeTraceFrame;
                        const bool changed = frame.cursorRow != lastCr ||
                                             frame.cursorCol != lastCc ||
                                             act->scrollOffset != lastSb;
                        if (imeTraceN < 30 && (changed || imeTraceFrame % 120 == 0)) {
                            ++imeTraceN;
                            lastCr = frame.cursorRow;
                            lastCc = frame.cursorCol;
                            lastSb = act->scrollOffset;
                            char line[192];
                            std::snprintf(line, sizeof(line),
                                          "ImeTrace: src ix=%.1f iy=%.1f cellW=%.2f cellH=%.2f "
                                          "cursorRow=%d cursorCol=%d scrollOffset=%d",
                                          static_cast<double>(ix), static_cast<double>(iy),
                                          static_cast<double>(cellW), static_cast<double>(cellH),
                                          frame.cursorRow, frame.cursorCol, act->scrollOffset);
                            eui_ime_trace_log(line);
                        }
                    }
                    // P1-2：imeRect 是 element.frame 之上的偏移（runtime_geometry.h
                    // 最终矩形=frame+imeRect 再过变换），position 已把坐标编进 frame
                    // ——再传 (ix,iy) 即双计，候选窗飞出光标区。偏移恒 (0,0,cellW,cellH)
                    ui.rect("term.ime.anchor")
                        .position(ix, iy)
                        .size(cellW, cellH)
                        .color(kTransparent)
                        .imeRect(0.0f, 0.0f, cellW, cellH);
                }
            }

            // 层 5：TAB 栏带 44px（全屏保留）
            {
                // 分隔竖线
                ui.rect("term.act.sep")
                    .position(sepX, (kToolbarH - 20.0f) * 0.5f)
                    .size(1.0f, 20.0f)
                    .color(t.colors.borderDefault);

                // 设置（32×32 图标钮；need250 改开页内模态面板——原 navigateTo(Settings)
                // 误接及其全屏复位 workaround 一并拆除）
                ui.rect("term.act.gear.bg")
                    .position(gearX, (kToolbarH - 32.0f) * 0.5f)
                    .size(32.0f, 32.0f)
                    .radius(6.0f)
                    .states(kTransparent, t.colors.hover, t.colors.primarySoftBg)
                    .onClick([this]() { openSettings(); });
                ui.text("term.act.gear.icon")
                    .position(gearX, (kToolbarH - 32.0f) * 0.5f)
                    .size(32.0f, 32.0f)
                    .icon(kFaSliders)
                    .fontSize(core::theme::scaled(16.0f))
                    .color(t.colors.textSecondary)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center);

                // 全屏切换钮（纯图标，进/退同槽位同尺寸，只切图标——老板 2026-11-05 裁决）
                ui.rect("term.act.fs.bg")
                    .position(midX, (kToolbarH - 32.0f) * 0.5f)
                    .size(32.0f, 32.0f)
                    .radius(6.0f)
                    .states(kTransparent, t.colors.hover, t.colors.primarySoftBg)
                    .onClick([this]() { toggleFullscreen(); });
                ui.text("term.act.fs.icon")
                    .position(midX, (kToolbarH - 32.0f) * 0.5f)
                    .size(32.0f, 32.0f)
                    .icon(fullscreen_ ? kFaCompress : kFaExpand)
                    .fontSize(core::theme::scaled(16.0f))
                    .color(t.colors.textSecondary)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center);

                // + 新建主按钮（唯一鲜艳色；窄窗收缩 32×32 图标钮 §8）。
                // 两态恒锚动作组最右（rightX）——全屏切换钮固定 mid 槽后位置不再互换
                {
                    const float newX = rightX;
                    const std::string bgId = "term.act.new";
                    ui.rect(bgId + ".bg")
                        .position(newX, kTabY)
                        .size(newBtnW, kTabH)
                        .radius(6.0f)
                        .states(t.colors.primary, t.colors.primaryHover, t.colors.primaryPressed)
                        .onClick([ctrl]() { ctrl->startSession(); });
                    if (narrowWindow) {
                        ui.text(bgId + ".icon")
                            .position(newX, kTabY)
                            .size(32.0f, kTabH)
                            .icon(kFaPlus)
                            .fontSize(core::theme::scaled(16.0f))
                            .color(t.colors.onPrimary)
                            .horizontalAlign(eui::HorizontalAlign::Center)
                            .verticalAlign(eui::VerticalAlign::Center);
                    } else {
                        ui.text(bgId + ".icon")
                            .position(newX + 12.0f, kTabY)
                            .size(14.0f, kTabH)
                            .icon(kFaPlus)
                            .fontSize(core::theme::scaled(14.0f))
                            .color(t.colors.onPrimary)
                            .verticalAlign(eui::VerticalAlign::Center);
                        ui.text(bgId + ".label")
                            .position(newX + 12.0f + 14.0f + 6.0f, kTabY)
                            .size(newLabelW + 4.0f, kTabH)
                            .text("新建")
                            .fontSize(titleFs)
                            .color(t.colors.onPrimary)
                            .verticalAlign(eui::VerticalAlign::Center);
                    }
                }

                // TAB 区（clip 横滚；空白区滚轮横滚）
                if (sessionCount > 0) {
                    ui.rect("term.tabs.scroll")
                        .position(tabAreaX, 0.0f)
                        .size(tabAreaW, kToolbarH)
                        .color(kTransparent)
                        .onScroll([this](const core::ScrollEvent& e) {
                            tabScroll_ += static_cast<float>(e.y) * 24.0f;
                            tabScroll_ = std::max(0.0f, tabScroll_);
                        })
                        .cursor(eui::CursorShape::Arrow);
                }
                ui.stack("term.tabs.clip")
                    .position(tabAreaX, kTabY)
                    .size(tabAreaW, kTabH)
                    .clip()
                    .content([&] {
                        float x = -tabScroll_;
                        for (int i = 0; i < sessionCount; i++) {
                            const Session* s = controller.at(i);
                            const bool isActive = i == controller.activeIndex();
                            const float w = tabW[static_cast<size_t>(i)];
                            if (x + w > 0.0f && x < tabAreaW) {
                                const std::string id = "term.tab." + std::to_string(i);
                                const std::string& title =
                                    tabTitles_[static_cast<size_t>(i)]; // bug248：复用缓存
                                const float titleW = std::min(
                                    textWidth(title, titleFs) + 2.0f, kTabTitleMaxW);
                                ui.stack(id)
                                    .position(x, 0.0f)
                                    .size(w, kTabH)
                                    .content([&, id, i, w, isActive, title, titleW, s] {
                                        // TAB 底（激活=surface 实色+1px 边+底缘绿条；
                                        // 非激活=透明/hover/press 三态）
                                        ui.rect(id + ".bg")
                                            .size(w, kTabH)
                                            .radius(6.0f)
                                            .border(isActive ? 1.0f : 0.0f,
                                                    isActive ? t.colors.borderDefault
                                                             : kTransparent)
                                            .states(isActive
                                                        ? (t.isDark ? t.colors.surface
                                                                    : t.colors.surface)
                                                        : kTransparent,
                                                    isActive ? (t.isDark ? t.colors.surface
                                                                         : t.colors.surface)
                                                             : t.colors.hover,
                                                    isActive
                                                        ? (t.isDark ? t.colors.surface
                                                                    : t.colors.surface)
                                                        : t.colors.primarySoftBg)
                                            .onHover([this, i](bool h) {
                                                if (h) hoveredTabIndex_ = i;
                                                else if (hoveredTabIndex_ == i) hoveredTabIndex_ = -1;
                                            })
                                            .onClick([ctrl, i]() { ctrl->setActive(i); });
                                        if (isActive) {
                                            ui.rect(id + ".bar")
                                                .position(0.0f, kTabH - 2.0f)
                                                .size(w, 2.0f)
                                                .radius(1.0f)
                                                .color(t.colors.primary);
                                        }
                                        // 状态点 8×8（绿=运行/空心=待启动/灰=结束/红=失败）
                                        const Color dotColor =
                                            s == nullptr ? t.colors.textTertiary
                                            : s->state == SessionState::Running ? t.colors.primary
                                            : s->state == SessionState::Failed ? t.colors.error
                                            : s->state == SessionState::Exited
                                                ? t.colors.textTertiary
                                                : kTransparent;
                                        ui.rect(id + ".dot")
                                            .position(12.0f, (kTabH - 8.0f) * 0.5f)
                                            .size(8.0f, 8.0f)
                                            .radius(4.0f)
                                            .border(s != nullptr &&
                                                            s->state == SessionState::Idle
                                                        ? 1.0f
                                                        : 0.0f,
                                                    s != nullptr &&
                                                            s->state == SessionState::Idle
                                                        ? t.colors.textTertiary
                                                        : kTransparent)
                                            .color(dotColor);
                                        // 标题（激活 textPrimary / 非激活 textSecondary）
                                        ui.text(id + ".title")
                                            .position(12.0f + 8.0f + 6.0f, 0.0f)
                                            .size(titleW, kTabH)
                                            .text(title)
                                            .fontSize(titleFs)
                                            .overflow(core::TextOverflow::Ellipsis)
                                            .color(isActive ? t.colors.textPrimary
                                                            : t.colors.textSecondary)
                                            .verticalAlign(eui::VerticalAlign::Center);
                                        // 关闭钮视觉 16×16（激活常显/非激活 hover 淡入；
                                        // 红警示 §4.1.3 只看关闭钮自身 hover——P2-5：
                                        // 悬停 TAB 其余区域不得红，红底由 hit 三态自带）
                                        const bool closeVisible =
                                            isActive || hoveredTabIndex_ == i ||
                                            hoveredCloseIndex_ == i;
                                        const bool closeHover = hoveredCloseIndex_ == i;
                                        if (closeVisible) {
                                            ui.text(id + ".close.icon")
                                                .position(w - 12.0f - 16.0f,
                                                          (kTabH - 16.0f) * 0.5f)
                                                .size(16.0f, 16.0f)
                                                .icon(kFaXmark)
                                                .fontSize(core::theme::scaled(10.0f))
                                                .color(closeHover ? t.colors.errorText
                                                                  : t.colors.textTertiary)
                                                .horizontalAlign(eui::HorizontalAlign::Center)
                                                .verticalAlign(eui::VerticalAlign::Center);
                                        }
                                        // 关闭钮独立命中区 20×20（z 序最上，点击不触发切换；
                                        // hover 独立维护，不与 TAB hover 互写）
                                        ui.rect(id + ".close.hit")
                                            .position(w - 12.0f - 16.0f - 2.0f,
                                                      (kTabH - 20.0f) * 0.5f)
                                            .size(20.0f, 20.0f)
                                            .states(kTransparent,
                                                    Color{t.colors.error.r, t.colors.error.g,
                                                          t.colors.error.b, 0.10f},
                                                    Color{t.colors.error.r, t.colors.error.g,
                                                          t.colors.error.b, 0.18f})
                                            .onHover([this, i](bool h) {
                                                if (h) hoveredCloseIndex_ = i;
                                                else if (hoveredCloseIndex_ == i)
                                                    hoveredCloseIndex_ = -1;
                                            })
                                            .onClick([ctrl, i]() { ctrl->closeSession(i); });
                                    });
                            }
                            x += w + kTabGap;
                        }
                    });
            }

            // 层 6：空态（0 TAB，§4.5）
            if (sessionCount == 0) {
                const float iconY = gridTop + bodyH * 0.5f - 60.0f;
                ui.text("term.empty.icon")
                    .position(0.0f, iconY)
                    .size(screen.width, 24.0f)
                    .icon(kFaTerminal)
                    .fontSize(24.0f)
                    .color(t.colors.textTertiary)
                    .horizontalAlign(eui::HorizontalAlign::Center);
                ui.text("term.empty.text")
                    .position(0.0f, iconY + 24.0f + 8.0f)
                    .size(screen.width, 20.0f)
                    .text("没有终端会话")
                    .fontSize(core::theme::scaled(14.0f))
                    .color(t.colors.textSecondary)
                    .horizontalAlign(eui::HorizontalAlign::Center);
                const float linkW = textWidth("+ 新建会话", core::theme::scaled(14.0f)) + 8.0f;
                ui.text("term.empty.new")
                    .position((screen.width - linkW) * 0.5f, iconY + 24.0f + 8.0f + 12.0f)
                    .size(linkW, 20.0f)
                    .text("+ 新建会话")
                    .fontSize(core::theme::scaled(14.0f))
                    .color(t.colors.primaryText)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .onClick([ctrl]() { ctrl->startSession(); });
            }

            // 层 7：覆盖卡（激活会话 Exited/Failed，§4.4；语义跟 TAB 走）
            if (act != nullptr &&
                (act->state == SessionState::Exited || act->state == SessionState::Failed)) {
                const bool failed = act->state == SessionState::Failed;
                const int activeIdx = controller.activeIndex();
                const std::string title =
                    failed ? "终端启动失败"
                           : "会话已结束，退出码 " + std::to_string(act->exitCode);
                const std::string detail =
                    failed ? (act->error.empty() ? "未知错误" : act->error)
                           : "输出已保留，可重新开始或新建会话";
                const float cardW = 360.0f, cardH = 148.0f;
                const float cx = (screen.width - cardW) * 0.5f;
                const float cy = gridTop + (bodyH - cardH) * 0.5f;
                ui.rect("term.overlay.scrim")
                    .position(0.0f, gridTop)
                    .size(screen.width, bodyH)
                    .color(Color{0.0f, 0.0f, 0.0f, 0.35f});
                ui_kit::cardPanel(ui, "term.overlay.card", cx, cy, cardW, cardH,
                                  t.sizes.radiusLarge, t);
                ui.column("term.overlay.card.body")
                    .width(cardW)
                    .height(cardH)
                    .padding(cx + t.sizes.spacingLarge, cy + t.sizes.spacingLarge, 0.0f, 0.0f)
                    .content([&] {
                        ui.row("term.overlay.title.row")
                            .size(cardW - 2.0f * t.sizes.spacingLarge, 22.0f)
                            .gap(8.0f)
                            .alignItems(eui::Align::CENTER)
                            .content([&] {
                                ui.text("term.overlay.icon")
                                    .size(16.0f, 22.0f)
                                    .icon(failed ? kFaCircleExclamation : kFaCircleCheck)
                                    .fontSize(16.0f)
                                    .color(failed ? t.colors.error : t.colors.textTertiary);
                                ui.text("term.overlay.title")
                                    .size(cardW - 2.0f * t.sizes.spacingLarge - 24.0f, 22.0f)
                                    .text(title)
                                    .fontSize(15.0f)
                                    .color(t.colors.textPrimary);
                            });
                        ui.text("term.overlay.detail")
                            .size(cardW - 2.0f * t.sizes.spacingLarge,
                                  core::theme::scaled(36.0f))
                            .text(detail)
                            .fontSize(detailFs)
                            .color(t.colors.textSecondary)
                            .overflow(core::TextOverflow::Clip);
                        // 按钮行：主（重新开始/重试）+ 次（关闭标签），§4.4
                        const float btnH = 28.0f;
                        const float retryLabelW =
                            textWidth(failed ? "重试" : "重新开始", core::theme::scaled(13.0f));
                        const float retryW = retryLabelW + 24.0f;
                        const float closeLabelW =
                            textWidth("关闭标签", core::theme::scaled(13.0f));
                        const float closeW = closeLabelW + 24.0f;
                        ui.row("term.overlay.btns")
                            .size(cardW - 2.0f * t.sizes.spacingLarge, btnH)
                            .gap(8.0f)
                            .content([&] {
                                ui.stack("term.overlay.retry.unit")
                                    .size(retryW, btnH)
                                    .content([&, retryW, failed] {
                                        ui.rect("term.overlay.retry")
                                            .size(retryW, btnH)
                                            .radius(6.0f)
                                            .states(t.colors.primary, t.colors.primaryHover,
                                                    t.colors.primaryPressed)
                                            .onClick([this, ctrl, activeIdx]() {
                                                scrollRemainder_ = 0.0f; // P3-1：重启=全新输出流
                                                ctrl->restartSession(activeIdx);
                                            });
                                        ui.text("term.overlay.retry.label")
                                            .size(retryW, btnH)
                                            .text(failed ? "重试" : "重新开始")
                                            .fontSize(core::theme::scaled(13.0f))
                                            .color(t.colors.onPrimary)
                                            .horizontalAlign(eui::HorizontalAlign::Center)
                                            .verticalAlign(eui::VerticalAlign::Center);
                                    });
                                ui.stack("term.overlay.close.unit")
                                    .size(closeW, btnH)
                                    .content([&, closeW] {
                                        ui.rect("term.overlay.close")
                                            .size(closeW, btnH)
                                            .radius(6.0f)
                                            .states(t.isDark ? t.colors.hover
                                                             : Color{0.95f, 0.95f, 0.95f, 1.0f},
                                                    t.colors.hover, t.colors.hover)
                                            .onClick([ctrl, activeIdx]() {
                                                ctrl->closeSession(activeIdx);
                                            });
                                        ui.text("term.overlay.close.label")
                                            .size(closeW, btnH)
                                            .text("关闭标签")
                                            .fontSize(core::theme::scaled(13.0f))
                                            .color(t.colors.textPrimary)
                                            .horizontalAlign(eui::HorizontalAlign::Center)
                                            .verticalAlign(eui::VerticalAlign::Center);
                                    });
                            });
                    });
            }

            // 层 8：按需出帧泵（bug249）：任一会话 Running 期间挂 1×1 onFrame 元素——
            // EUI 契约 onFrame 每帧置 composeRequested/animating（runtime_update.h），
            // reader 新输出因此自动重组上屏，无需点击唤醒；无 Running 会话不挂 →
            // 空闲零空转。回调体空，重组由下一帧 compose（含 pollAll）完成。
            bool anyRunning = false;
            for (int i = 0; i < sessionCount; i++) {
                const Session* s = controller.at(i);
                if (s != nullptr && s->state == SessionState::Running) {
                    anyRunning = true;
                    break;
                }
            }
            if (anyRunning) {
                ui.rect("term.framepump")
                    .position(0.0f, 0.0f)
                    .size(1.0f, 1.0f)
                    .color(kTransparent)
                    .onFrame([](float) {});
            }

            // 层 9：终端设置面板（need250 页内模态；设计 §2/§4）。scrim α0.35 盖整页面区
            // 含 TAB 栏；点 scrim 不关（无 onClick = 纯 hit 隔离）；关闭通道 Esc（module
            // 级 onKeyEvent）/ × / 完成。会话照常 poll——字体/配色即时生效透过半透 scrim
            // 可感知（§2.1）。
            if (settingsOpen_) {
                const float cardW = std::min(kPanelW, std::max(320.0f, screen.width - 32.0f));
                const bool narrowPanel = screen.width < 600.0f;
                const float rowH = narrowPanel ? kRowBlockHNarrow : kRowBlockH;
                const float innerW = cardW - 2.0f * kPanelPad;
                const float controlX = kLabelW + kLabelGap;
                const float controlW = innerW - kLabelW - kLabelGap;
                const auto ctlY = [&](float ry) {
                    return narrowPanel ? ry + 24.0f : ry + (kRowBlockH - kControlH) * 0.5f;
                };

                const bool workDirInvalid =
                    !settings.workDir.empty() && !workDirUsableForSpawn(settings.workDir);
                const bool scrollbackHeavy = settings.scrollback > kScrollbackWarnThreshold;

                // 行画布高（error/hint 行参与布局，§4.2⑤⑥）
                float canvasH = 12.0f + kGroupHeadH + 8.0f;          // 分组头「外观」
                canvasH += rowH + 8.0f + rowH + 8.0f + rowH;         // 字体/字号/配色
                canvasH += 12.0f + kGroupHeadH + 8.0f;               // 分组头「会话」
                canvasH += rowH + 8.0f +                             // 默认 shell
                           rowH + (workDirInvalid ? kHintH : 0.0f) + 8.0f +  // 启动目录(+错误行)
                           rowH + (scrollbackHeavy ? kHintH : 0.0f) + 8.0f + // 回滚(+警示行)
                           rowH;                                             // 光标

                const float titleBlock = kPanelPad + 28.0f + 12.0f;
                const float footerBlock = 12.0f + 1.0f + 12.0f + 32.0f + kPanelPad;
                const float neededH = titleBlock + canvasH + footerBlock;
                const float cardH =
                    std::min(neededH, std::max(240.0f, screen.height - 64.0f));
                const float viewportH =
                    std::max(80.0f, cardH - titleBlock - footerBlock);
                const float cardX = (screen.width - cardW) * 0.5f;
                const float cardY = (screen.height - cardH) * 0.5f;
                const auto ct = core::theme::componentTokens(t);

                // 事件闭包只捕稳对象（模块成员指针 / 值拷贝的 std::function）；compose
                // 栈局部一律不进闭包（EUI 事件在帧间派发，引用捕获悬空）
                TerminalSettings* sp = &settings;
                auto persist = onSettingsChanged;
                // 次按钮底色（设计 §9：浅 #F2F2F2/#E5E5E5 · 暗 #353231/#3E3B39）
                const Color doneBg = t.isDark ? Color{0x35 / 255.0f, 0x32 / 255.0f, 0x31 / 255.0f, 1.0f}
                                              : Color{0xF2 / 255.0f, 0xF2 / 255.0f, 0xF2 / 255.0f, 1.0f};
                const Color doneHover =
                    t.isDark ? Color{0x3E / 255.0f, 0x3B / 255.0f, 0x39 / 255.0f, 1.0f}
                             : Color{0xE5 / 255.0f, 0xE5 / 255.0f, 0xE5 / 255.0f, 1.0f};

                ui.rect("term.set.scrim")
                    .position(0.0f, 0.0f)
                    .size(screen.width, screen.height)
                    .color(Color{0.0f, 0.0f, 0.0f, 0.35f});
                ui_kit::cardPanel(ui, "term.set.card", cardX, cardY, cardW, cardH,
                                  t.sizes.radiusLarge, t);

                // 标题行 + × 关闭（24×24，hover 底，fa-xmark）
                ui.text("term.set.title")
                    .position(cardX + kPanelPad, cardY + kPanelPad)
                    .size(cardW - 2.0f * kPanelPad - 32.0f, 28.0f)
                    .text("终端设置")
                    .fontSize(core::theme::scaled(15.0f))
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .verticalAlign(eui::VerticalAlign::Center);
                ui.rect("term.set.close.bg")
                    .position(cardX + cardW - kPanelPad - 24.0f, cardY + kPanelPad + 2.0f)
                    .size(24.0f, 24.0f)
                    .radius(6.0f)
                    .states(kTransparent, t.colors.hover, t.colors.hover)
                    .onClick([this]() { closeSettings(); });
                ui.text("term.set.close.icon")
                    .position(cardX + cardW - kPanelPad - 24.0f, cardY + kPanelPad + 2.0f)
                    .size(24.0f, 24.0f)
                    .icon(kFaXmark)
                    .fontSize(core::theme::scaled(12.0f))
                    .color(t.colors.textSecondary)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center);

                // 行标签/微字基线（横排 label 128 定宽左对齐；窄窗 label 独占行，
                // 「对新会话生效」贴 label 行右缘 §6）
                auto drawRowBase = [&](eui::Ui& inner, const std::string& id, float y,
                                       const char* label, bool newSessionHint) {
                    const float lw = narrowPanel ? innerW : kLabelW;
                    const float ly = narrowPanel ? y : y + (kRowBlockH - 20.0f) * 0.5f;
                    inner.text(id + ".label")
                        .position(0.0f, ly)
                        .size(lw, 20.0f)
                        .text(label)
                        .fontSize(core::theme::scaled(14.0f))
                        .color(t.colors.textPrimary)
                        .verticalAlign(eui::VerticalAlign::Center);
                    if (newSessionHint) {
                        const float hw =
                            textWidth("对新会话生效", core::theme::scaled(12.0f)) + 4.0f;
                        inner.text(id + ".hint")
                            .position(innerW - hw, y)
                            .size(hw, 20.0f)
                            .text("对新会话生效")
                            .fontSize(core::theme::scaled(12.0f))
                            .color(t.colors.textTertiary)
                            .horizontalAlign(eui::HorizontalAlign::Right)
                            .verticalAlign(eui::VerticalAlign::Center);
                    }
                };

                // 内容区内滚（滚动条视觉隐藏 §2.2/§8）
                components::scrollView(ui, "term.set.scroll")
                    .position(cardX + kPanelPad, cardY + titleBlock)
                    .size(innerW, viewportH)
                    .gap(0.0f)
                    .step(rowH)
                    .theme(ct)
                    .scrollbarOverlay(true)
                    .scrollbarWidth(0.0f)
                    .offset(panelScroll_)
                    .onChange([this](float v) { panelScroll_ = v; })
                    .contentKey("term.set.rows")
                    .content([&](eui::Ui& inner, float canvasW, float) {
                        inner.stack("term.set.rows.canvas")
                            .size(canvasW, canvasH)
                            .content([&] {
                                const OptionTable& ft = fontOptionTable();
                                const OptionTable& sht = shellOptionTable();
                                float y = 0.0f;

                                inner.text("term.set.g1")
                                    .position(0.0f, y + 12.0f)
                                    .size(innerW, kGroupHeadH)
                                    .text("外观")
                                    .fontSize(core::theme::scaled(12.0f))
                                    .color(t.colors.textSecondary)
                                    .verticalAlign(eui::VerticalAlign::Center);
                                y += 12.0f + kGroupHeadH + 8.0f;

                                // ① 字体（即时：度量链重算 → 全会话 resize）
                                drawRowBase(inner, "term.set.row.font", y, "字体", false);
                                inner.stack("term.set.row.font.slot")
                                    .position(controlX, ctlY(y))
                                    .size(200.0f, kControlH)
                                    .content([&] {
                                        components::dropdown(inner, "term.set.row.font.dd")
                                            .size(200.0f, kControlH)
                                            .theme(ct)
                                            .items(ft.labels)
                                            .selected(indexOfValue(ft, settings.fontFamily))
                                            .onChange([sp, persist](int idx) {
                                                // 表引用经静态单例访问器在闭包内取
                                                //（compose 局部不进跨帧闭包）
                                                const OptionTable& table = fontOptionTable();
                                                sp->fontFamily =
                                                    table.values[std::clamp(
                                                        idx, 0,
                                                        static_cast<int>(table.values.size()) -
                                                            1)];
                                                persist();
                                            })
                                            .build();
                                    });
                                y += rowH + 8.0f;

                                // ② 字号（即时；钳 10–24 由 stepper min/max 承担）
                                drawRowBase(inner, "term.set.row.fsize", y, "字号", false);
                                inner.stack("term.set.row.fsize.slot")
                                    .position(controlX, ctlY(y))
                                    .size(120.0f, kControlH)
                                    .content([&] {
                                        components::stepper(inner, "term.set.row.fsize.step")
                                            .size(120.0f, kControlH)
                                            .theme(ct)
                                            .min(kFontSizeMin)
                                            .max(kFontSizeMax)
                                            .step(1)
                                            .value(settings.fontSize)
                                            .onChange([sp, persist](long long v) {
                                                sp->fontSize = clampedFontSize(
                                                    static_cast<int>(v));
                                                persist();
                                            })
                                            .build();
                                    });
                                y += rowH + 8.0f;

                                // ③ 配色主题：模式 segmented + 色板 dropdown 联动（跟随
                                // 应用时色板置灰=灰字显示当前继承值，§4.2③）
                                drawRowBase(inner, "term.set.row.palette", y, "配色主题", false);
                                const float palCtlY = ctlY(y);
                                inner.stack("term.set.row.palette.mode.slot")
                                    .position(controlX, palCtlY)
                                    .size(160.0f, kControlH)
                                    .content([&] {
                                        components::segmented(inner, "term.set.row.palette.mode")
                                            .size(160.0f, kControlH)
                                            .theme(ct)
                                            .items(std::vector<std::string>{"跟随应用", "独立"})
                                            .selected(settings.colorMode ==
                                                              TerminalColorMode::Independent
                                                          ? 1
                                                          : 0)
                                            .onChange([sp, persist](int idx) {
                                                sp->colorMode = idx == 1
                                                                    ? TerminalColorMode::Independent
                                                                    : TerminalColorMode::FollowApp;
                                                persist();
                                            })
                                            .build();
                                    });
                                if (settings.colorMode == TerminalColorMode::Independent) {
                                    inner.stack("term.set.row.palette.dd.slot")
                                        .position(controlX + 168.0f, palCtlY)
                                        .size(140.0f, kControlH)
                                        .content([&] {
                                            components::dropdown(
                                                inner, "term.set.row.palette.dd")
                                                .size(140.0f, kControlH)
                                                .theme(ct)
                                                .items(std::vector<std::string>{"亮色 ANSI",
                                                                                "暗色 ANSI"})
                                                .selected(settings.colorPalette ==
                                                                  TerminalColorPalette::Dark
                                                              ? 1
                                                              : 0)
                                                .onChange([sp, persist](int idx) {
                                                    sp->colorPalette =
                                                        idx == 1 ? TerminalColorPalette::Dark
                                                                 : TerminalColorPalette::Light;
                                                    persist();
                                                })
                                                .build();
                                        });
                                } else {
                                    inner.text("term.set.row.palette.inherit")
                                        .position(controlX + 168.0f, palCtlY)
                                        .size(140.0f, kControlH)
                                        .text(settings.colorPalette == TerminalColorPalette::Dark
                                                  ? "暗色 ANSI"
                                                  : "亮色 ANSI")
                                        .fontSize(core::theme::scaled(13.0f))
                                        .color(t.colors.textTertiary)
                                        .verticalAlign(eui::VerticalAlign::Center);
                                }
                                y += rowH + 8.0f;

                                inner.text("term.set.g2")
                                    .position(0.0f, y + 12.0f)
                                    .size(innerW, kGroupHeadH)
                                    .text("会话")
                                    .fontSize(core::theme::scaled(12.0f))
                                    .color(t.colors.textSecondary)
                                    .verticalAlign(eui::VerticalAlign::Center);
                                y += 12.0f + kGroupHeadH + 8.0f;

                                // ④ 默认 shell（仅新会话；行内右缘微字）
                                drawRowBase(inner, "term.set.row.shell", y, "默认 shell", true);
                                inner.stack("term.set.row.shell.slot")
                                    .position(controlX, ctlY(y))
                                    .size(200.0f, kControlH)
                                    .content([&] {
                                        components::dropdown(inner, "term.set.row.shell.dd")
                                            .size(200.0f, kControlH)
                                            .theme(ct)
                                            .items(sht.labels)
                                            .selected(indexOfValue(sht, settings.shell))
                                            .onChange([sp, persist](int idx) {
                                                const OptionTable& table = shellOptionTable();
                                                sp->shell =
                                                    table.values[std::clamp(
                                                        idx, 0,
                                                        static_cast<int>(table.values.size()) -
                                                            1)];
                                                persist();
                                            })
                                            .build();
                                    });
                                y += rowH + 8.0f;

                                // ⑤ 启动目录（仅新会话；无效 → 红边+行下红字，不阻断；
                                // 浏览 = 原生目录对话框，选中回填）
                                drawRowBase(inner, "term.set.row.wdir", y, "启动目录", true);
                                const float inputW = controlW - 64.0f - 8.0f;
                                const float wdirCtlY = ctlY(y);
                                inner.stack("term.set.row.wdir.slot")
                                    .position(controlX, wdirCtlY)
                                    .size(inputW, kControlH)
                                    .content([&] {
                                        components::input(inner, "term.set.row.wdir.input")
                                            .size(inputW, kControlH)
                                            .theme(ct)
                                            .value(settings.workDir)
                                            .placeholder("默认（用户主目录）")
                                            .fontSize(core::theme::scaled(13.0f))
                                            .onChange([sp, persist](const std::string& txt) {
                                                sp->workDir = txt;
                                                persist();
                                            })
                                            .build();
                                    });
                                // 浏览钮（设计 §4.2 input+浏览钮）：接平台原生目录对话框；
                                // 取消/失败静默不动（pickFolder 返回 fail，不打扰输入）
                                const float browseW = 64.0f;
                                const float browseX = controlX + inputW + 8.0f;
                                inner.rect("term.set.row.wdir.browse")
                                    .position(browseX, wdirCtlY)
                                    .size(browseW, kControlH)
                                    .radius(6.0f)
                                    .states(kTransparent, t.colors.hover, t.colors.hover)
                                    .onClick([sp, persist, startDir = settings.workDir]() {
                                        auto picked = platform::folderpicker::pickFolder(
                                            "选择启动目录", startDir);
                                        if (picked.isOk()) {
                                            sp->workDir = picked.value();
                                            persist();
                                        }
                                    });
                                inner.text("term.set.row.wdir.browse.label")
                                    .position(browseX, wdirCtlY)
                                    .size(browseW, kControlH)
                                    .text("浏览")
                                    .fontSize(core::theme::scaled(13.0f))
                                    .color(t.colors.textSecondary)
                                    .horizontalAlign(eui::HorizontalAlign::Center)
                                    .verticalAlign(eui::VerticalAlign::Center);
                                if (workDirInvalid) {
                                    inner.rect("term.set.row.wdir.errborder")
                                        .position(controlX - 1.0f, wdirCtlY - 1.0f)
                                        .size(inputW + 2.0f, kControlH + 2.0f)
                                        .color(kTransparent)
                                        .border(1.0f, t.colors.error);
                                    inner.text("term.set.row.wdir.errtext")
                                        .position(controlX, y + kRowBlockH + 2.0f)
                                        .size(innerW - controlX, kHintH)
                                        .text("目录不存在")
                                        .fontSize(core::theme::scaled(13.0f))
                                        .color(t.colors.errorText);
                                }
                                y += rowH + (workDirInvalid ? kHintH : 0.0f) + 8.0f;

                                // ⑥ 回滚行数（即时改上限裁最旧；>50000 行下微字）
                                drawRowBase(inner, "term.set.row.sb", y, "回滚行数", false);
                                inner.stack("term.set.row.sb.slot")
                                    .position(controlX, ctlY(y))
                                    .size(140.0f, kControlH)
                                    .content([&] {
                                        components::stepper(inner, "term.set.row.sb.step")
                                            .size(140.0f, kControlH)
                                            .theme(ct)
                                            .min(kScrollbackMin)
                                            .max(kScrollbackMax)
                                            .step(100)
                                            .value(settings.scrollback)
                                            .onChange([sp, persist](long long v) {
                                                sp->scrollback =
                                                    clampedScrollback(static_cast<int>(v));
                                                persist();
                                            })
                                            .build();
                                    });
                                if (scrollbackHeavy) {
                                    inner.text("term.set.row.sb.hint")
                                        .position(controlX, y + kRowBlockH + 2.0f)
                                        .size(innerW - controlX, kHintH)
                                        .text("较大回滚占用内存")
                                        .fontSize(core::theme::scaled(12.0f))
                                        .color(t.colors.textSecondary);
                                }
                                y += rowH + (scrollbackHeavy ? kHintH : 0.0f) + 8.0f;

                                // ⑦ 光标样式（即时）：块/线 segmented + 闪烁 switch
                                drawRowBase(inner, "term.set.row.cursor", y, "光标", false);
                                const float curCtlY = ctlY(y);
                                inner.stack("term.set.row.cursor.style.slot")
                                    .position(controlX, curCtlY)
                                    .size(120.0f, kControlH)
                                    .content([&] {
                                        components::segmented(inner, "term.set.row.cursor.style")
                                            .size(120.0f, kControlH)
                                            .theme(ct)
                                            .items(std::vector<std::string>{"块", "线"})
                                            .selected(settings.cursorStyle ==
                                                              TerminalCursorStyle::Bar
                                                          ? 1
                                                          : 0)
                                            .onChange([sp, persist](int idx) {
                                                sp->cursorStyle = idx == 1
                                                                      ? TerminalCursorStyle::Bar
                                                                      : TerminalCursorStyle::Block;
                                                persist();
                                            })
                                            .build();
                                    });
                                const float blinkLabelW =
                                    textWidth("闪烁", core::theme::scaled(12.0f)) + 6.0f;
                                inner.text("term.set.row.cursor.blink.label")
                                    .position(controlX + 136.0f, curCtlY)
                                    .size(blinkLabelW, kControlH)
                                    .text("闪烁")
                                    .fontSize(core::theme::scaled(12.0f))
                                    .color(t.colors.textSecondary)
                                    .verticalAlign(eui::VerticalAlign::Center);
                                components::toggleSwitch(inner, "term.set.row.cursor.blink.sw")
                                    .position(controlX + 136.0f + blinkLabelW + 8.0f, curCtlY)
                                    .size(44.0f, 24.0f)
                                    .checked(settings.cursorBlink)
                                    .theme(ct)
                                    .onChange([sp, persist](bool v) {
                                        sp->cursorBlink = v;
                                        persist();
                                    })
                                    .build();
                            });
                    })
                    .build();

                // 底部行（sticky：卡外固定，不随内滚 §6）：分隔线 + 恢复默认 + 完成
                const float footerY = cardY + cardH - kPanelPad - 32.0f;
                ui.rect("term.set.sep")
                    .position(cardX + kPanelPad, footerY - 12.0f - 1.0f)
                    .size(innerW, 1.0f)
                    .color(t.colors.borderMuted);
                const float resetW = textWidth("恢复默认", core::theme::scaled(13.0f)) + 24.0f;
                ui.rect("term.set.reset")
                    .position(cardX + kPanelPad, footerY)
                    .size(resetW, 32.0f)
                    .radius(6.0f)
                    .states(kTransparent, t.colors.hover, t.colors.hover)
                    .onClick([sp, persist]() {
                        *sp = TerminalSettings{};
                        persist();
                    });
                ui.text("term.set.reset.label")
                    .position(cardX + kPanelPad, footerY)
                    .size(resetW, 32.0f)
                    .text("恢复默认")
                    .fontSize(core::theme::scaled(13.0f))
                    .color(t.colors.textSecondary)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center);
                const float doneW = textWidth("完成", core::theme::scaled(14.0f)) + 40.0f;
                ui.rect("term.set.done")
                    .position(cardX + cardW - kPanelPad - doneW, footerY)
                    .size(doneW, 32.0f)
                    .radius(6.0f)
                    .states(doneBg, doneHover, doneHover)
                    .onClick([this]() { closeSettings(); });
                ui.text("term.set.done.label")
                    .position(cardX + cardW - kPanelPad - doneW, footerY)
                    .size(doneW, 32.0f)
                    .text("完成")
                    .fontSize(core::theme::scaled(14.0f))
                    .color(t.colors.textPrimary)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center);
            }
        });

    // TermPerf UI 侧 5s 汇总（bug248 临时诊断）
    {
        const auto composeT1 = std::chrono::steady_clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(composeT1 - composeT0).count();
        pf.composeMs += ms;
        pf.composeMaxMs = std::max(pf.composeMaxMs, ms);
        pf.frames++;
        if (!pf.hasLog) {
            pf.lastLog = composeT1;
            pf.hasLog = true;
        } else if (composeT1 - pf.lastLog >= std::chrono::seconds(5)) {
            char line[256];
            const double avgGap = pf.frames > 1 ? pf.gapMs / static_cast<double>(pf.frames - 1)
                                                : 0.0;
            std::snprintf(line, sizeof(line),
                          "TermPerf: composeMs=%.1f maxComposeMs=%.1f frames=%d "
                          "pollMs=%.1f maxPollMs=%.1f frameGapMs=%.1f/%.1f",
                          pf.composeMs, pf.composeMaxMs, pf.frames, pf.pollMs, pf.pollMaxMs,
                          avgGap, pf.gapMaxMs);
            LOG_INFO(line);
            pf.composeMs = 0.0;
            pf.pollMs = 0.0;
            pf.gapMs = 0.0;
            pf.composeMaxMs = 0.0;
            pf.pollMaxMs = 0.0;
            pf.gapMaxMs = 0.0;
            pf.frames = 0;
            pf.lastLog = composeT1;
        }
    }

    // Cmd+C 兜底路径：module 级 onKeyEvent 捕 super+C → onCopyText（见 module_terminal.cpp）
    return layout;
}

} // namespace terminal
