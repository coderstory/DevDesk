// Markdown 模块·编辑器块（MarkdownModule 成员函数定义）：编辑列 / 键盘流接线 /
// 文本变更 / 标签栏 / 状态栏 / 标签增删筛选 / 自动保存与落盘。
// 拆分第一步（抽编译单元）产物：类定义在 markdown_module_internal.h，
// 本文件只承载成员函数体，纯代码搬移，逻辑与签名零变化。

#include "markdown_module_internal.h"

#include <utility>  // std::move（check-std-includes：不靠传递包含）

namespace module::markdown {

void MarkdownModule::drawEditorColumn(eui::Ui& ui,
                      float bodyH,
                      float midW,
                      const core::theme::Tokens& t,
                      const components::theme::ThemeColorTokens& componentTheme) {
    (void)componentTheme;
    // 布局语义与耦合点见 drawTreeColumn 注释。
    drawTextColumnChrome(
        ui, "md.editor", bodyH, midW, t,
        componentTheme, [&](const TextColumnGeometry& g) {
            components::input(ui, kEditorInputId)
                .position(g.x, g.y)
                .size(g.w, g.h)
                .value(editorText_)
                .placeholder("在这里写 Markdown…")
                .multiline(true)
                .scrollbar(true)
                .fontSize(mdEditorFontSize())
                .inset(10.0f)
                .theme(componentTheme)
                .onChange([this](const std::string& v) {
                    // 输入更新内存缓冲；落盘走防抖（见 flushEditorIfIdle）
                    onEditorTextChanged(v);
                })
                // 聚焦时先于内置处理收到按键，返回 true = 拦截。
                .onKeyEvent([this, &ui](const eui::KeyEvent& event) {
                    return handleEditorKey(ui, event);
                })
                // 行级高亮（styled-runs）：回调只给行文本/行首偏移，围栏状态需全文 → 从
                // InputState 取 display 文本（IME 期间是 preedit 快照，字节偏移不会错位）。
                .lineStyles([this, &ui](const std::string& lineText,
                                        int lineStartByte, int lineNo) {
                    (void)lineNo;
                    using InputState =
                        components::input_detail::InputModel::InputState;
                    auto& st = ui.state<InputState>(kEditorInputId);
                    return highlighter_.runsForLine(
                        st.preedit ? st.preedit->text : st.text, lineText,
                        lineStartByte);
                })
                // 输入框右键菜单直挂 input（onContextMenu），坐标经统一换算（窗口 → 页面坐标）。
                .onContextMenu([this](const eui::PointerEvent& ev,
                                      const eui::Rect&) {
                    const core::MenuAnchor a =
                        menuAnchorForPointerInPage(ev);
                    editorCtxX_ = a.x;
                    editorCtxY_ = a.y;
                    editorCtxOpen_ = true;
                })
                .build();
        });
}

bool MarkdownModule::handleEditorKey(eui::Ui& ui, const eui::KeyEvent& event) {
    using InputState = components::input_detail::InputModel::InputState;
    if (!event.isDown()) return false;
    auto& st = ui.state<InputState>(kEditorInputId);
    // IME 预编辑中不拦截（Ctrl 类修饰键 IME 也可能用）
    if (!st.compositionText.empty()) return false;

    EditBuffer buf{st.text, st.cursor, st.selectionStart, st.selectionEnd};
    const bool shortcut = event.modifiers.shortcut();
    const bool plain = !shortcut && !event.modifiers.shift &&
                       !event.modifiers.alt && !event.modifiers.super;
    bool handled = false;
    if (event.key == eui::InputKey::Tab && !shortcut && !event.modifiers.alt) {
        if (!applyTabTable(buf, event.modifiers.shift)) {
            applyTab(buf, event.modifiers.shift);
        }
        handled = true;
    } else if (event.key == eui::InputKey::Enter && plain) {
        handled = applyEnterList(buf);
    } else if (shortcut && !event.modifiers.shift && !event.modifiers.alt) {
        if (event.key == eui::InputKey::B) {
            applyWrapAround(buf, "**");
            handled = true;
        } else if (event.key == eui::InputKey::I) {
            applyWrapAround(buf, "*");
            handled = true;
        } else if (event.key == eui::InputKey::K) {
            applyLinkSkeleton(buf);
            handled = true;
        }
    }
    if (!handled) return false;
    commitEditBuffer(ui, buf);
    return true;
}

void MarkdownModule::commitEditBuffer(eui::Ui& ui, EditBuffer& buf) {
    using InputState = components::input_detail::InputModel::InputState;
    auto& st = ui.state<InputState>(kEditorInputId);
    if (buf.text == st.text) {
        // 纯光标/选区移动：只更新位置，不进 undo / 不 bump revision / 不打脏。
        if (buf.cursor == st.cursor && buf.selStart == st.selectionStart &&
            buf.selEnd == st.selectionEnd) {
            return;
        }
        st.cursor = buf.cursor;
        st.selectionStart = buf.selStart;
        st.selectionEnd = buf.selEnd;
        st.hasPreferredCursorX = false;
        st.followCaret = true;
        return;
    }
    components::input_detail::InputModel::pushUndoState(st);
    st.text = std::move(buf.text);
    ++st.textRevision;
    st.cursor = buf.cursor;
    st.selectionStart = buf.selStart;
    st.selectionEnd = buf.selEnd;
    st.hasPreferredCursorX = false;
    st.followCaret = true;  // 编辑后滚到光标（同内置行为）
    onEditorTextChanged(st.text);
}

void MarkdownModule::onEditorTextChanged(const std::string& v) {
    setEditorText(v);
    editorDirty_ = true;
    outlineDirty_ = true;
    lastEditAt_ = std::chrono::steady_clock::now();
}

void MarkdownModule::setEditorText(const std::string& v) {
    if (editorText_ == v) return;
    editorText_ = v;
    ++editorTextRev_;
    previewTextHash_ = std::hash<std::string>{}(editorText_);
    // 块模型：唯一文本写点顺带 update（同文零重算），编辑只脏受影响块。
    // 分块重算耗时记录式日志（无硬断言，扫日志复核）；零重算路径不打。
    const auto tSeg0 = std::chrono::steady_clock::now();
    const MdBlockUpdate upd = blockModel_.update(editorText_);
    const auto tSeg1 = std::chrono::steady_clock::now();
    if (upd.changed) {
        char msBuf[32];
        std::snprintf(msBuf, sizeof(msBuf), "%.2f",
                      std::chrono::duration<double, std::milli>(tSeg1 - tSeg0)
                          .count());
        LOG_DEBUG("[live] segment n=" +
                  std::to_string(blockModel_.blocks().size()) + " ms=" + msBuf +
                  " dirty=[" + std::to_string(upd.dirtyFirst) + "," +
                  std::to_string(upd.dirtyLast) + "], 复用 " +
                  std::to_string(upd.reused) + " 块");
    }
}

void MarkdownModule::drawTagBar(eui::Ui& ui,
                float x,
                float w,
                const core::theme::Tokens& t,
                const components::theme::ThemeColorTokens& componentTheme) {
    const float y = 16.0f;
    const float h = 24.0f;
    const float fs = 11.0f;
    float cx = x;
    for (size_t i = 0; i < currentTags_.size(); ++i) {
        const std::string& name = currentTags_[i];
        const std::string id = "md.tags.chip." + std::to_string(i);
        const float chipW = 10.0f + estimateTextWidth(name, fs) + 26.0f;
        if (cx + chipW > x + w) break;  // 溢出截断（见函数头注释）
        ui.stack(id)
            .position(cx, y)
            .size(chipW, h)
            .content([&] {
                ui.rect(id + ".bg")
                    .size(chipW, h)
                    .color(t.colors.hover)
                    .radius(t.sizes.radiusSmall)
                    .build();
                ui.text(id + ".name")
                    .position(10.0f, 0.0f)
                    .size(std::max(0.0f, chipW - 30.0f), h)
                    .text(name)
                    .fontSize(fs)
                    .lineHeight(h)
                    // 文本盒 == 胶囊高，须显式垂直居中（缺它 ink 从行框顶沿起排 → 字偏上）
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.primary)
                    .fontWeight(600)
                    .build();
                // × 删除热区（底色同 chip 不额外可见）
                ui.rect(id + ".del")
                    .position(chipW - 24.0f, 0.0f)
                    .size(24.0f, h)
                    .color(t.colors.hover)
                    .hoverColor(t.colors.primarySoftBg)
                    .radius(t.sizes.radiusSmall)
                    .onClick([this, name] { removeTag(name); })
                    .build();
                ui.text(id + ".x")
                    .position(chipW - 24.0f, 0.0f)
                    .size(24.0f, h)
                    .text("×")
                    .fontSize(fs)
                    .lineHeight(h)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    // 文本盒 == 热区高，须显式垂直居中
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.primary)
                    .build();
            })
            .build();
        cx += chipW + 6.0f;
    }
    // 内置添加输入（§3.3.4：从红点加号改为输入框内置「添加标签」入口）。
    // 回车提交（onEnter 已接 commitAddTag）；移除独立 addbtn 按钮。
    const float inW = 120.0f;
    if (cx + inW <= x + w) {
        components::input(ui, "md.tags.add")
            .position(cx, y)
            .size(inW, h)
            .value(tagInput_)
            .placeholder("添加标签")
            .fontSize(fs)
            .inset(8.0f)
            .theme(componentTheme)
            .onChange([this](const std::string& v) { tagInput_ = v; })
            .onEnter([this] { commitAddTag(); })
            .onCancel([this] { tagInput_.clear(); })
            .build();
    }
}

