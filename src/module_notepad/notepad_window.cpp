// notepad_window.cpp — 记事本窗 compose + 窗口级键路由 + 窗内打开
//（need:273 批 1）。三段式版面（设计稿 §1.2）：工具条 44 / 提示条 32 条件 /
// 编辑区 flex / 状态栏 28。查找条 / 预览 / 字体下拉 / 格式化 = 批 2/3 面，
// 本批工具条只落「打开 / 保存」两钮（无新建钮 = N-14 硬约束）。
#include <chrono>
#include "notepad_session.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>  // std::move（工具条 onClick 闭包搬运；std include 守卫要求显式声明）
#include <vector>

#include <components/input.h>
#include <components/dialog.h>  // F-05 未保存保护三选弹窗（content() 缝三钮）
#include <components/markdown.h>  // F-13 预览整篇渲染（EUI 组件层合法依赖）
#include <components/mousearea.h>  // need345 预览拖选 hit 覆盖层（press/drag/release）
#include <components/scrollview.h>  // 字体下拉面板内滚（F-09，设计稿 §4.2 max-h 内滚）+ 预览纵滚
#include <components/theme.h>
#include <components/tooltip.h>  // F-11 格式化钮键序明示副文案（D-4 轻量明示）
#include <core/component_theme.h>
#include <core/input/input_types.h>
#include <core/render/text.h>  // TextPrimitive::systemFontFamilies（need266 已合入 EUI 指针）
#include <core/theme.h>
#include <core/window/window_backend.h>  // requestWindowClose（M+W）
#include <eui/dsl_app.h>                 // app::requestUpdate（定义在 EUI glfw TU）
#include <eui/platform.h>                // openFileDialog（窗内打开 F-02）

#include "core/logging.h"
#include "core/notepad_file_types.h"  // 打开对话框过滤器（单一真源）
#include "platform/json.h"  // F-11 parse/stringify/jsonLineColForPos（platform 不依赖 module）
#include "notepad_file_io.h"
#include "notepad_find.h"   // findAll/nextFrom/prevFrom/replaceOneAt/replaceAll/mergeRuns
#include "notepad_language.h"  // highlightLine/languageForExtension（语法层 provider）
#include "notepad_protect.h"  // 三选动作→后续动作映射（关窗/退出两语境共用）
#include "notepad_module.h"
#include "notepad_prefs.h"  // notepadSaveEditorPrefs（F-09/F-10 写穿透）
#include "notepad_sql_format.h"  // formatSqlText（need380 SQL 格式化引擎）
#include "ui_kit/light_toast.h"

namespace module::notepad {

namespace {

using InputState = components::input_detail::InputModel::InputState;

constexpr float kToolbarH = 44.0f;   // 2 + 40 + 2（设计稿 §4.2）
constexpr float kPromptH = 32.0f;    // 条件提示条（只读 / 大文件）
constexpr float kStatusH = 28.0f;    // 状态栏

// 工具条按钮（本批两枚文本钮；图标钮规格批 3 与字体下拉一起收敛到设计稿 §4.2）。
// active = 选中态高亮（F-19 换行钮 toggle 口径）：主色淡底 + 主色描边。
void toolbarButton(eui::Ui& ui, const char* id, const char* label, float x,
                   const core::theme::Tokens& t, bool enabled,
                   std::function<void()> onClick, bool active = false) {
    constexpr float kBtnW = 64.0f;
    constexpr float kBtnH = 28.0f;
    const std::string base(id);
    ui.stack(base)
        .position(x, 8.0f)
        .size(kBtnW, kBtnH)
        .disabled(!enabled)  // 真禁用（拦截点击）而非仅灰显；md 状态栏步进钮同款
        .content([&] {
            ui.rect(base + ".bg")
                .size(kBtnW, kBtnH)
                .color(active ? components::theme::withAlpha(t.colors.primary, 0.16f)
                              : (enabled ? t.colors.surface : t.colors.background))
                .radius(6.0f)
                .border(1.0f, !enabled ? t.colors.borderMuted
                                       : (active ? components::theme::withAlpha(t.colors.primary, 0.60f)
                                                 : t.colors.borderDefault))
                .build();
            ui.text(base + ".label")
                .position(6.0f, 0.0f)
                .size(kBtnW - 12.0f, kBtnH)
                .text(label)
                .fontSize(13.0f)
                .lineHeight(kBtnH)
                .verticalAlign(eui::VerticalAlign::Center)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(enabled ? t.colors.textPrimary : t.colors.textDisabled)
                .build();
        })
        .onClick(std::move(onClick))
        .build();
}

// 查找条小钮（24×24 图标钮 / 替换行文本钮共用；toolbarButton 同款视觉语言）。
void barButton(eui::Ui& ui, const char* id, const char* label, float x, float y,
               float w, float h, const core::theme::Tokens& t, bool enabled,
               std::function<void()> onClick) {
    const std::string base(id);
    ui.stack(base)
        .position(x, y)
        .size(w, h)
        .disabled(!enabled)
        .content([&] {
            ui.rect(base + ".bg")
                .size(w, h)
                .color(enabled ? t.colors.surface : t.colors.background)
                .radius(6.0f)
                .border(1.0f, enabled ? t.colors.borderDefault
                                      : t.colors.borderMuted)
                .build();
            ui.text(base + ".label")
                .position(0.0f, 0.0f)
                .size(w, h)
                .text(label)
                .fontSize(13.0f)
                .lineHeight(h)
                .verticalAlign(eui::VerticalAlign::Center)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(enabled ? t.colors.textPrimary : t.colors.textDisabled)
                .build();
        })
        .onClick(std::move(onClick))
        .build();
}

// ── 批 3：字体 / 字号 / 编码·行尾（F-09/F-10/F-15）──────────────────

// 字体下拉弹层开合态（每窗独立，Ui 树 retained；照抄 EUI dropdown
// DropdownOpenState 模式，不进 session——纯 UI 态与窗口同寿）。
struct FontPopupState {
    bool open = false;
    bool initialized = false;
    float scroll = 0.0f;
};

// 系统字体族枚举缓存（进程内一份；CoreText/DirectWrite 枚举有成本，禁每帧调）。
const std::vector<std::string>& notepadSystemFontFamilies() {
    static const std::vector<std::string> families =
        core::TextPrimitive::systemFontFamilies();
    return families;
}

// 脏判定单点：文本镜像 + 编码/行尾基线（F-15 点切即刻标脏——D-8 两档口径；
// 点切回翻到载入基线 = 回到干净态）。
void notepadRecomputeDirty(NotepadSession& s) {
    sessionSetDirty(s, s.editText != s.loadText || s.hasBom != s.loadHasBom ||
                          s.eolMode != s.loadEol);
}

// F-10 A±：theme 全局通道（D-1 联动笔记页字号）+ session + prefs 写穿透；
// 钳 [10,32]（PRD F-10），toast 反馈当前字号。
void notepadStepFontSize(NotepadSession& s, int delta) {
    int next = s.editorFontSize + delta;
    next = next < 10 ? 10 : (next > 32 ? 32 : next);
    if (next == s.editorFontSize) return;
    s.editorFontSize = next;
    core::theme::setEditorFontSize(static_cast<float>(next));
    notepadSaveEditorPrefs(s.fontFamily, s.wrapOn, s.editorFontSize);
    char buf[32];
    std::snprintf(buf, sizeof buf, "字号 %d", next);
    sessionToast(s, buf);
    app::requestUpdate();
}

// F-09 字体族选择（空 = 系统默认）+ prefs 写穿透；编辑 input 的
// fontFamily 参数下帧 compose 即时生效（InputBuilder 原生支持）。
void notepadSetFontFamily(NotepadSession& s, std::string family) {
    if (s.fontFamily == family) return;
    s.fontFamily = std::move(family);
    notepadSaveEditorPrefs(s.fontFamily, s.wrapOn, s.editorFontSize);
    app::requestUpdate();
}

// F-19 自动换行开关（notepad.btnWrap）：翻转 session.wrapOn + prefs 即时写
// 穿透；编辑 input 的 .softWrap 下帧 compose 即时生效（EUI softWrap 旋钮，
// clue #291 缺口已补）。大文件模式 load 侧已强制关 + 钮禁用，此处早退兜底。
void notepadToggleWrap(NotepadSession& s) {
    if (s.largeFile) return;
    s.wrapOn = !s.wrapOn;
    notepadSaveEditorPrefs(s.fontFamily, s.wrapOn, s.editorFontSize);
    app::requestUpdate();
}

// F-15 编码点切：UTF-8 ↔ UTF-8 BOM（hasBom 翻转即刻标脏；字节重编码
// 发生在保存期 notepadEncodeBuffer，与 BOM 语义一致）。
void notepadToggleBom(NotepadSession& s) {
    s.hasBom = !s.hasBom;
    notepadRecomputeDirty(s);
    app::requestUpdate();
}

// F-15 行尾点切：LF ↔ CRLF。缓冲恒 LF 域（行尾是保存期编码属性）：把 \r
// 注入编辑缓冲会被 input 输入过滤剥掉（input_model.h:319）、保存重编码还会
// 产出 \r\r\n——故与 BOM 同口径做元数据翻转 + 标脏，不做缓冲改写。
void notepadToggleEol(NotepadSession& s) {
    s.eolMode =
        s.eolMode == NotepadEol::Crlf ? NotepadEol::Lf : NotepadEol::Crlf;
    notepadRecomputeDirty(s);
    app::requestUpdate();
}

// 状态栏编码段标签（D-8 两档：UTF-8 / UTF-8 BOM；只读档显示推测/UTF-16）。
const char* notepadEncodingLabel(const NotepadSession& s) {
    if (s.enc.bom == NotepadBom::Utf8) {
        return s.hasBom ? "UTF-8 BOM" : "UTF-8";
    }
    if (s.enc.bom == NotepadBom::Utf16Le) return "UTF-16 LE";
    if (s.enc.bom == NotepadBom::Utf16Be) return "UTF-16 BE";
    if (!s.enc.validUtf8) {
        return s.enc.guessLabel != nullptr ? s.enc.guessLabel : "未知（推测）";
    }
    return "UTF-8";
}

// 状态栏行尾段标签（Mixed 显「混合」）。
const char* notepadEolLabel(const NotepadSession& s) {
    switch (s.eolMode) {
        case NotepadEol::Crlf:
            return "CRLF";
        case NotepadEol::Cr:
            return "CR";
        case NotepadEol::Mixed:
            return "混合";
        case NotepadEol::Lf:
            break;
    }
    return "LF";
}

// 触发器标签：码点安全截断（18 字节 ≈ 6 CJK / 18 Latin，160px 内不溢出）。
std::string notepadFontTriggerLabel(const std::string& family) {
    constexpr size_t kMaxBytes = 18;
    if (family.empty()) return "默认字体";
    if (family.size() <= kMaxBytes) return family;
    size_t cut = kMaxBytes;
    while (cut > 0 &&
           (static_cast<unsigned char>(family[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return family.substr(0, cut) + "…";
}

// 状态栏统计缓存（P2-5 走查节流，need289）：行:列/字符数每帧全量 3×全文扫描
// → (textRevision, cursor) 门控，键未变帧零扫描（10MB 只读窗每帧 3×10MB → 0）。
// 设计稿 §11.4「200ms 节流」的事件驱动等价实现：失效点=编辑/光标移动本身。
struct NotepadStatusStats {
    bool valid = false;
    unsigned long long textRev = 0;
    int cursor = 0;
    int line = 0;
    size_t colCp = 0;
    size_t chars = 0;
};

// 脏态「●未保存」x 估宽（P3-1 走查）：码点分档 CJK 12px / 其余 7px（12px
// 字号实测档）。原码点×7px 粗估对 CJK 文件名系统性偏小 → ● 叠上标题文字。
float notepadTitleEstWidth(const std::string& title) {
    float width = 0.0f;
    for (size_t i = 0; i < title.size();) {
        const unsigned char b = static_cast<unsigned char>(title[i]);
        const size_t len = b < 0x80                ? 1
                           : (b & 0xE0) == 0xC0    ? 2
                           : (b & 0xF0) == 0xE0    ? 3
                                                   : 4;
        width += len >= 3 ? 12.0f : 7.0f;  // 3/4 字节 UTF-8 ≈ CJK/emoji 宽档
        i += len;
    }
    return width + 20.0f;
}

// 命中层合成色（need377 改版）：命中词着**文字色**（styled-runs 是字形色，
// 原设计稿 §4.5 的 warning@22%/26% 半透明观感 = 老板口中的「淡粉色」）。
// 加深 = 提高不透明度到接近实色；当前命中再叠 primary（colorOver 预合成）。
// 独立常量单点（波末验收可调，勿散落复制）。
constexpr float kFindHitAlphaDark = 0.95f;    // 普通命中文字不透明度（暗）
constexpr float kFindHitAlphaLight = 0.92f;   // 普通命中文字不透明度（浅）
constexpr float kFindCurHitOverlayDark = 0.45f;   // 当前命中 primary 叠加强度（暗）
constexpr float kFindCurHitOverlayLight = 0.40f;  // 当前命中 primary 叠加强度（浅）
constexpr bool kFindHitBold = true;  // need377：命中词加粗（TextRun.bold faux-bold）

eui::Color colorOver(eui::Color bottom, eui::Color top) {
    const float outA = top.a + bottom.a * (1.0f - top.a);
    if (outA <= 0.0f) return eui::Color{0.0f, 0.0f, 0.0f, 0.0f};
    eui::Color out;
    out.r = (top.r * top.a + bottom.r * bottom.a * (1.0f - top.a)) / outA;
    out.g = (top.g * top.a + bottom.g * bottom.a * (1.0f - top.a)) / outA;
    out.b = (top.b * top.a + bottom.b * bottom.a * (1.0f - top.a)) / outA;
    out.a = outA;
    return out;
}

// ── 查找态 session 操作（键路由 / 查找条按钮共用；InputState 直写统一在 compose 消费）──

void notepadFindClose(NotepadSession& s) {
    NotepadSession::NotepadFindState& f = s.find;
    f.visible = false;
    f.pendingSeed = false;
    f.pendingJump = 0;
    f.hits.clear();
    f.cur = kFindNoHit;
    f.appliedQuery.clear();    // Esc 清高亮留词（F-06）；重开经门控强制重算
    f.styleDirty = true;       // 命中层缓存失效（lineRuns 缓存键 = textRevision|styleRevision）
    f.pendingFocusEditor = true;  // P2-3：关条焦点回归编辑器（开条播种对称）
}

void notepadFindToggle(NotepadSession& s) {
    if (s.find.visible) {
        notepadFindClose(s);
        return;
    }
    s.find.visible = true;
    s.find.pendingSeed = true;    // compose 播种输入框 + 全选 + 聚焦（再唤起保留词全选）
    s.find.appliedQuery.clear();  // 强制重算（重开即恢复高亮）
    s.find.pendingJump = 0;
    s.find.styleDirty = true;
    app::requestUpdate();
}

// M+H 替换条直开（PRD 流程 D3，need289 修复棒 1）：未开 = 开条（播种+聚焦+
// 重算，复用 F-06 全套）+ 展开替换行；已开 = 仅展开替换行（不重播种，
// 避免打断正在输入的查找词）。
void notepadFindOpenReplace(NotepadSession& s) {
    if (s.find.visible) {
        s.find.replaceOpen = true;
        app::requestUpdate();
        return;
    }
    notepadFindToggle(s);       // 开条 + 播种 + 聚焦 + 强制重算
    s.find.replaceOpen = true;  // D3：直开替换行
}

// 查找态每帧 tick（compose 编辑分支调用）：待跳转消费 + 输入即重算
//（appliedQuery 门控）+ 文本编辑重算（textRevision 门控，当前序号尽量邻近）。
void notepadFindTick(NotepadSession& s,
                     components::input_detail::InputModel::InputState& st) {
    NotepadSession::NotepadFindState& f = s.find;
    if (f.styleDirty) {
        f.styleDirty = false;
        ++st.styleRevision;
    }
    // 待跳转落地（键路由 / 查找输入 onKeyEvent 置位）：cursor 直写 +
    // followCaret=true 自动滚动（无需碰 scrollview）
    if (f.pendingJump != 0) {
        const int dir = f.pendingJump;
        f.pendingJump = 0;
        if (f.visible && !f.hits.empty()) {
            const size_t from =
                st.cursor >= 0 ? static_cast<size_t>(st.cursor) : 0;
            const size_t idx =
                dir > 0 ? nextFrom(f.hits, from) : prevFrom(f.hits, from);
            if (idx != kFindNoHit) {
                f.cur = idx;
                st.cursor = static_cast<int>(f.hits[idx].start);
                st.selectionStart = static_cast<int>(f.hits[idx].start);
                st.selectionEnd = static_cast<int>(f.hits[idx].end);
                st.hasPreferredCursorX = false;
                st.followCaret = true;
                ++st.styleRevision;  // 当前命中色变化 → 命中层缓存失效
            }
        }
    }
    if (!f.visible) return;
    const bool queryChanged = f.query != f.appliedQuery;
    // need400：文本域切换（预览显示域 ↔ 编辑源域）必须重算——hits 字节区
    // 间属于哪个域由 hitsPreview 标记（跨域复用会错位）
    const bool textChanged = f.hitsRevision != st.textRevision || f.hitsPreview;
    if (!queryChanged && !textChanged) return;
    f.appliedQuery = f.query;
    f.hitsRevision = st.textRevision;
    f.hitsPreview = false;
    if (f.appliedQuery.empty()) {
        // 空关键词清高亮（F-06）
        if (!f.hits.empty()) {
            f.hits.clear();
            f.cur = kFindNoHit;
            ++st.styleRevision;
        }
        return;
    }
    // 当前序号尽量邻近：锚字节之后首个命中，无则最后命中
    const size_t anchor =
        f.cur < f.hits.size()
            ? f.hits[f.cur].start
            : static_cast<size_t>(st.cursor < 0 ? 0 : st.cursor);
    f.hits = findAll(st.text, f.appliedQuery,
                     FindOptions{f.caseSensitive, f.wholeWord});
    f.cur = kFindNoHit;
    for (size_t i = 0; i < f.hits.size(); ++i) {
        if (f.hits[i].start >= anchor) {
            f.cur = i;
            break;
        }
    }
    if (f.cur == kFindNoHit && !f.hits.empty()) f.cur = f.hits.size() - 1;
    ++st.styleRevision;  // 命中层缓存失效
}

// need400：预览态查找显示文本缓存（域 = displayTextOf，textRevision 键控；
// 预览禁编辑 → 修订稳定，查找条开着时每帧复用同一显示文本）。
const std::string& notepadFindPreviewDisplay(NotepadSession& s,
                                             unsigned long long rev) {
    if (s.findPreviewRev != rev) {
        s.findPreviewDisplay =
            components::MarkdownBuilder::displayTextOf(s.editText);
        s.findPreviewRev = rev;
    }
    return s.findPreviewDisplay;
}

// 预览态查找 tick（compose 预览分支调用，need400）：命中域 = 显示文本（用户
// 所见即所搜）；跳转只请求预览滚动（pendingPreviewScroll 由 drawPreviewSlot
// 换算矩形落点），InputState 光标/选区不动——预览禁编辑。重算门控与编辑态
// notepadFindTick 同构（输入即重算 + 文本修订重算），域切换由 hitsPreview 标记
// 强制重算。
void notepadPreviewFindTick(NotepadSession& s,
                            components::input_detail::InputModel::InputState& st) {
    NotepadSession::NotepadFindState& f = s.find;
    if (f.styleDirty) {
        f.styleDirty = false;
        ++st.styleRevision;
    }
    if (!f.visible) return;
    // 待跳转：相邻命中（环绕语义与编辑态一致；无当前命中 → 首/末命中）
    if (f.pendingJump != 0) {
        const int dir = f.pendingJump;
        f.pendingJump = 0;
        if (!f.hits.empty()) {
            size_t idx = kFindNoHit;
            if (f.cur >= f.hits.size()) {
                idx = dir > 0 ? 0 : f.hits.size() - 1;
            } else {
                const size_t from = f.hits[f.cur].start;
                idx = dir > 0 ? nextFrom(f.hits, from) : prevFrom(f.hits, from);
            }
            if (idx != kFindNoHit) {
                f.cur = idx;
                f.pendingPreviewScroll = true;
                ++st.styleRevision;  // 当前命中色变化（切回编辑时缓存同步失效）
            }
        }
    }
    const bool queryChanged = f.query != f.appliedQuery;
    const bool domainChanged = !f.hitsPreview || f.hitsRevision != st.textRevision;
    if (!queryChanged && !domainChanged) return;
    const std::string& display = notepadFindPreviewDisplay(s, st.textRevision);
    f.appliedQuery = f.query;
    f.hitsRevision = st.textRevision;
    f.hitsPreview = true;
    if (f.appliedQuery.empty()) {
        // 空关键词清高亮（F-06）
        if (!f.hits.empty()) {
            f.hits.clear();
            f.cur = kFindNoHit;
            ++st.styleRevision;
        }
        return;
    }
    // 当前序号尽量邻近（与编辑态同口径：锚之后首个命中，无则末命中）
    const size_t anchor = f.cur < f.hits.size() ? f.hits[f.cur].start : 0;
    f.hits = findAll(display, f.appliedQuery,
                     FindOptions{f.caseSensitive, f.wholeWord});
    f.cur = kFindNoHit;
    for (size_t i = 0; i < f.hits.size(); ++i) {
        if (f.hits[i].start >= anchor) {
            f.cur = i;
            break;
        }
    }
    if (f.cur == kFindNoHit && !f.hits.empty()) f.cur = f.hits.size() - 1;
    ++st.styleRevision;  // 命中层缓存失效
}

// 统一文本提交 helper（tech 卡 §四，md_module_editor.cpp:140-142 同构）：
// 整批一次 pushUndoState = 单撤销单元（F-08）；镜像 + 脏态单点 + 即时刷新。
void notepadCommitText(NotepadSession& s,
                       components::input_detail::InputModel::InputState& st,
                       const std::string& text, int cursor, int selStart,
                       int selEnd) {
    components::input_detail::InputModel::pushUndoState(st);
    st.text = text;
    ++st.textRevision;
    st.cursor = cursor;
    st.selectionStart = selStart;
    st.selectionEnd = selEnd;
    st.hasPreferredCursorX = false;
    st.followCaret = true;  // 替换后自动滚到落点
    s.editText = text;
    notepadRecomputeDirty(s);  // 编码/行尾点切脏态不因文本提交回翻被吞（F-15）
    app::requestUpdate();
}

void notepadReplaceOne(NotepadSession& s,
                       components::input_detail::InputModel::InputState& st) {
    NotepadSession::NotepadFindState& f = s.find;
    if (f.cur >= f.hits.size()) return;  // 无当前命中 → 零动作
    const FindHit hit = f.hits[f.cur];
    const ReplaceOneResult r =
        replaceOneAt(st.text, hit, f.replaceWith, f.appliedQuery,
                     FindOptions{f.caseSensitive, f.wholeWord});
    notepadCommitText(s, st, r.text, static_cast<int>(r.cursor),
                      static_cast<int>(r.selStart), static_cast<int>(r.selEnd));
}

void notepadReplaceAll(NotepadSession& s,
                       components::input_detail::InputModel::InputState& st) {
    NotepadSession::NotepadFindState& f = s.find;
    const ReplaceAllResult r =
        replaceAll(st.text, f.appliedQuery, f.replaceWith,
                   FindOptions{f.caseSensitive, f.wholeWord});
    if (r.count == 0) {
        sessionToast(s, "已替换 0 处");  // 含 0 处明示（F-08）
        app::requestUpdate();
        return;
    }
    const int cursor = static_cast<int>(
        std::min<size_t>(st.cursor >= 0 ? static_cast<size_t>(st.cursor) : 0,
                         r.text.size()));
    notepadCommitText(s, st, r.text, cursor, cursor, cursor);
    char buf[48];
    std::snprintf(buf, sizeof buf, "已替换 %zu 处", r.count);
    sessionToast(s, buf);
}

// F-11 JSON 格式化：parse→stringify(pretty 两空格)→notepadCommitText 同款
// 单撤销提交。失败 = toast「第 X 行 第 Y 列：<msg>」+ 光标跳错误位（内容
// 不变）；键序重排（json.cpp 对象容器 std::map，clue #252/#253）按 D-4 裁决
// 走轻量明示（钮 tooltip 固定副文案 + 成功 toast 后缀），不弹确认。
void notepadFormatJson(NotepadSession& s,
                       components::input_detail::InputModel::InputState& st) {
    platform::json::ParseDiag diag;
    const core::Result<platform::json::JsonValue> parsed =
        platform::json::parse(st.text, &diag);
    if (!parsed.isOk()) {
        const platform::json::JsonLineCol lc =
            platform::json::jsonLineColForPos(st.text, diag.bytePos);
        char buf[192];
        std::snprintf(buf, sizeof buf, "第 %zu 行 第 %zu 列：%s", lc.line,
                      lc.col, diag.msg.c_str());
        sessionToast(s, buf);
        // 光标跳错误位：钳界 + followCaret 自动滚到光标；内容不变
        const int target = static_cast<int>(
            std::min<size_t>(diag.bytePos, st.text.size()));
        st.cursor = target;
        st.selectionStart = target;
        st.selectionEnd = target;
        st.hasPreferredCursorX = false;
        st.followCaret = true;
        app::requestUpdate();
        return;
    }
    const core::Result<std::string> pretty =
        platform::json::stringify(parsed.value(), /*pretty=*/true);
    if (!pretty.isOk()) {
        sessionToast(s, "JSON 格式化失败：序列化失败");
        app::requestUpdate();
        return;
    }
    // 格式化改写整篇字节布局，原光标偏移失义 → 落文档头（单撤销一步）
    notepadCommitText(s, st, pretty.value(), 0, 0, 0);
    sessionToast(s, "JSON 已格式化（对象键已按字典序重排）");
}

// need380 SQL 格式化（最小范围：关键字规范化 + 缩进）：formatSqlText 纯函数
// 引擎 + notepadCommitText 同款单撤销提交（与 JSON 格式化同构）。格式化改写
// 整篇布局 → 光标落文档头；输出与原文相同 = 不入撤销栈只 toast 明示。
void notepadFormatSql(NotepadSession& s,
                      components::input_detail::InputModel::InputState& st) {
    const std::string formatted = formatSqlText(st.text);
    if (formatted == st.text) {
        sessionToast(s, "SQL 已是规范格式，无需格式化");
        app::requestUpdate();
        return;
    }
    notepadCommitText(s, st, formatted, 0, 0, 0);
    sessionToast(s, "SQL 已格式化（关键字大写 + 缩进重排）");
}

// F-13 预览切换：整页编辑⇄预览替换编辑区槽位；InputState 与 session 镜像
// 不动 → 切回光标/编辑滚动保留。
void notepadTogglePreview(NotepadSession& s) {    s.previewOn = !s.previewOn;
    app::requestUpdate();
}

// 预览切换键路由入口（M+P 设计稿 §6 / M+Shift+V PRD §九，need289 修复棒 1）：
// guard = .md/.markdown 且非大文件，与钮面 matrix.previewEnabled 同口径。
void notepadTogglePreviewIfAvailable(NotepadSession& s) {
    const NotepadToolbarMatrix m = notepadToolbarMatrixFor(s);
    if (!m.showPreview || !m.previewEnabled) return;
    notepadTogglePreview(s);
}

// ── md 预览文本选区（need345）：拖选三件 + 自动复制（整篇显示码点域）──
// 语义平移自退役查看器（eebed0b9 池 #264：拖选自动复制、剪贴板回读校验、
// Ctrl+C 兼容通道）；域差异（整篇扁平码点域 vs 旧逐块域）见 notepad_session.h
// 选区字段注释。toast = sessionToast（compose 尾 drawLightToastPill 单源消费，
// ui_kit 轻提示只调用）。

// 码点 → 字节偏移（显示文本切片）：lead byte 步进 + 钳位串尾（旧查看器
// viewerSelCpToByte 同式；码点域切片天然不产 CJK/emoji 半字）。
size_t notepadSelCpToByte(const std::string& text, size_t cp) {
    size_t i = 0;
    for (size_t done = 0; done < cp && i < text.size(); ++done) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        i += std::min<size_t>(c >= 0xF0U ? 4 : c >= 0xE0U ? 3 : c >= 0xC0U ? 2 : 1,
                              text.size() - i);
    }
    return i;
}

// 归一化阅读序 [cp0, cp1)；空选区（锚=焦）返回 false。
bool notepadSelNormalized(const NotepadSession& s, size_t* cp0, size_t* cp1) {
    if (s.selAnchorCp == s.selFocusCp) return false;
    if (s.selAnchorCp < s.selFocusCp) {
        *cp0 = s.selAnchorCp;
        *cp1 = s.selFocusCp;
    } else {
        *cp0 = s.selFocusCp;
        *cp1 = s.selAnchorCp;
    }
    return true;
}

// 指针事件（hit 层内容系局部坐标 = 整篇内容系）→ 整篇显示码点：渲染同源
// displayCpForPoint 反查（x 钳渲染宽、y 钳非负——旧查看器 localX 同式）。
size_t notepadSelHitCp(NotepadSession& s, const components::MouseEvent& ev) {
    const auto hit = components::MarkdownBuilder::displayCpForPoint(
        s.editText, s.selPreviewWidth, s.selPreviewStyle,
        std::clamp(ev.x, 0.0f, std::max(0.0f, s.selPreviewWidth)),
        std::max(0.0f, ev.y));
    return hit.ok ? hit.cp : 0;
}

// 写剪贴板 + 回读校验 + 成功 toast（need345）。回读校验 = 旧查看器
// viewerCopyToClipboard 同语义：setClipboardText 无返回值，写后经 clipboardText
// 比对，失配 LOG_WARN（agent 模块复制先例同源；成功面不打日志降噪）。
void notepadCopySel(NotepadSession& s, const std::string& text) {
    if (text.empty()) return;
    core::window::setClipboardText(text);
    if (s.handle != nullptr &&
        core::window::clipboardText(static_cast<core::window::Handle>(s.handle)) != text) {
        LOG_WARN("notepad md 预览: 剪贴板写入回读校验失败（期望 " +
                 std::to_string(text.size()) + " 字节，选区自动复制可能未生效）");
    }
    char buf[48];
    std::snprintf(buf, sizeof buf, "已复制 %zu 字", notepadUtf8Length(text));
    sessionToast(s, buf);
    app::requestUpdate();
}

// 按下：清旧选区并落锚点（简单点击 = 空选区，抬起清零不写剪贴板）。
void notepadSelPress(NotepadSession& s, const components::MouseEvent& ev) {
    s.selDragging = true;
    s.selHasText = false;
    s.selAnchorCp = s.selFocusCp = notepadSelHitCp(s, ev);
    app::requestUpdate();
}

// 拖动：焦点实时跟随（非空即实时高亮；矩形层 drawPreviewSlot 每帧按选区态画）。
void notepadSelDrag(NotepadSession& s, const components::MouseEvent& ev) {
    if (!s.selDragging) return;
    s.selFocusCp = notepadSelHitCp(s, ev);
    s.selHasText = s.selAnchorCp != s.selFocusCp;
    app::requestUpdate();
}

// 抬起：非空选区 = 保留高亮 + 自动复制（选中即复制，老板 2026-11-11 口径）；
// 空选择只清零（不写剪贴板）。
void notepadSelRelease(NotepadSession& s, const components::MouseEvent& ev) {
    if (!s.selDragging) return;  // 无在途按压的释放（异常时序）不动作
    s.selDragging = false;
    s.selFocusCp = notepadSelHitCp(s, ev);
    s.selHasText = s.selAnchorCp != s.selFocusCp;
    if (s.selHasText) {
        notepadCopySel(s, notepadPreviewSelText(s));
    }
    app::requestUpdate();
}

// 编辑器 per-line provider（语法层 ∪ 命中层；3 参 lineStyles 契约 + 4 参
// highlightLine 经本函数桥接，session.language 在此绑定）。
std::vector<eui::TextRun> notepadEditorLineRuns(
    NotepadSession& s, const std::string& displayText, int lineStartByte,
    int lineNo, const eui::Color& hitBg, const eui::Color& curBg) {
    const size_t lineStart =
        lineStartByte > 0 ? static_cast<size_t>(lineStartByte) : 0;
    std::vector<eui::TextRun> base =
        highlightLine(displayText, lineStart, lineNo, s.language);
    // 命中标记：大文件同步关（F-16 v1 取舍——仅保留查找跳转+计数）；条关无命中层
    if (s.largeFile || !s.find.visible || s.find.hits.empty()) return base;
    const FindHit* current =
        s.find.cur < s.find.hits.size() ? &s.find.hits[s.find.cur] : nullptr;
    return mergeRuns(base, s.find.hits, current, displayText, lineStart, hitBg,
                     curBg, kFindHitBold);
}

// 查找条浮层（设计稿 §4.5：锚编辑区右上 top 12 / right 16，surfaceElevated +
// 1px borderDefault + r10 + 内边距 6；行 1 = 输入/计数/↑/↓/替换展开/×，
// 行 2 展开 = 替换输入/替换/全部替换）。
void drawSearchBar(NotepadSession& s, eui::Ui& ui, float w, float editorTop,
                   const core::theme::Tokens& t,
                   const components::theme::ThemeColorTokens& componentTheme,
                   bool editable) {
    using InputState = components::input_detail::InputModel::InputState;
    NotepadSession::NotepadFindState& f = s.find;
    const float barW = 416.0f;
    const float barH = f.replaceOpen ? 74.0f : 40.0f;
    const float bx = w - 16.0f - barW;
    const float by = editorTop + 12.0f;
    // need400：预览态只读——替换面（展开/替换/全部替换）禁用，查找面照常
    const bool canReplace = editable && !s.previewOn;

    ui.rect(kNotepadSearchBarId)
        .position(bx, by)
        .size(barW, barH)
        .color(t.colors.surface)
        .radius(10.0f)
        .border(1.0f, t.colors.borderDefault)
        .build();

    // 开条首帧播种：保留词回填 + 全选 + 聚焦（设计稿「再唤起保留词全选」）
    if (f.pendingSeed) {
        f.pendingSeed = false;
        InputState& qst = ui.state<InputState>(kNotepadSearchQueryInputId);
        qst.text = f.query;
        ++qst.textRevision;
        qst.cursor = static_cast<int>(f.query.size());
        qst.selectionStart = 0;
        qst.selectionEnd = static_cast<int>(f.query.size());
        // need376：聚焦目标 = input 组件可聚焦元素 <id>.hit（requestFocus 按
        // 元素 id 解析，传组件根 id 静默落空 → 开条后仍须手点输入框）
        ui.requestFocus(std::string(kNotepadSearchQueryInputId) + ".hit");
    }

    // 行 1：查询输入（Enter/Shift+Enter/Esc 经 passthrough 先于内置处理消费）
    components::input(ui, kNotepadSearchQueryInputId)
        .position(bx + 6.0f, by + 6.0f)
        .size(220.0f, 28.0f)
        .value(f.query)
        .placeholder("查找")
        .fontSize(13.0f)
        .inset(6.0f)
        .theme(componentTheme)
        .onChange([&s](const std::string& v) {
            s.find.query = v;  // 输入即重算：经 appliedQuery 门控下帧生效
            app::requestUpdate();
        })
        .onKeyEvent([&s](const core::KeyEvent& ev) {
            if (!ev.isDown()) return false;
            if (ev.key == core::InputKey::Enter) {
                // 单调跳转（环绕）：Shift=上一个；置位后 compose 消费
                s.find.pendingJump = ev.modifiers.shift ? -1 : 1;
                app::requestUpdate();
                return true;
            }
            if (ev.key == core::InputKey::Escape) {
                notepadFindClose(s);  // 清高亮留词（F-06）
                app::requestUpdate();
                return true;
            }
            return false;
        })
        .build();

    // 行 1：计数 n/m（无命中 0/0＋「无匹配」textTertiary）
    char countBuf[40];
    if (f.hits.empty()) {
        std::snprintf(countBuf, sizeof countBuf, "0/0 无匹配");
    } else {
        std::snprintf(countBuf, sizeof countBuf, "%zu/%zu",
                      f.cur < f.hits.size() ? f.cur + 1 : 0, f.hits.size());
    }
    ui.text(kNotepadSearchCountId)
        .position(bx + 234.0f, by + 6.0f)
        .size(56.0f, 28.0f)
        .text(countBuf)
        .fontSize(12.0f)
        .lineHeight(28.0f)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(f.hits.empty() ? t.colors.textTertiary : t.colors.textSecondary)
        .build();

    // 行 1：↑ / ↓ / 替换展开 / ×
    barButton(ui, kNotepadSearchPrevId, "↑", bx + 298.0f, by + 8.0f, 24.0f,
              24.0f, t, true, [&s] {
                  s.find.pendingJump = -1;
                  app::requestUpdate();
              });
    barButton(ui, kNotepadSearchNextId, "↓", bx + 326.0f, by + 8.0f, 24.0f,
              24.0f, t, true, [&s] {
                  s.find.pendingJump = 1;
                  app::requestUpdate();
              });
    barButton(ui, kNotepadSearchReplaceToggleId, f.replaceOpen ? "−" : "＋",
              bx + 358.0f, by + 8.0f, 24.0f, 24.0f, t, canReplace, [&s] {
                  s.find.replaceOpen = !s.find.replaceOpen;
                  app::requestUpdate();
              });
    barButton(ui, kNotepadSearchCloseId, "×", bx + 386.0f, by + 8.0f, 24.0f,
              24.0f, t, true, [&s] {
                  notepadFindClose(s);
                  app::requestUpdate();
              });

    // 行 2：替换输入 / 替换 / 全部替换（整批一次 push = 单撤销 F-08）
    if (f.replaceOpen) {
        components::input(ui, kNotepadSearchReplaceInputId)
            .position(bx + 6.0f, by + 40.0f)
            .size(220.0f, 28.0f)
            .value(f.replaceWith)
            .placeholder("替换为")
            .fontSize(13.0f)
            .inset(6.0f)
            .theme(componentTheme)
            .onChange([&s](const std::string& v) {
                s.find.replaceWith = v;
                app::requestUpdate();
            })
            .build();
        barButton(ui, kNotepadBtnReplaceOneId, "替换", bx + 234.0f, by + 42.0f,
                  64.0f, 24.0f, t, canReplace, [&s, &ui] {
                      notepadReplaceOne(
                          s, ui.state<InputState>(kNotepadEditorInputId));
                  });
        barButton(ui, kNotepadBtnReplaceAllId, "全部替换", bx + 302.0f,
                  by + 42.0f, 88.0f, 24.0f, t, canReplace, [&s, &ui] {
                      notepadReplaceAll(
                          s, ui.state<InputState>(kNotepadEditorInputId));
                  });
    }
}

// 弹层列表项（选中 ✓ primary；点击选中 → prefs 写穿透 + 收起）。
void drawFontPopupItem(NotepadSession& s, eui::Ui& inner,
                       const std::string& base, int index, const char* name,
                       float w, const core::theme::Tokens& t,
                       FontPopupState& ps) {
    const bool selected =
        index < 0 ? s.fontFamily.empty() : s.fontFamily == name;
    const std::string id = base + ".popup.item." + std::to_string(index);
    inner.stack(id)
        .size(w, 26.0f)
        .content([&] {
            inner.rect(id + ".bg")
                .size(w, 26.0f)
                .states(eui::Color{0.0f, 0.0f, 0.0f, 0.0f}, t.colors.hover,
                        t.colors.hover)
                .radius(4.0f)
                .instantStates()
                .onClick([&s, &ps, index] {
                    // 空族 = 系统默认（首项）；其余按枚举名原样落 prefs
                    notepadSetFontFamily(
                        s, index < 0
                               ? std::string()
                               : notepadSystemFontFamilies()[static_cast<size_t>(
                                     index)]);
                    ps.open = false;
                    app::requestUpdate();
                })
                .build();
            inner.text(id + ".label")
                .position(8.0f, 0.0f)
                .size(w - 30.0f, 26.0f)
                .text(name)
                .fontSize(13.0f)
                .lineHeight(26.0f)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(selected ? t.colors.primary : t.colors.textPrimary)
                .build();
            if (selected) {
                inner.text(id + ".check")
                    .position(w - 24.0f, 0.0f)
                    .size(18.0f, 26.0f)
                    .text("✓")
                    .fontSize(12.0f)
                    .lineHeight(26.0f)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .color(t.colors.primary)
                    .build();
            }
        })
        .build();
}

// F-09 字体下拉触发器（设计稿 §4.2 ddFont 规格：h28 r6 surface 底 1px 边框
// 13px + chevron；x 由调用方游标给定（need374 宽度自适应）。预览态禁用（§4.6 字体禁用）。
void drawFontTrigger(NotepadSession& s, eui::Ui& ui,
                     const core::theme::Tokens& t, bool enabled, float x) {
    const std::string base(kNotepadDdFontId);
    constexpr float kW = 160.0f;
    constexpr float kH = 28.0f;
    FontPopupState& ps = ui.state<FontPopupState>(base + ".state");
    if (!ps.initialized) {
        ps.initialized = true;
        ps.open = false;
        ps.scroll = 0.0f;
    }
    ui.stack(base)
        .position(x, 8.0f)
        .size(kW, kH)
        .disabled(!enabled)
        .content([&] {
            ui.rect(base + ".bg")
                .size(kW, kH)
                .color(t.colors.surface)
                .radius(6.0f)
                .border(1.0f, t.colors.borderDefault)
                .build();
            ui.text(base + ".label")
                .position(8.0f, 0.0f)
                .size(kW - 26.0f, kH)
                .text(notepadFontTriggerLabel(s.fontFamily))
                .fontSize(13.0f)
                .lineHeight(kH)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(enabled ? t.colors.textPrimary : t.colors.textDisabled)
                .build();
            ui.text(base + ".chevron")
                .position(kW - 22.0f, 0.0f)
                .size(18.0f, kH)
                .text(ps.open ? "▴" : "▾")
                .fontSize(10.0f)
                .lineHeight(kH)
                .verticalAlign(eui::VerticalAlign::Center)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(t.colors.textTertiary)
                .build();
        })
        .onClick([&ps] {
            ps.open = !ps.open;
            app::requestUpdate();
        })
        .build();
}

// F-09 弹层面板（260×300 触发器下方 6px；内滚 = components::scrollView，
// 首项「默认字体」= 空族复位；命中遮罩 blockPointer 防穿透编辑器）。
// P2-4（need289 走查）：开时先铺全窗透明点击层（点击弹层外任意处收起不改值，
// 设计稿 §5.2）+ Esc 收起请求位消费（键路由 Ui 态不可达 → session 置位）。
void drawFontPopup(NotepadSession& s, eui::Ui& ui,
                   const core::theme::Tokens& t, float w, float h,
                   float triggerX) {
    const std::string base(kNotepadDdFontId);
    FontPopupState& ps = ui.state<FontPopupState>(base + ".state");
    if (s.pendingPopupDismiss) {  // Esc 收起请求（键路由置位，本处单点消费）
        s.pendingPopupDismiss = false;
        if (ps.open) {
            ps.open = false;
            app::requestUpdate();
        }
    }
    if (!ps.open) return;
    if (s.previewOn || !s.loadError.empty()) {  // 预览/错误态字体禁用：残留弹层强制收起
        ps.open = false;
        return;
    }
    const std::vector<std::string>& families = notepadSystemFontFamilies();
    constexpr float kW = 260.0f;
    constexpr float kH = 300.0f;
    constexpr float kPad = 6.0f;
    constexpr float kItemH = 26.0f;
    // 全窗遮罩点击层（P2-4）：透明可点，点击 = 收起不改值；弹层面板画在其上
    //（后画者在上）且面板自身 hit rect blockPointer，面板内点击不透到本层。
    ui.rect(base + ".popup.scrim")
        .position(0.0f, 0.0f)
        .size(w, h)
        .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([&ui] {
            ui.state<FontPopupState>(std::string(kNotepadDdFontId) + ".state")
                .open = false;
            app::requestUpdate();
        })
        .build();
    ui.stack(base + ".popup")
        .position(triggerX, 8.0f + 28.0f + 6.0f)  // need374：随触发器游标位
        .size(kW, kH)
        .content([&] {
            ui.rect(base + ".popup.bg")
                .size(kW, kH)
                .color(t.colors.surface)
                .radius(10.0f)
                .border(1.0f, t.colors.borderDefault)
                .build();
            ui.rect(base + ".popup.hit")
                .size(kW, kH)
                .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                .blockPointer()
                .build();
            components::scrollView(ui, base + ".popup.list")
                .position(kPad, kPad)
                .size(kW - kPad * 2.0f, kH - kPad * 2.0f)
                .gap(0.0f)
                .step(kItemH)
                .theme(core::theme::componentTokensWeChat(t))
                .offset(ps.scroll)
                .onChange([&ps](float v) { ps.scroll = v; })
                .content([&](eui::Ui& inner, float innerW, float) {
                    drawFontPopupItem(s, inner, base, -1, "默认字体", innerW,
                                      t, ps);
                    for (size_t i = 0; i < families.size(); ++i) {
                        drawFontPopupItem(s, inner, base, static_cast<int>(i),
                                          families[i].c_str(), innerW, t, ps);
                    }
                })
                .build();
        })
        .build();
}

// ── debt398 分段测高（预览总高异步化）──────────────────────────────
// 段 = 连续块区间（不切表格行组）；首帧只出近似总高，可见带块由 markdown 组件
// 自行精测（它出内容本来就要测），屏外未测段只建包装几何（blockHeightHint），
// 每帧预算补测 → 总高/滚动条渐进收敛（老板令：先出画，1 秒内刷出滚动条）。
constexpr std::size_t kPreviewSegBlocks = 12;  // 目标每段块数 = 补测步长
constexpr double kPreviewSegBudgetMs = 12.0;   // 单帧补测预算（<16ms 帧预算口径）

float previewSegWidthKey(float innerW) {
    return std::round(innerW * 2.0f) * 0.5f;
}

std::string previewSegStyleKey(const components::MarkdownStyle& style) {
    return core::theme::currentThemeName() + "|" + std::to_string(core::theme::fontScale()) +
           "|" + std::to_string(style.bodySize) + "|" + std::to_string(style.blockGap);
}

NotepadSession::PreviewSegTable* previewSegFind(NotepadSession& s, unsigned long long rev,
                                                float widthKey,
                                                const std::string& styleKey) {
    for (auto& table : s.previewSegTables) {
        if (table.usable && table.rev == rev && std::fabs(table.width - widthKey) < 0.6f &&
            table.styleKey == styleKey) {
            return &table;
        }
    }
    return nullptr;
}

std::size_t previewSegIndexOf(const NotepadSession::PreviewSegTable& table,
                              std::size_t block) {
    if (table.bounds.size() < 2) return 0;
    const auto it = std::upper_bound(table.bounds.begin(), table.bounds.end(), block);
    if (it == table.bounds.begin()) return 0;
    return static_cast<std::size_t>((it - table.bounds.begin()) - 1);
}

// 未测段块近似高 = 段高均摊（去掉段内 gap 后按块数均分）；<1px 兜底。
float previewSegBlockApprox(const NotepadSession::PreviewSegTable& table, std::size_t seg,
                            const components::MarkdownStyle& style) {
    if (seg + 1 >= table.bounds.size()) return 0.0f;
    const std::size_t count = table.bounds[seg + 1] - table.bounds[seg];
    if (count == 0) return 0.0f;
    const float gaps = style.blockGap * static_cast<float>(count - 1);
    return std::max(1.0f, (table.heights[seg] - gaps) / static_cast<float>(count));
}

// 段表初始化（不测量）：段高优先复用同 rev 旧宽度段表（按宽度比缩放 = 切宽度
// 旧段复用），无旧表则粗估（块数 × 正文行高 + 段内 gap）。
NotepadSession::PreviewSegTable& previewSegInit(NotepadSession& s, unsigned long long rev,
                                                float widthKey, const std::string& styleKey,
                                                const components::MarkdownStyle& style) {
    auto& slot = s.previewSegTables[static_cast<std::size_t>(s.previewSegNext) %
                                    s.previewSegTables.size()];
    s.previewSegNext = (s.previewSegNext + 1) % static_cast<int>(s.previewSegTables.size());
    slot = NotepadSession::PreviewSegTable{};
    slot.rev = rev;
    slot.width = widthKey;
    slot.styleKey = styleKey;
    slot.bounds =
        components::MarkdownBuilder::blockSegmentBoundaries(s.editText, kPreviewSegBlocks);

    const NotepadSession::PreviewSegTable* source = nullptr;
    float scale = 1.0f;
    for (const auto& table : s.previewSegTables) {
        if (&table == &slot || !table.usable || table.rev != rev) continue;
        if (table.bounds.size() != slot.bounds.size()) continue;
        source = &table;
        scale = (table.width > 1.0f && widthKey > 1.0f) ? table.width / widthKey : 1.0f;
        break;
    }
    const std::size_t segCount = slot.bounds.size() > 1 ? slot.bounds.size() - 1 : 0;
    slot.heights.assign(segCount, 0.0f);
    slot.measured.assign(segCount, 0);
    float total = 0.0f;
    for (std::size_t i = 0; i < segCount; ++i) {
        const std::size_t count = slot.bounds[i + 1] - slot.bounds[i];
        float h = 0.0f;
        if (source != nullptr) {
            h = source->heights[i] * scale;
        } else {
            h = static_cast<float>(count) * style.bodyLineHeight +
                style.blockGap * static_cast<float>(count > 0 ? count - 1 : 0);
        }
        slot.heights[i] = h;
        total += h;
    }
    if (segCount > 1) total += style.blockGap * static_cast<float>(segCount - 1);
    slot.total = total;
    slot.pending = static_cast<int>(segCount);
    slot.usable = true;
    return slot;
}

void previewSegRecount(NotepadSession::PreviewSegTable& table,
                       const components::MarkdownStyle& style) {
    float total = 0.0f;
    for (const float h : table.heights) total += h;
    if (table.heights.size() > 1) {
        total += style.blockGap * static_cast<float>(table.heights.size() - 1);
    }
    table.total = total;
}

// 可见带段范围：按当前段高累加（未测段用近似高，误差随补测收敛）。
void previewSegBand(const NotepadSession::PreviewSegTable& table, float scroll,
                    float viewportH, const components::MarkdownStyle& style,
                    std::size_t* lo, std::size_t* hi) {
    *lo = 0;
    *hi = 0;
    const std::size_t segCount = table.heights.size();
    if (segCount == 0) return;
    float y = 0.0f;
    bool loSet = false;
    for (std::size_t i = 0; i < segCount; ++i) {
        const float top = y;
        const float bottom = y + table.heights[i];
        if (!loSet && bottom >= scroll) {
            *lo = i;
            loSet = true;
        }
        if (top <= scroll + viewportH) *hi = i;
        y = bottom + style.blockGap;
    }
    if (!loSet) *lo = segCount - 1;
    if (*hi < *lo) *hi = *lo;
}

// 单段精测（区间高与 estimateHeight 同可加口径，见 EUI 缝注）。
void previewSegMeasure(NotepadSession& s, NotepadSession::PreviewSegTable& table,
                       std::size_t seg, const components::MarkdownStyle& style) {
    if (seg >= table.heights.size() || table.measured[seg]) return;
    table.heights[seg] = components::MarkdownBuilder::estimateHeightRange(
        s.editText, table.bounds[seg], table.bounds[seg + 1], table.width, style);
    table.measured[seg] = 1;
    if (table.pending > 0) --table.pending;
    previewSegRecount(table, style);
}

// 收敛回写：精确总高写 previewDocHs（稳态 provider 快路径，键口径同旧缓存）。
void previewSegNoteConverged(NotepadSession& s, NotepadSession::PreviewSegTable& table) {
    if (table.pending > 0 || table.converged) return;
    table.converged = true;
    auto& slot = s.previewDocHs[static_cast<std::size_t>(s.previewDocHNext) %
                                s.previewDocHs.size()];
    s.previewDocHNext = (s.previewDocHNext + 1) % static_cast<int>(s.previewDocHs.size());
    slot = {table.rev, table.width, table.styleKey, table.total};
}

// 后台补测一帧：可见带段优先，再文档序；单帧预算 kPreviewSegBudgetMs，
// 未完 → requestUpdate 自驱下一帧（子窗无帧泵，空闲帧需自驱）。
void previewSegPump(NotepadSession& s, const components::MarkdownStyle& style, float scroll,
                    float viewportH) {
    NotepadSession::PreviewSegTable* table = s.previewSegActive;
    if (table == nullptr || !table->usable) {
        s.previewSegPendings = 0;
        return;
    }
    if (table->pending > 0) {
        std::size_t lo = 0;
        std::size_t hi = 0;
        previewSegBand(*table, scroll, viewportH, style, &lo, &hi);
        std::vector<std::size_t> order;
        order.reserve(table->heights.size());
        for (std::size_t i = lo; i <= hi && i < table->heights.size(); ++i) {
            if (!table->measured[i]) order.push_back(i);
        }
        for (std::size_t i = 0; i < table->heights.size(); ++i) {
            if (!table->measured[i] && (i < lo || i > hi)) order.push_back(i);
        }
        const auto started = std::chrono::steady_clock::now();
        for (const std::size_t seg : order) {
            const double elapsed = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - started).count();
            if (elapsed >= kPreviewSegBudgetMs) break;
            previewSegMeasure(s, *table, seg, style);
        }
    }
    previewSegNoteConverged(s, *table);
    s.previewSegPendings = table->pending;
    if (table->pending > 0) app::requestUpdate();
}

// F-13 预览槽位（设计稿 §4.6：编辑区同槽位整页替换）：components::markdown
// 整篇渲染（EUI 组件层合法依赖；块流核是 module_markdown 私有，隔离禁依赖，
// tech 卡 §六）+ scrollView 纵滚。MarkdownStyle 本地 inline 构建（不碰
// module_markdown 私有 scaledPreviewStyle，markdown_module_internal.h:125）；
// blockGap×fontScale 语义照抄 :135——间距侧 token 未参与字号档预乘，字号族
// 已在 componentTokensWeChat 预乘过不再乘。渲染异常兜底（PRD E2）：组件走
// 返回态、无异常纪律，本槽无失败抛出面；空文件 = 「（空文件）」居中占位。
void drawPreviewSlot(
    NotepadSession& s, eui::Ui& ui, float w, float h, float editorTop,
    const components::input_detail::InputModel::InputState& st,
    const components::theme::ThemeColorTokens& componentTheme) {
    components::MarkdownStyle style(componentTheme);
    style.blockGap *= core::theme::fontScale();
    s.selPreviewStyle = style;  // need345 选区反查样式快照（指针回调帧间消费）
    // 滚动偏移留存（Ui 态与窗同寿）；contentKey = 文本修订 → 预览态禁编辑，
    // 度量缓存稳定（textRevision 变化才会重测）。
    float& scroll = ui.state<float>(std::string(kNotepadPreviewId) + ".scroll");
    // bug382：预览视口高（与 scrollView .size 同式）——传给 markdown 组件做
    // 视口裁剪：滚动/调窗/全局帧泵每帧全文档重建元素树是预览卡顿根因，
    // 裁剪后窗口外块只留包装几何（显式高不变），content 不再生成。
    const float viewportH = h - editorTop - kStatusH;
    // debt398：帧首补测上一帧宽度档的段表（provider 本帧同宽直接命中，内容按
    // 已测段出画；宽度/修订变了就不动旧表，等本帧 provider 重建）。
    if (s.previewSegActive != nullptr && s.previewSegActive->usable &&
        s.previewSegActive->rev == st.textRevision && s.selPreviewWidth > 1.0f &&
        std::fabs(s.previewSegActive->width - previewSegWidthKey(s.selPreviewWidth)) < 0.6f) {
        if (s.previewSegActive->pending > 0) previewSegPump(s, style, scroll, viewportH);
        // 总高收敛中可能先增后减：偏移按上一帧已知总高夹紧（防状态越界留白）。
        const float lastMax = std::max(0.0f, s.previewSegActive->total - viewportH);
        if (scroll > lastMax) scroll = lastMax;
    }

    // debt398：分段测高（预览总高异步化）。首帧只出近似总高（段表初始化，0 段
    // 精测），可见带块由 markdown 组件自行精测出内容；屏外未测段只建包装几何，
    // 后台每帧预算补测 → 总高/滚动条渐进收敛。收敛后走 previewDocHs 精确缓存。
    auto previewDocHeight = [&s, &style, rev = st.textRevision](float innerW) -> float {
        const float widthKey = previewSegWidthKey(innerW);
        const std::string styleKey = previewSegStyleKey(style);
        for (const auto& entry : s.previewDocHs) {
            if (entry.height > 0.0f && entry.rev == rev &&
                std::fabs(entry.width - widthKey) < 0.6f && entry.styleKey == styleKey) {
                s.previewSegActive = previewSegFind(s, rev, widthKey, styleKey);
                s.previewSegPendings =
                    s.previewSegActive != nullptr ? s.previewSegActive->pending : 0;
                s.previewSegLastDocH = entry.height;
                return entry.height;
            }
        }
        NotepadSession::PreviewSegTable* table = previewSegFind(s, rev, widthKey, styleKey);
        if (table == nullptr) {
            table = &previewSegInit(s, rev, widthKey, styleKey, style);
        }
        s.previewSegActive = table;
        s.previewSegPendings = table->pending;
        s.previewSegLastDocH = table->total;
        return table->total;
    };
    components::scrollView(ui, kNotepadPreviewId)
        .position(0.0f, editorTop)
        .size(w, viewportH)
        .theme(componentTheme)
        .offset(scroll)
        .onChange([&scroll](float v) { scroll = v; })
        .contentKey(std::to_string(st.textRevision))
        .composeOnScrollOffsetChange()
        // bug399 round5：跳过 scrollView scratch 测量 pass（长文档逐帧全量构建
        // + layout 是 bug259-C 主根因）；宽度变化时才重算，命中走会话缓存。
        .contentHeightProvider([&previewDocHeight](float contentWidth, float) {
            return previewDocHeight(contentWidth);
        })
        // bug399：裁剪型内容必须随滚动重排——visibleRange（bug382）用 scroll
        // 生成裁剪范围，而滚轮本身不触发重排（EUI 语义），不开该标志时范围
        // 冻结在末次 compose 值，滚出 overscan 后新暴露区空白（真机实测）。
        .content([&s, &st, &style, &scroll, viewportH](eui::Ui& inner, float innerW, float) {
            if (s.editText.empty()) {
                inner.text(kNotepadPreviewEmptyId)
                    .position(0.0f, 40.0f)
                    .size(innerW, 32.0f)
                    .text("（空文件）")
                    .fontSize(15.0f)
                    .lineHeight(32.0f)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .color(core::theme::current().colors.textTertiary)
                    .build();
                return;
            }
            // need345 反查宽度快照（内容系 = hit 层局部系，见 session 选区字段注）
            s.selPreviewWidth = innerW;
            // debt398：已测段由组件精测出内容；未测段 hint 近似高（只建包装几何，
            // 帧首补测逐帧推进）。hint 与 docH 同源本帧段表（窗口内无跨帧漂移）。
            const float widthKey = previewSegWidthKey(innerW);
            NotepadSession::PreviewSegTable* segs = s.previewSegActive;
            const bool hintOk = segs != nullptr && segs->usable &&
                                std::fabs(segs->width - widthKey) < 0.6f;
            auto hintFor = [segs, hintOk, &style](std::size_t block) -> float {
                if (!hintOk || segs->bounds.size() < 2) return 0.0f;
                const std::size_t seg = previewSegIndexOf(*segs, block);
                if (seg >= segs->measured.size() || segs->measured[seg]) return 0.0f;
                return previewSegBlockApprox(*segs, seg, style);
            };
            // 整篇包裹 stack：md 文本 + 选区矩形叠层 + 透明 hit 覆盖层（拖选
            // 三件）。高 = estimateHeight（渲染同源估算，displayCpForPoint 的
            // 纵向排布同式）——旧查看器逐块 hit 缝（mdDrawBlockFlowCore）的
            // 整篇等价形态（块流核 module_markdown 私有，隔离禁依赖，见 §注）。
            const float docH = hintOk ? segs->total : s.previewSegLastDocH;
            const std::string wrapId = std::string(kNotepadPreviewContentId) + ".wrap";
            auto wrap = inner.stack(wrapId)
                .width(innerW)
                .height(docH);
            wrap.content([&] {
                components::markdown(inner, kNotepadPreviewContentId)
                    .markdown(s.editText)
                    .width(innerW)
                    .wrapContentHeight()
                    .style(style)
                    .visibleRange(scroll, scroll + viewportH)  // bug382 视口裁剪
                    .blockHeightHint(hintFor)                 // debt398 未测段近似高
                    .build();
                // 选区矩形（info 蓝低透明，旧查看器 selAlpha 同档）：文档系 y
                // 直接叠层（hitRectsForDisplayRangeDoc，need345 EUI-NEO 新缝）；
                // rect 不消费命中，不挡 hit 层事件。
                if (s.selHasText) {
                    size_t cp0 = 0, cp1 = 0;
                    if (notepadSelNormalized(s, &cp0, &cp1)) {
                        const auto rects =
                            components::MarkdownBuilder::hitRectsForDisplayRangeDoc(
                                s.editText, innerW, style, cp0, cp1);
                        const auto& t = core::theme::current();
                        const float selAlpha =
                            core::theme::currentThemeName() != "Dark" ? 0.16f : 0.22f;
                        size_t i = 0;
                        for (const auto& r : rects) {
                            inner.rect(wrapId + ".sel." + std::to_string(i++))
                                .position(r.x, r.y)
                                .size(r.w, r.h)
                                .color(components::theme::withAlpha(t.colors.info, selAlpha))
                                .build();
                        }
                    }
                }
                // need400：查找命中高亮层（独立于选区，不触发「选中即复制」；
                // 半透明 tint 叠在文字上，不消费命中、不挡 hit 层）。命中是显示
                // 文本字节区间 → 码点域反查矩形；hits 升序不重叠 → 线性累计换算。
                if (s.find.visible && !s.find.hits.empty()) {
                    const std::string& fdisp =
                        notepadFindPreviewDisplay(s, st.textRevision);
                    const auto& th = core::theme::current();
                    const eui::Color hitFill = components::theme::withAlpha(
                        th.colors.warning, th.isDark ? 0.30f : 0.26f);
                    const eui::Color curFill = colorOver(
                        hitFill, components::theme::withAlpha(th.colors.primary, 0.45f));
                    size_t byteCur = 0;
                    size_t cpCur = 0;
                    size_t rid = 0;
                    for (size_t hi = 0; hi < s.find.hits.size(); ++hi) {
                        const FindHit& hit = s.find.hits[hi];
                        if (hit.start < byteCur || hit.end > fdisp.size()) continue;
                        cpCur += notepadUtf8Length(
                            fdisp.substr(byteCur, hit.start - byteCur));
                        byteCur = hit.start;
                        const size_t cp0 = cpCur;
                        const size_t cp1 =
                            cpCur + notepadUtf8Length(
                                        fdisp.substr(hit.start, hit.end - hit.start));
                        const auto rects =
                            components::MarkdownBuilder::hitRectsForDisplayRangeDoc(
                                s.editText, innerW, style, cp0, cp1);
                        const eui::Color fill =
                            hi == s.find.cur ? curFill : hitFill;
                        for (const auto& r : rects) {
                            inner.rect(wrapId + ".find." + std::to_string(rid++))
                                .position(r.x, r.y)
                                .size(r.w, r.h)
                                .color(fill)
                                .build();
                        }
                    }
                }
                // need400：待跳转滚动落点（命中置于视口上部 12px，旧查看器
                // computeSyncOffset 口径）；本帧改 offset 下帧生效，requestUpdate 兜底。
                if (s.find.pendingPreviewScroll) {
                    s.find.pendingPreviewScroll = false;
                    if (s.find.visible && s.find.cur < s.find.hits.size()) {
                        const std::string& fdisp =
                            notepadFindPreviewDisplay(s, st.textRevision);
                        const FindHit& hit = s.find.hits[s.find.cur];
                        if (hit.end <= fdisp.size()) {
                            const size_t cp0 =
                                notepadUtf8Length(fdisp.substr(0, hit.start));
                            const size_t cp1 =
                                cp0 + notepadUtf8Length(
                                          fdisp.substr(hit.start, hit.end - hit.start));
                            const auto rects =
                                components::MarkdownBuilder::hitRectsForDisplayRangeDoc(
                                    s.editText, innerW, style, cp0, cp1);
                            if (!rects.empty()) {
                                scroll = std::max(0.0f, rects.front().y - 12.0f);
                                app::requestUpdate();
                            }
                        }
                    }
                }
                // 透明 hit 覆盖层（不劫持滚动——块流 hit 层同机制）：坐标 = 覆盖
                // 层局部 = 整篇内容系（notepadSelHitCp 直入反查）；回调只捕
                // session 指针（clue #292 值捕获纪律，禁捕帧栈上 style/innerW）。
                components::mouseArea(inner, wrapId + ".hit")
                    .size(innerW, docH)
                    .cursor(core::CursorShape::IBeam)
                    .onPress([p = &s](const components::MouseEvent& ev) {
                        notepadSelPress(*p, ev);
                    })
                    .onDrag([p = &s](const components::MouseEvent& ev) {
                        notepadSelDrag(*p, ev);
                    })
                    .onRelease([p = &s](const components::MouseEvent& ev) {
                        notepadSelRelease(*p, ev);
                    })
                    .build();
            });
            wrap.build();
        })
        .build();
}

}  // namespace

// debt398 测试缝（声明见 notepad_session.h）：同步把活动段表全部段精测到底。
int notepadPreviewWarmForTest(NotepadSession& s) {
    NotepadSession::PreviewSegTable* table = s.previewSegActive;
    if (table == nullptr || !table->usable) return 0;
    const components::MarkdownStyle style = s.selPreviewStyle;
    int measured = 0;
    for (std::size_t seg = 0; seg < table->heights.size(); ++seg) {
        if (table->measured[seg]) continue;
        previewSegMeasure(s, *table, seg, style);
        ++measured;
    }
    previewSegNoteConverged(s, *table);
    s.previewSegPendings = table->pending;
    return measured;
}

// need345：预览选区文本构建单点（声明见 notepad_session.h）——整篇显示文本
// 码点域切片：跨块 '\n' 已含在扁平域内（= 旧查看器「多块按阅读序 '\n' 连接」
// 的整篇等价语义）；显示域无 markdown 标记符；码点切片不产半字。拖选抬起
// 自动复制与 Ctrl+C 兼容通道共用本函数（测试直驱同一条通路）。
std::string notepadPreviewSelText(const NotepadSession& s) {
    size_t cp0 = 0, cp1 = 0;
    if (!notepadSelNormalized(s, &cp0, &cp1)) return {};
    const std::string display = components::MarkdownBuilder::displayTextOf(s.editText);
    const size_t begin = notepadSelCpToByte(display, cp0);
    const size_t end = notepadSelCpToByte(display, cp1);
    if (begin >= end) return {};
    return display.substr(begin, end - begin);
}

// 未保存保护三选弹窗（批 4 F-05）：渲染在子窗自己的 Ui 树内（每窗独立）。
// components::dialog content() 缝自绘三钮行（dialog.h 默认布局只有两钮位）；
// primary=保存 / 取消=中性钮 / 不保存=危险文字钮（error 色）。
// 回调按 clue #292 值捕获 session 指针（点击帧才调，禁 [&] 捕 compose 帧栈局部）。
void drawProtectDialog(NotepadSession& s, eui::Ui& ui, float w, float h,
                       const components::theme::ThemeColorTokens& tokens) {
    using components::button;
    const components::DialogStyle style(tokens);
    const auto& m = tokens.metrics;
    constexpr float kDialogW = 420.0f;  // P3-6 走查：设计稿 §4.9 宽 420
    constexpr float kDialogH = 200.0f;
    const float inset = m.spacing.panel;
    const float contentWidth = kDialogW - 2.0f * inset;
    const float gap = m.spacing.content;
    const float btnW = (contentWidth - 2.0f * gap) / 3.0f;
    const float rowY = std::max(120.0f, kDialogH - m.control.control - inset);
    NotepadSession* raw = &s;

    components::dialog(ui, kNotepadProtectDialogId)
        .open(s.protectOpen)
        .screen(w, h)
        .size(kDialogW, kDialogH)
        .theme(tokens)
        // 背板点击 / requestClose = 取消语义（关窗不保存，窗保留）
        .onOpenChange([raw](bool open) {
            if (!open) raw->protectOpen = false;
        })
        .content([&] {
            // need381：字号调正常档（老板「字太大」）——标题 heading(24)→
            // subtitle(20)+600 字重（对齐全站弹窗 kDialogTitleFontSize=20 统一
            // 档），正文 input(17)→label(14)。
            ui.text(std::string(kNotepadProtectDialogId) + ".title")
                .position(inset, 16.0f)
                .size(contentWidth, m.typography.subtitle + m.typography.lineGapLoose)
                .text("未保存的更改")
                .fontSize(m.typography.subtitle)
                .fontWeight(600)
                .lineHeight(m.typography.subtitle + m.typography.lineGapLoose)
                .color(style.title)
                .build();
            ui.text(std::string(kNotepadProtectDialogId) + ".message")
                .position(inset, 52.0f)
                .size(contentWidth, 64.0f)
                .text("“" + s.title + "”有未保存的更改，关闭前要保存吗？")
                .fontSize(m.typography.label)
                .lineHeight(m.typography.label + m.typography.lineGapComfortable)
                .maxWidth(contentWidth)
                .wrap(true)
                .color(style.message)
                .build();
            // 三钮行（L→R：不保存 | 取消 | 保存——主钮最右，macOS sheet 惯例）
            ui.row(std::string(kNotepadProtectDialogId) + ".actions")
                .position(inset, rowY)
                .size(contentWidth, m.control.control)
                .gap(gap)
                .content([raw, btnW, &ui, &m, &tokens, &style] {
                    const float h = m.control.control;
                    button(ui, kNotepadProtectBtnDiscardId)
                        .theme(tokens, false)
                        .size(btnW, h)
                        .text("不保存")
                        .fontSize(m.typography.option)
                        .colors(style.secondary, style.secondaryHover,
                                style.secondaryPressed)
                        .textColor(tokens.error)
                        .iconColor(tokens.error)
                        .radius(m.radius.popup)
                        .border(m.spacing.hairline, style.border)
                        .disabled(!raw->protectOpen)
                        .onClick([raw] {
                            NotepadSession& s = *raw;
                            const NotepadProtectOutcome out =
                                notepadProtectOutcomeFor(
                                    NotepadProtectChoice::Discard, true,
                                    s.protectQuitContext);
                            if (out.armExempt) s.skipCloseConfirm = true;
                            s.protectOpen = false;
                            if (out.rearmExit) {
                                app::requestExit();  // clue #269：guard 链再入队
                            } else if (out.closeWindow && s.handle != nullptr) {
                                core::window::requestWindowClose(
                                    static_cast<core::window::Handle>(s.handle));
                            }
                        })
                        .build();
                    button(ui, kNotepadProtectBtnCancelId)
                        .theme(tokens, false)
                        .size(btnW, h)
                        .text("取消")
                        .fontSize(m.typography.option)
                        .colors(style.secondary, style.secondaryHover,
                                style.secondaryPressed)
                        .textColor(style.title)
                        .iconColor(style.title)
                        .radius(m.radius.popup)
                        .border(m.spacing.hairline, style.border)
                        .disabled(!raw->protectOpen)
                        .onClick([raw] {
                            // 取消 = 留窗；退出语境不 rearm = 中止整个退出（D-3）
                            raw->protectOpen = false;
                        })
                        .build();
                    button(ui, kNotepadProtectBtnSaveId)
                        .theme(tokens, true)
                        .size(btnW, h)
                        .text("保存")
                        .fontSize(m.typography.option)
                        .colors(style.primary, style.primaryHover,
                                style.primaryPressed)
                        .radius(m.radius.popup)
                        .disabled(!raw->protectOpen)
                        .onClick([raw] {
                            NotepadSession& s = *raw;
                            notepadSaveSession(s);  // 失败（mtime 拦截/写失败）toast 已示
                            const NotepadProtectOutcome out =
                                notepadProtectOutcomeFor(
                                    NotepadProtectChoice::Save, !s.dirty,
                                    s.protectQuitContext);
                            s.protectOpen = false;
                            if (out.rearmExit) {
                                app::requestExit();
                            } else if (out.closeWindow && s.handle != nullptr) {
                                core::window::requestWindowClose(
                                    static_cast<core::window::Handle>(s.handle));
                            }
                        })
                        .build();
                })
                .build();
        })
        .build();
}

void notepadComposeSession(NotepadSession& s, eui::Ui& ui,
                           const eui::Screen& screen) {
    const core::theme::Tokens& t = core::theme::current();
    const auto componentTheme = core::theme::componentTokensWeChat(t);
    const float w = screen.width;
    const float h = screen.height;

    // 尺寸记忆回写源：每帧回填（关窗回调读不到 compose 态，只能这样取）
    s.lastW = static_cast<int>(w);
    s.lastH = static_cast<int>(h);
    s.sizeSeen = true;

    const bool errorState = !s.loadError.empty();
    // 提示条分档（P3-5 走查，设计稿 §7.4）：只读档 = warning 不可关；大文件
    // 档 = info 可关（关 = 本窗 Ui 态旗标，窗重开还原）。二进制 suspect 恒伴
    // readonly（applyLoadedState），归只读档。
    bool& promptDismissed = ui.state<bool>("notepad.promptbar.dismissed");
    const bool readonlyPrompt =
        !errorState && (s.readonly || s.binarySuspect);
    const bool largeFilePrompt =
        !errorState && !readonlyPrompt && s.largeFile;
    const bool showPrompt =
        readonlyPrompt || (largeFilePrompt && !promptDismissed);
    const float editorTop = kToolbarH + (showPrompt ? kPromptH : 0.0f);

    // ── 工具条 44（无新建钮 = N-14）──
    ui.rect(kNotepadToolbarId)
        .position(0.0f, 0.0f)
        .size(w, kToolbarH)
        .color(t.colors.background)
        .build();
    const bool editable = !errorState && !s.readonly && !s.binarySuspect;
    // 按扩展名的按钮矩阵 + 预览态工具条联动（F-11/F-13，设计稿 §4.6；纯函数）
    const NotepadToolbarMatrix matrix = notepadToolbarMatrixFor(s);
    // need374 工具条宽度自适应：固定 x 全改游标式依序排布（控件有则占位、
    // 无则收紧，不留位空洞——原 P3-7 让位逻辑的泛化）。窄窗 <900 收起
    // 「字体族+字号步进」组（与状态栏 T1 档隐编码/行尾同阈值同口径）；≥900
    // 各控件位与旧固定布局逐位一致（16/88/160/240/408/492/564/636）。
    constexpr float kTbGap = 8.0f;      // 控件间距（原布局 72=64+8 同源）
    constexpr float kTbBtnW = 64.0f;    // toolbarButton 钮宽
    const bool showFontGroup = w >= 900.0f;
    float tbX = 16.0f;
    toolbarButton(ui, kNotepadBtnOpenId, "打开", tbX, t, true, [&s] {
        notepadOpenViaDialog(s);
    });
    tbX += kTbBtnW + kTbGap;
    toolbarButton(ui, kNotepadBtnSaveId, "保存", tbX, t, editable, [&s] {
        notepadSaveSession(s);
    });
    tbX += kTbBtnW + kTbGap;
    // F-04 另存为（need289 修复棒 1；设计稿 §4.2 btnSaveAs，保存钮右侧 8px）：
    // readonly/binary/错误窗禁用（禁用矩阵与保存钮同 editable 口径，§7.2）
    toolbarButton(ui, kNotepadBtnSaveAsId, "另存为", tbX, t, editable, [&s] {
        notepadSaveSessionAs(s);
    });
    tbX += kTbBtnW + kTbGap;
    // ── 批 3 文档语义组（设计稿 §4.2）：字体下拉 + 字号步进 + 格式化/预览（棒 B）──
    const float fontTriggerX = tbX + kTbGap;  // 旧布局 240（另存为 224 后 16px）；
    // 弹层面板与触发器同位（compose 尾共用）
    if (showFontGroup) {
        drawFontTrigger(s, ui, t, matrix.fontControlsEnabled, fontTriggerX);
        {
            const float stepperX = fontTriggerX + 160.0f + kTbGap;  // 旧布局 408
            const bool canStep = !errorState && matrix.fontControlsEnabled;
            barButton(ui, kNotepadStepperFontMinusId, "A-", stepperX, 10.0f,
                      24.0f, 24.0f, t, canStep && s.editorFontSize > 10, [&s] {
                          notepadStepFontSize(s, -1);
                      });
            char fsBuf[16];
            std::snprintf(fsBuf, sizeof fsBuf, "%d", s.editorFontSize);
            ui.text("notepad.stepperFont.val")
                .position(stepperX + 24.0f, 8.0f)
                .size(28.0f, 28.0f)
                .text(fsBuf)
                .fontSize(13.0f)
                .lineHeight(28.0f)
                .verticalAlign(eui::VerticalAlign::Center)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(t.colors.textPrimary)
                .build();
            barButton(ui, kNotepadStepperFontPlusId, "A+", stepperX + 52.0f,
                      10.0f, 24.0f, 24.0f, t,
                      canStep && s.editorFontSize < 32, [&s] {
                          notepadStepFontSize(s, 1);
                      });
            tbX = stepperX + 76.0f + kTbGap;  // A+ 460+24 之后 8px
        }
    }
    // F-11/need380 格式化钮（.json = JSON 美化；.sql = SQL 关键字规范化+缩进；
    // readonly/binary/预览态禁用）。tooltip 固定副文案 = D-4 轻量明示
    //（settings_ui_cards.cpp:618 同款写法）。
    if (matrix.showFormat) {
        const bool isSqlFormat = notepadSessionHasExtension(s, ".sql");
        toolbarButton(ui, kNotepadBtnFormatId, "格式化", tbX, t,
                      matrix.formatEnabled, [&s, &ui, isSqlFormat] {
                          if (isSqlFormat) {
                              notepadFormatSql(
                                  s, ui.state<InputState>(kNotepadEditorInputId));
                          } else {
                              notepadFormatJson(
                                  s, ui.state<InputState>(kNotepadEditorInputId));
                          }
                      });
        components::tooltip(ui, std::string(kNotepadBtnFormatId) + ".tooltip")
            .theme(componentTheme)
            .source(kNotepadBtnFormatId)
            .value(isSqlFormat ? "SQL 关键字将统一大写并重排缩进（语义等值）"
                               : "对象键将按字典序重排（语义等值，字节序会变）")
            .anchor(tbX + 32.0f, 8.0f + 28.0f)
            .bounds(w, h)
            .minWidth(200.0f)
            .maxWidth(320.0f)
            .build();
        tbX += kTbBtnW + kTbGap;
    }
    // F-13 预览钮（仅 .md/.markdown 扩展显示；大文件禁用 = 1MB 级渲染成本
    // 未证，PRD F-16 降级口径）。预览态钮面切「编辑」= 点击切回。
    if (matrix.showPreview) {
        toolbarButton(ui, kNotepadBtnPreviewId, s.previewOn ? "编辑" : "预览",
                      tbX, t, matrix.previewEnabled, [&s] {
                          notepadTogglePreview(s);
                      });
        tbX += kTbBtnW + kTbGap;
    }
    // F-19 自动换行钮（toggle 选中态高亮；largeFile 强制关 + 钮禁用 =
    // 大文件模式口径）。
    toolbarButton(ui, kNotepadBtnWrapId, "换行", tbX, t,
                  !errorState && !s.largeFile, [&s] {
                      notepadToggleWrap(s);
                  }, s.wrapOn);

    // ── 提示条 32（只读降级 / 大文件模式；错误态文本直接占编辑区，不叠提示条）──
    if (showPrompt) {
        const bool infoTier = largeFilePrompt;  // P3-5：大文件档 info 可关
        ui.rect("notepad.promptbar")
            .position(0.0f, kToolbarH)
            .size(w, kPromptH)
            .color(components::theme::withAlpha(
                infoTier ? t.colors.info : t.colors.warning, 0.12f))
            .build();
        const char* msg = infoTier ? "大文件模式：已关闭代码高亮与自动换行"
                                   : s.readonlyReason.c_str();
        ui.text("notepad.promptbar.text")
            .position(16.0f, kToolbarH)
            .size(w - 32.0f - (infoTier ? 28.0f : 0.0f), kPromptH)
            .text(msg)
            .fontSize(12.0f)
            .lineHeight(kPromptH)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textSecondary)
            .build();
        if (infoTier) {
            // 关闭钮（P3-5）：点击收本条（编辑区上移补位）；只读档无此钮
            barButton(ui, "notepad.promptbar.close", "×", w - 36.0f,
                      kToolbarH + 4.0f, 24.0f, 24.0f, t, true, [&ui] {
                          ui.state<bool>("notepad.promptbar.dismissed") = true;
                          app::requestUpdate();
                      });
        }
    }

    // ── 编辑区 flex（唯一内滚容器；错误态 = 占位文案）──
    if (errorState) {
        ui.text("notepad.loaderror")
            .position(0.0f, editorTop)
            .size(w, h - editorTop - kStatusH)
            .text(s.loadError)
            .fontSize(14.0f)
            .lineHeight(28.0f)
            .verticalAlign(eui::VerticalAlign::Center)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.textSecondary)
            .build();
    } else {
        auto& st = ui.state<InputState>(kNotepadEditorInputId);
        if (!s.seeded) {
            // 首帧播种：载入初值（LF 域）进框架 InputState；此后镜像随 onChange 回流
            s.seeded = true;
            st.text = s.editText;
            st.cursor = 0;
            st.selectionStart = 0;
            st.selectionEnd = 0;
            ++st.textRevision;
        }
        // 语言懒求值（映射结果随 path 固定；空 = None 纯文本，不再重算也无妨——
        // 一次 16 行后缀扫描，成本可忽略）
        if (s.language.empty() && !s.path.empty()) {
            s.language = std::string(languageForExtension(s.path));
        }
        // M+Shift+F 待消费（键路由置位，InputState 直写单点在 compose）；
        // 按键与帧间状态可能漂移，§4.6 预览禁用 + 只读面再核一遍。
        // need380：按扩展名分派（.json → JSON 美化；.sql → SQL 规范化）
        if (s.pendingFormatJson) {
            s.pendingFormatJson = false;
            if (!s.previewOn && !s.readonly && !s.binarySuspect) {
                if (notepadSessionHasExtension(s, ".json")) {
                    notepadFormatJson(s, st);
                } else if (notepadSessionHasExtension(s, ".sql")) {
                    notepadFormatSql(s, st);
                }
            }
        }
        if (s.previewOn) {
            // 预览槽位整页替换编辑器（InputState 不动 → 切回光标/滚动保留）。
            // need400：预览态查找 tick 走显示文本域（命中/跳转只驱动预览滚动，
            // 不落编辑器光标/选区——预览禁编辑）
            notepadPreviewFindTick(s, st);
            drawPreviewSlot(s, ui, w, h, editorTop, st, componentTheme);
        } else {
            // 查找态 tick：待跳转消费 + 输入即重算 + 文本编辑重算（见函数注）
            notepadFindTick(s, st);
        }
        if (!s.previewOn) {
        // 命中层合成色（need377：kFindHit* 常量档，随主题明暗取档）
        const eui::Color hitBg = components::theme::withAlpha(
            t.colors.warning, t.isDark ? kFindHitAlphaDark : kFindHitAlphaLight);
        const eui::Color curHitBg =
            colorOver(hitBg, components::theme::withAlpha(
                                 t.colors.primary, t.isDark ? kFindCurHitOverlayDark
                                                             : kFindCurHitOverlayLight));
        auto editor = components::input(ui, kNotepadEditorInputId)
                          .position(0.0f, editorTop)
                          .size(w, h - editorTop - kStatusH)
                          .value(s.editText)
                          .multiline(true)
                          .scrollbar(true)
                          .softWrap(s.wrapOn)  // F-19 换行钮实时口径（EUI 旋钮）
                          .fontSize(static_cast<float>(s.editorFontSize))
                          .fontFamily(s.fontFamily)  // 空 = 系统默认（F-09 即时生效）
                          .inset(10.0f)
                          .theme(componentTheme)
                          .onChange([&s](const std::string& v) {
                              // 只读降级窗：不建编辑计账（输入回显被下帧 value 重置覆盖，脏态
                              // 恒 false → 保存/关窗链路零风险）；正 经编辑 = 镜像 + 脏态单点
                              if (s.readonly || s.binarySuspect) return;
                              if (v == s.editText) return;
                              s.editText = v;
                              sessionSetDirty(s, v != s.loadText);
                              app::requestUpdate();  // 状态栏「行:列 / ●未保存」实时刷新
                          });
        if (!s.largeFile) {
            // 语法层 ∪ 命中层（3 参 lineStyles 契约 + 4 参 highlightLine 桥接，
            // session.language 绑定）；大文件两层全关（F-16 v1 取舍）
            editor.lineStyles([&s, hitBg, curHitBg](
                                  const std::string& displayText,
                                  int lineStartByte, int lineNo) {
                return notepadEditorLineRuns(s, displayText, lineStartByte,
                                             lineNo, hitBg, curHitBg);
            });
        }
        editor.build();
        // P2-3：关条焦点回收消费（notepadFindClose 置位；开条播种聚焦的对称面）。
        // need376：同款 .hit 修正——聚焦可聚焦元素而非组件根 id
        if (s.find.pendingFocusEditor && !s.find.visible) {
            s.find.pendingFocusEditor = false;
            ui.requestFocus(std::string(kNotepadEditorInputId) + ".hit");
        }
        }
    }