void MarkdownModule::drawStatusBar(eui::Ui& ui, float x, float y, float w, const core::theme::Tokens& t) {
    ui.rect("md.status.bg")
        .position(x, y)
        .size(w, 22.0f)
        .color(ui_kit::cardSurface(t))
        .build();

    std::string left = "未选择笔记";
    eui::Color leftColor = t.colors.textTertiary;
    if (!currentPath_.empty()) {
        const std::string name = fs::path(currentPath_).filename().string();
        if (saveFailed_) {
            left = name + "  ✕ 保存失败，自动重试中";
            leftColor = t.colors.errorText;
        } else if (editorDirty_) {
            left = name + "  ● 未保存";
            // UAT3 T1：未保存圆点改用 wechat 警示红（旧注释称「wechat 无对应槽」
            // 有误——warningRed #FA5151 即警示档），暖橙黄退出 md 页主体。
            leftColor = t.colors.errorText;
        } else {
            left = name + "  ✓ 已保存 " + formatSaveTime(lastSavedAt_);
            leftColor = t.colors.primary;  // 保存成功 = 绿
        }
    }
    // 左框右缘 = 右串左缘留 24px 间距
    const float statusLeftW = w - 24.0f - 220.0f - 24.0f;
    ui.text("md.status.save")
        .position(x + 24.0f, y)
        .size(statusLeftW, 22.0f)
        // 超长文件名用单行省略号；显式 maxWidth 是生效前提（==0 时 runtime 回退
        // frame.width，DSL 按 maxWidth 走，两条路会分叉）；clip 兜底绘制范围。
        .maxWidth(statusLeftW)
        .overflow(core::TextOverflow::Ellipsis)
        .clip()
        .text(left)
        .fontSize(core::theme::scaled(11.0f))
        .lineHeight(core::theme::scaled(22.0f))
        // 文本盒 == 状态栏底高，须显式垂直居中
        .verticalAlign(eui::VerticalAlign::Center)
        .color(leftColor)
        .build();

    // 行:列 · N 字 · N 篇笔记（列/字数按 UTF-8 码点计）
    std::string right;
    if (!currentPath_.empty()) {
        // (行, 列, 字数) 按 (修订, 光标, mode) 缓存，未变帧直接复用。实时态光标权威 =
        // liveCursorByte_、文本 = 整篇 editorText_（活动 input 只持块局部文本）。
        const auto& st = ui.state<components::input_detail::InputModel::InputState>(kEditorInputId);
        const std::string* text = &st.text;
        size_t cursor = static_cast<size_t>(st.cursor);
        if (mode_ == ViewMode::Live) {
            text = &editorText_;
            cursor = std::min(liveCursorByte_, editorText_.size());
        }
        if (statusCacheRev_ != editorTextRev_ ||
            statusCacheCursor_ != cursor ||
            statusCacheMode_ != static_cast<int>(mode_)) {
            const int line = byteOffsetToLine(*text, cursor);
            const size_t lineStart = lineStartOffset(*text, line);
            const int col = static_cast<int>(utf8Length(
                text->substr(lineStart, cursor - lineStart))) + 1;
            statusCacheLine_ = line;
            statusCacheCol_ = col;
            statusCacheChars_ = utf8Length(*text);
            statusCacheRev_ = editorTextRev_;
            statusCacheCursor_ = cursor;
            statusCacheMode_ = static_cast<int>(mode_);
        }
        right = std::to_string(statusCacheLine_ + 1) + ":" +
                std::to_string(statusCacheCol_) + " · " +
                std::to_string(statusCacheChars_) + " 字";
    }
    if (service_ != nullptr) {
        if (!right.empty()) right += " · ";
        right += std::to_string(notes_.size()) + " 篇笔记";
    }
    ui.text("md.status.pos")
        .position(x + w - 24.0f - 220.0f, y)
        .size(220.0f, 22.0f)
        .text(right)
        .fontSize(core::theme::scaled(11.0f))
        .lineHeight(core::theme::scaled(22.0f))
        .horizontalAlign(eui::HorizontalAlign::Right)
        // 文本盒 == 状态栏底高，须显式垂直居中
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textTertiary)
        .build();
}

void MarkdownModule::refreshTags() {
    if (!service_) {
        allTags_.clear();
        return;
    }
    auto r = service_->listAllTags();
    if (r.isOk()) {
        allTags_ = std::move(r.value());
    } else {
        LOG_WARN("加载标签失败: " + r.errorMessage());
    }
    // 筛选标签已无笔记引用 → 自动取消筛选
    if (!tagFilter_.empty()) {
        bool found = false;
        for (const auto& tc : allTags_) {
            if (tc.name == tagFilter_) {
                found = true;
                break;
            }
        }
        if (!found) tagFilter_.clear();
    }
}

void MarkdownModule::toggleTagFilter(const std::string& name) {
    tagFilter_ = (tagFilter_ == name) ? std::string() : name;
    refreshList();
    LOG_INFO("标签筛选: " + (tagFilter_.empty() ? "(无)" : tagFilter_));
}

core::Result<void> MarkdownModule::writeTags(const std::vector<std::string>& tags) {
    auto r = service_->setTags(currentPath_, tags);
    if (r.isOk()) core::event::bus().emit(core::event::kMdTagsChanged);
    return r;
}

void MarkdownModule::commitAddTag() {
    if (!service_ || currentPath_.empty()) return;
    std::string name = tagInput_;
    while (!name.empty() &&
           (name.front() == ' ' || name.front() == '\t')) {
        name.erase(name.begin());
    }
    while (!name.empty() &&
           (name.back() == ' ' || name.back() == '\t' ||
            name.back() == '\r' || name.back() == '\n')) {
        name.pop_back();
    }
    tagInput_.clear();
    if (name.empty()) return;
    for (const auto& existing : currentTags_) {
        if (existing == name) return;  // 已有，不重复
    }
    currentTags_.push_back(name);
    auto r = writeTags(currentTags_);
    if (r.isFail()) {
        currentTags_.pop_back();
        LOG_ERROR("添加标签失败 " + name + ": " + r.errorMessage());
        showToast("添加标签失败", r.errorMessage(), true);
        return;
    }
    refreshTags();
    showToast("已添加标签", name, false);
    LOG_INFO("添加标签 " + name + " → " + currentPath_);
}