    // ── 状态栏 28（F-17 基础版：文件名｜●未保存｜行:列｜字符数）──
    ui.rect(kNotepadStatusbarId)
        .position(0.0f, h - kStatusH)
        .size(w, kStatusH)
        .color(t.colors.surface)
        .border(1.0f, t.colors.borderMuted)
        .build();
    ui.text("notepad.status.title")
        .position(16.0f, h - kStatusH)
        .size(280.0f, kStatusH)
        .text(s.title)
        .fontSize(12.0f)
        .lineHeight(kStatusH)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textSecondary)
        .build();
    if (s.dirty) {
        // x 随标题估宽：CJK/其余分档（notepadTitleEstWidth，P3-1 走查）
        const float titleW = notepadTitleEstWidth(s.title);
        ui.text("notepad.status.dirty")
            .position(16.0f + titleW, h - kStatusH)
            .size(90.0f, kStatusH)
            .text("●未保存")
            .fontSize(12.0f)
            .lineHeight(kStatusH)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.warning)
            .build();
    }
    if (!errorState) {
        // 行:列 / 字符数（P2-5 缓存门控）：键 = (textRevision, cursor)，未变帧
        // 零扫描（原每帧 3×全文遍历，10MB 只读窗约 3-5ms/帧）
        NotepadStatusStats& stats =
            ui.state<NotepadStatusStats>("notepad.status.stats");
        const InputState& st = ui.state<InputState>(kNotepadEditorInputId);
        if (!stats.valid || stats.textRev != st.textRevision ||
            stats.cursor != st.cursor) {
            stats.valid = true;
            stats.textRev = st.textRevision;
            stats.cursor = st.cursor;
            stats.line = notepadByteOffsetToLine(st.text, st.cursor);
            const size_t lineStart = notepadLineStartOffset(st.text, stats.line);
            // cursor 是 int（InputState 契约），lineStart 是 size_t：先归一为
            // size_t 再比较，避免 -Wsign-compare（本仓 -Werror）。
            const size_t cursorBytes = static_cast<size_t>(st.cursor);
            stats.colCp =
                cursorBytes >= lineStart
                    ? notepadUtf8Length(
                          st.text.substr(lineStart, cursorBytes - lineStart))
                    : 0;
            stats.chars = notepadUtf8Length(st.text);
        }
        char buf[64];
        std::snprintf(buf, sizeof buf, "行 %d, 列 %zu", stats.line + 1,
                      stats.colCp + 1);
        ui.text("notepad.status.cursor")
            .position(w - 300.0f, h - kStatusH)
            .size(140.0f, kStatusH)
            .text(buf)
            .fontSize(12.0f)
            .lineHeight(kStatusH)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textSecondary)
            .build();
        std::snprintf(buf, sizeof buf, "%zu 字符", stats.chars);
        ui.text("notepad.status.chars")
            .position(w - 150.0f, h - kStatusH)
            .size(134.0f, kStatusH)
            .text(buf)
            .fontSize(12.0f)
            .lineHeight(kStatusH)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textSecondary)
            .build();
        // F-15 编码 / 行尾点切段（设计稿 §4.7 statusMeta 右锚链最左两段；
        // 窄窗 <900 隐藏 = T1 档「状态栏隐编码/行尾」挤压顺序）。只读窗
        // 禁用（灰显不可点）；错误占位窗整块不渲染（无文件无编码语义）。
        if (w >= 900.0f) {
            const bool canToggle = editable;
            // 编码段：UTF-8 ↔ UTF-8 BOM 循环（D-8 两档）；段宽 128（P3-2 走查：
            // 最长标签「GB18030（推测）」约 109px 超 102 固定段宽会截断）
            ui.stack(kNotepadStatusEncodingId)
                .position(w - 500.0f, h - kStatusH)
                .size(128.0f, kStatusH)
                .disabled(!canToggle)
                .onClick([&s] { notepadToggleBom(s); })
                .content([&] {
                    ui.rect(kNotepadStatusEncodingId + std::string(".hover"))
                        .size(128.0f, kStatusH)
                        .states(eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                                t.colors.hover, t.colors.hover)
                        .radius(4.0f)
                        .instantStates()
                        .build();
                    ui.text(kNotepadStatusEncodingId + std::string(".label"))
                        .size(128.0f, kStatusH)
                        .text(notepadEncodingLabel(s))
                        .fontSize(12.0f)
                        .lineHeight(kStatusH)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .color(canToggle ? t.colors.textSecondary
                                         : t.colors.textDisabled)
                        .build();
                })
                .build();
            // 行尾段：LF ↔ CRLF 循环（Mixed 显「混合」，点切先归 CRLF）
            ui.stack(kNotepadStatusEolId)
                .position(w - 364.0f, h - kStatusH)
                .size(60.0f, kStatusH)
                .disabled(!canToggle)
                .onClick([&s] { notepadToggleEol(s); })
                .content([&] {
                    ui.rect(kNotepadStatusEolId + std::string(".hover"))
                        .size(60.0f, kStatusH)
                        .states(eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                                t.colors.hover, t.colors.hover)
                        .radius(4.0f)
                        .instantStates()
                        .build();
                    ui.text(kNotepadStatusEolId + std::string(".label"))
                        .size(60.0f, kStatusH)
                        .text(notepadEolLabel(s))
                        .fontSize(12.0f)
                        .lineHeight(kStatusH)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .color(canToggle ? t.colors.textSecondary
                                         : t.colors.textDisabled)
                        .build();
                })
                .build();
        }
    }

    // ── 查找条浮层（z 序最上，锚编辑区右上；错误态无编辑器不渲染；need400
    // 预览态同样渲染——查找/跳转作用于预览显示域，替换面禁用）──
    if (s.find.visible && !errorState) {
        drawSearchBar(s, ui, w, editorTop, t, componentTheme, editable);
    }

    // ── 字体下拉弹层（z 序最上：工具条触发器 + 全窗遮罩点击层 + 内滚面板；
    // 开时遮编辑区）──
    drawFontPopup(s, ui, t, w, h, fontTriggerX);  // need374：面板随触发器游标位

    // ── 会话内轻提示（聚焦/保存反馈；自管 1.5s 窗口，多窗互不覆盖）──
    if (!s.toastMessage.empty() && std::chrono::steady_clock::now() < s.toastUntil) {
        ui_kit::drawLightToastPill(ui, "notepad.toast", s.toastMessage, w / 2.0f,
                                   h * 0.4f, t);
    } else if (!s.toastMessage.empty()) {
        s.toastMessage.clear();
    }

    // ── 未保存保护三选弹窗（批 4 F-05；z 序最上，该窗 Ui 树内每窗独立）──
    drawProtectDialog(s, ui, w, h, componentTheme);
}