void MarkdownModule::removeTag(const std::string& name) {
    if (!service_ || currentPath_.empty()) return;
    std::vector<std::string> next;
    for (const auto& tg : currentTags_) {
        if (tg != name) next.push_back(tg);
    }
    if (next.size() == currentTags_.size()) return;
    auto r = writeTags(next);
    if (r.isFail()) {
        LOG_ERROR("删除标签失败 " + name + ": " + r.errorMessage());
        showToast("删除标签失败", r.errorMessage(), true);
        return;
    }
    currentTags_ = std::move(next);
    refreshTags();
    refreshList();  // 筛选标签可能随删除失效，树要同步
    showToast("已删除标签", name, false);
    LOG_INFO("删除标签 " + name + " ← " + currentPath_);
}

void MarkdownModule::flushEditorIfIdle() {
    if (conflictOpen_) return;
    if (!editorDirty_) return;
    const auto now = std::chrono::steady_clock::now();
    const auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - lastEditAt_).count();
    if (idleMs < kAutoSaveDelayMs) return;
    const auto sinceAttemptMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now - lastSaveAttemptAt_).count();
    if (sinceAttemptMs < saveBackoffMs_) return;
    lastSaveAttemptAt_ = now;
    saveEditor();
}

void MarkdownModule::saveEditor() {
    if (!service_ || currentPath_.empty()) return;
    if (editorText_ == lastSavedText_) {
        editorDirty_ = false;
        saveFailed_ = false;
        return;
    }
    auto r = service_->write(currentPath_, editorText_);
    if (r.isFail()) {
        LOG_ERROR("保存笔记失败 " + currentPath_ + ": " + r.errorMessage());
        saveFailed_ = true;
        // 500ms → 1s → 2s → … 封顶 60s
        saveBackoffMs_ = (saveBackoffMs_ == 0)
            ? 500 : std::min(saveBackoffMs_ * 2, kMaxSaveBackoffMs);
        // 失败重试期间只在无 toast 展示时弹一次
        if (!toast_.open) {
            showToast("保存失败",
                      "「" + currentPath_ + "」" + r.errorMessage() +
                          "，已自动重试",
                      true);
        }
        return;
    }
    lastSavedText_ = editorText_;
    editorDirty_ = false;
    saveFailed_ = false;
    saveBackoffMs_ = 0;
    lastSavedAt_ = std::time(nullptr);
    syncCurrentMtime();  // 自己写盘后推进 mtime 基准（避免误报外部修改）
    // 写盘成功 = 本地版本已在磁盘上 —— 若冲突 toast 还开着，它的另一半答案
    //（「重载磁盘版本」）会把刚保存的内容再丢掉，选项与现实不符：此刻等价于用户
    // 选了「保留本地」（✕ 关闭那条），立刻收掉。自动链路（flushEditorIfIdle）
    // 在 conflictOpen_ 时根本不写，所以只有显式保存路径（Ctrl+S / 切换笔记 /
    // 退出模块）会走到这里 —— 外部版本仍可从笔记 git 历史 / 远端找回。
    if (conflictOpen_) resolveConflict(false);
    core::event::bus().emit(core::event::kMdSaved);
    // 写盘成功 → 触发 git 自动提交（节流判定在 onFileSaved 内，<5 分钟不重复）
    if (noteGit_) {
        noteGit_->onFileSaved(lastSavedAt_);
    }
}

void MarkdownModule::syncCurrentMtime() {
    if (currentPath_.empty()) {
        lastKnownMtime_ = 0;
        return;
    }
    auto m = service_->fileMtime(currentPath_);
    lastKnownMtime_ = m.isOk() ? m.value() : -1;
}

} // namespace module::markdown