void notepadHandleKeySession(NotepadSession& s, const eui::KeyEvent& key) {
    if (!key.isDown()) return;
    // M=平台主修饰键（shortcut() 按 App=Super / Win=Ctrl 映射）；Shift 组合另判
    const bool shortcut =
        key.modifiers.shortcut() && !key.modifiers.shift && !key.modifiers.alt;
    if (shortcut && key.key == eui::InputKey::O) {
        notepadOpenViaDialog(s);  // F-02：窗内打开（已开路径由查重聚焦收口）
        return;
    }
    if (shortcut && key.key == eui::InputKey::S) {
        notepadSaveSession(s);  // F-03：干净短路 / mtime 拦截 / 脏重编码
        return;
    }
    // Shift 组合支路（shortcut() 显式排除 shift，M+Shift+S/F/V 单独判定；
    // PRD §九快捷键表 + 设计稿 §6，need289 修复棒 1 接线）
    const bool shortcutShift =
        key.modifiers.shortcut() && key.modifiers.shift && !key.modifiers.alt;
    if (shortcutShift && key.key == eui::InputKey::S) {
        notepadSaveSessionAs(s);  // F-04 另存为（错误/只读/二进制窗同口径拦截）
        return;
    }
    if (shortcutShift && key.key == eui::InputKey::F) {
        // M+Shift+F 格式化（F-11：仅 .json 可编辑窗；预览态禁用 §4.6）。
        // 键处理器无 Ui 访问权 → 置位 pendingFormatJson，compose 消费
        //（find.pendingJump 同模式）
        const NotepadToolbarMatrix m = notepadToolbarMatrixFor(s);
        if (m.showFormat && m.formatEnabled) s.pendingFormatJson = true;
        return;
    }
    if (shortcutShift && key.key == eui::InputKey::V) {
        notepadTogglePreviewIfAvailable(s);  // M+Shift+V 预览切换（PRD §九）
        return;
    }
    if (shortcut && key.key == eui::InputKey::H) {
        // need400：预览态只读无替换语义 → 退化为普通查找条
        if (s.previewOn) {
            notepadFindToggle(s);
        } else {
            notepadFindOpenReplace(s);  // M+H 替换条直开（PRD 流程 D3）
        }
        return;
    }
    if (shortcut && key.key == eui::InputKey::P) {
        notepadTogglePreviewIfAvailable(s);  // M+P 预览切换（设计稿 §6）
        return;
    }
    // Ctrl/Cmd+C（need345 预览选区复制兼容通道）：与拖选自动复制同一条
    // notepadPreviewSelText 构建通路（预览只读无编辑态；编辑态 Ctrl+C 由
    // input 组件剪贴板语义承担——预览态编辑 input 不在树内，键必到窗口级）。
    if (shortcut && key.key == eui::InputKey::C && s.previewOn && s.selHasText) {
        notepadCopySel(s, notepadPreviewSelText(s));
        return;
    }
    if (shortcut && key.key == eui::InputKey::W && s.handle != nullptr) {
        // M+W 关窗（窗口级，不进主窗 Router）；未保存保护链批 4 于
        // onCloseRequested 挂载（本批回调直通放行）
        core::window::requestWindowClose(
            static_cast<core::window::Handle>(s.handle));
        return;
    }
    if (shortcut && key.key == eui::InputKey::F) {
        // need400：预览态同样开条（命中域 = 显示文本，notepadPreviewFindTick）
        notepadFindToggle(s);  // M+F 开/关查找条（再唤起保留词全选，F-06）
        return;
    }
    if (shortcut && key.key == eui::InputKey::Equal) {
        notepadStepFontSize(s, 1);  // M+= 放大（F-10；Shift+Equal 同分支兼容）
        return;
    }
    if (shortcut && key.key == eui::InputKey::Minus) {
        notepadStepFontSize(s, -1);  // M+- 缩小
        return;
    }
    // 查找条开着时的 Enter / Shift+Enter / Esc 兜底通路：查找输入聚焦时由
    // input onKeyEvent 先消费（input.h:240 passthrough，已处理键不冒泡到
    // 窗口级——runtime_input.h:323 仅 !handled 才调窗级处理器），未聚焦走这里。
    if (s.find.visible && s.loadError.empty()) {
        if (key.key == eui::InputKey::Enter) {
            s.find.pendingJump = key.modifiers.shift ? -1 : 1;  // 单调跳转（环绕）
            app::requestUpdate();
            return;
        }
        if (key.key == eui::InputKey::Escape) {
            notepadFindClose(s);  // 清高亮留词（F-06）
            app::requestUpdate();
            return;
        }
    }
    // Esc 兜底（P2-4）：字体弹层收起请求位（Ui 树 FontPopupState 键路由不可达
    // → session 置位，drawFontPopup 消费；弹层未开时消费即弃，零副作用）
    if (key.key == eui::InputKey::Escape) {
        s.pendingPopupDismiss = true;
        app::requestUpdate();
    }
}

void notepadOpenViaDialog(NotepadSession& s) {
    (void)s;
    eui::platform::FileDialogOptions options;
    options.prompt = "打开文件";
    options.filterName = "文本与代码文件";
    for (const std::string_view ext : core::notepad::notepadExtensions()) {
        options.allowedExtensions.emplace_back(ext);
    }
    const eui::platform::FileDialogResult result =
        eui::platform::openFileDialog(options);
    if (!result.selected()) return;  // 取消 → 零副作用（F-02）
    for (const std::string& path : result.paths) {
        LOG_INFO("记事本窗内打开: " + path);
        openNotepadWindow(path);  // 已开路径 → 查重命中聚焦，不开新窗
    }
    app::requestUpdate();
}

}  // namespace module::notepad
