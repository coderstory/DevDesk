// Markdown 模块·菜单轮询块（MarkdownModule 成员函数定义）：新建/删除/重命名弹窗、
// 右键菜单、磁盘轮询与外部修改冲突处理。
// 拆分第一步（抽编译单元）产物：类定义在 markdown_module_internal.h，
// 本文件只承载成员函数体，纯代码搬移，逻辑与签名零变化。

#include "markdown_module_internal.h"

#include <utility>  // std::move（check-std-includes：不靠传递包含）

namespace module::markdown {

void MarkdownModule::drawNewNoteDialog(eui::Ui& ui,
                       const eui::Screen& screen,
                       const core::theme::Tokens& t,
                       const components::theme::ThemeColorTokens& componentTheme) {
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
    const float h = 220.0f;
    // M-1（UAT2 一审）：弹窗内控件统一 wechat 皮肤；页面暖橙 componentTheme
    // 有意只喂页面主体（compose/drawHeader 两处页面变量），popup 控件就地取 wechat 令牌。
    const auto wcTheme = core::theme::componentTokensWeChat(t);

    ui_kit::DialogFrame frame;
    frame.id = "md.dialog.new";
    frame.width = ui_kit::DialogWidth::Standard;
    frame.height = h;
    frame.open = newNoteOpen_;
    frame.onOpenChange = [this](bool v) { newNoteOpen_ = v; };

    ui_kit::drawDialogFrame(ui, screen, frame, core::theme::componentTokensWeChat(t), [&] {
            ui_kit::drawDialogTitle(ui, "md.dialog.new.title", w, "新建笔记",
                                    20.0f, t.colors.textPrimary);

            // 提示行用就地判据而非 newItemParentDir()（后者每帧 WARN 刷屏）
            const std::string newNoteParent =
                dirExistsOnDisk(newItemTargetDir_) ? newItemTargetDir_ : std::string();
            ui_kit::drawDialogText(ui, "md.dialog.new.hint", w,
                                   "保存到：" +
                                       (newNoteParent.empty()
                                            ? std::string("根目录")
                                            : newNoteParent) +
                                       " · 名字可带子目录，如 工作/周会.md",
                                   t.sizes.fontSizeSmall, t.colors.textTertiary,
                                   {.y = 56.0f, .height = 18.0f, .lineHeight = 18.0f});

            components::input(ui, "md.dialog.new.input")
                .position(28.0f, 84.0f)
                .size(w - 56.0f, 44.0f)
                .value(newNoteName_)
                .placeholder("笔记名，可用 / 分文件夹，如 工作/周会")
                .fontSize(t.sizes.fontSizeBody)
                .theme(wcTheme)
                .onChange([this](const std::string& v) {
                    newNoteName_ = v;
                    newNoteError_.clear();  // 错误随下一次输入清除
                })
                .onEnter([this, &ui] { commitNewNote(ui); })
                // Esc=取消（关弹窗不落盘，同「取消」按钮）
                .onCancel([this] { newNoteOpen_ = false; })
                .build();

            // 内嵌红字错误行（校验/创建失败不走 toast）
            ui_kit::drawDialogText(ui, "md.dialog.new.error", w, newNoteError_, 12.0f,
                                   t.colors.errorText,
                                   {.y = 134.0f, .height = 18.0f, .lineHeight = 18.0f,
                                    .skipIfEmpty = true});

            ui_kit::DialogButtonSpec cancel;
            cancel.label = "取消";
            cancel.fontSize = t.sizes.fontSizeBody;
            cancel.style = ui_kit::DialogButtonStyle::SecondaryThemed;
            cancel.onClick = [this] { newNoteOpen_ = false; };
            ui_kit::DialogButtonSpec confirm;
            confirm.label = "创建";
            confirm.fontSize = t.sizes.fontSizeBody;
            confirm.style = ui_kit::DialogButtonStyle::PrimaryThemed;
            confirm.onClick = [this, &ui] { commitNewNote(ui); };
            ui_kit::drawDialogButtons(ui, "md.dialog.new", w, h, cancel, confirm, t,
                                      core::theme::componentTokensWeChat(t));
    });
}

void MarkdownModule::drawNewFolderDialog(eui::Ui& ui,
                         const eui::Screen& screen,
                         const core::theme::Tokens& t,
                         const components::theme::ThemeColorTokens& componentTheme) {
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
    const float h = 320.0f;
    // M-1（UAT2 一审）：弹窗内控件（下拉/输入框）统一 wechat 皮肤。
    const auto wcTheme = core::theme::componentTokensWeChat(t);

    // 父目录候选：根目录 + 磁盘上每一级目录
    const auto candidates = folderParentCandidates();
    std::vector<std::string> parents;
    parents.reserve(candidates.size());
    for (const auto& c : candidates) parents.push_back(c.first);
    // 已不在候选 → 0 = 根目录（显示 == 行为）
    const int parentIdx = folderParentIndexFor(candidates, newFolderParentDir_);

    ui_kit::DialogFrame frame;
    frame.id = "md.dialog.newfolder";
    frame.width = ui_kit::DialogWidth::Standard;
    frame.height = h;
    frame.open = newFolderOpen_;
    frame.onOpenChange = [this](bool v) {
        newFolderOpen_ = v;
        if (!v) newFolderParentOpen_ = false;  // 关弹窗顺手收下拉
    };

    ui_kit::drawDialogFrame(ui, screen, frame, core::theme::componentTokensWeChat(t), [&] {
            ui_kit::drawDialogTitle(ui, "md.dialog.newfolder.title", w, "新建文件夹",
                                    20.0f, t.colors.textPrimary);

            // 提示行带上实际落点（= 当前上级目录），避免「显示 A 建到 B」。
            ui_kit::drawDialogText(ui, "md.dialog.newfolder.hint", w,
                                   "创建在：" +
                                       parents[static_cast<size_t>(parentIdx)] +
                                       "/ · 空文件夹也会显示在树里",
                                   t.sizes.fontSizeSmall, t.colors.textTertiary,
                                   {.y = 56.0f, .height = 18.0f, .lineHeight = 18.0f});

            ui_kit::drawDialogText(ui, "md.dialog.newfolder.parent.label", w, "父目录",
                                   12.0f, t.colors.textTertiary,
                                   {.y = 84.0f, .height = 16.0f, .lineHeight = 16.0f});

            // 下拉弹层自带 zIndex=20，包一层定位 stack 即可
            ui.stack("md.dialog.newfolder.parent")
                .position(28.0f, 102.0f)
                .size(w - 56.0f, 40.0f)
                .content([&] {
                    components::dropdown(ui, "md.dialog.newfolder.parent.dd")
                        .size(w - 56.0f, 40.0f)
                        .items(parents)
                        .selected(parentIdx)
                        .placeholder("根目录")
                        .theme(wcTheme)
                        .open(newFolderParentOpen_)
                        .onOpenChange(
                            [this](bool v) { newFolderParentOpen_ = v; })
                        // 命中即按当帧候选的路径落状态（capture by value），列表后续变化不影响本次选择
                        .onChange([this, candidates](int idx) {
                            if (idx < 0 ||
                                static_cast<size_t>(idx) >= candidates.size()) {
                                return;
                            }
                            newFolderParentDir_ = candidates[static_cast<size_t>(idx)].second;
                        })
                        .build();
                })
                .build();

            ui_kit::drawDialogText(ui, "md.dialog.newfolder.name.label", w, "文件夹名",
                                   12.0f, t.colors.textTertiary,
                                   {.y = 156.0f, .height = 16.0f, .lineHeight = 16.0f});

            components::input(ui, "md.dialog.newfolder.input")
                .position(28.0f, 174.0f)
                .size(w - 56.0f, 44.0f)
                .value(newFolderName_)
                .placeholder("文件夹名，如 工作")
                .fontSize(t.sizes.fontSizeBody)
                .theme(wcTheme)
                .onChange([this](const std::string& v) {
                    newFolderName_ = v;
                    newFolderError_.clear();  // 错误随下一次输入清除
                })
                .onEnter([this] { commitNewFolder(); })
                // Esc=取消
                .onCancel([this] { newFolderOpen_ = false; })
                .build();

            // 内嵌红字错误行，同新建笔记弹窗
            ui_kit::drawDialogText(ui, "md.dialog.newfolder.error", w, newFolderError_,
                                   12.0f, t.colors.errorText,
                                   {.y = 228.0f, .height = 18.0f, .lineHeight = 18.0f,
                                    .skipIfEmpty = true});

            ui_kit::DialogButtonSpec cancel;
            cancel.label = "取消";
            cancel.fontSize = t.sizes.fontSizeBody;
            cancel.style = ui_kit::DialogButtonStyle::SecondaryThemed;
            cancel.onClick = [this] { newFolderOpen_ = false; };
            ui_kit::DialogButtonSpec confirm;
            confirm.label = "创建";
            confirm.fontSize = t.sizes.fontSizeBody;
            confirm.style = ui_kit::DialogButtonStyle::PrimaryThemed;
            confirm.onClick = [this] { commitNewFolder(); };
            ui_kit::drawDialogButtons(ui, "md.dialog.newfolder", w, h, cancel, confirm, t,
                                      core::theme::componentTokensWeChat(t));
    });
}

void MarkdownModule::pollDiskChangesIfDue() {
    if (!service_) return;
    const auto now = std::chrono::steady_clock::now();
    const auto sinceMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now - lastPollAt_).count();
    if (!service_->hasWatchCredit() && sinceMs < pollIntervalMs_) return;
    lastPollAt_ = now;

    // 1) 目录级轻量比对：有差异才做全量 rescan
    auto changed = service_->pollChanged();
    if (changed.isOk() && changed.value()) {
        auto r = service_->rescan();
        if (r.isOk()) {
            listError_.clear();
            refreshList();
        } else {
            LOG_WARN("自动重扫失败: " + r.errorMessage());
        }
    }
    // 2) 当前笔记的外部修改 / 删除检测
    checkCurrentNoteOnDisk();
}

void MarkdownModule::checkCurrentNoteOnDisk() {
    if (currentPath_.empty() || conflictOpen_) return;
    auto m = service_->fileMtime(currentPath_);
    if (m.isFail()) {
        // 文件被外部删除：lastKnownMtime_ = -1 做已知缺失标记（防重复 toast）
        if (lastKnownMtime_ == -1) return;
        lastKnownMtime_ = -1;
        if (!editorDirty_) {
            currentPath_.clear();
            setEditorText(std::string());
            lastSavedText_.clear();
            outlineDirty_ = true;
            activeOutlineIdx_ = -1;
            saveViewState();  // 已删笔记不该在下次启动被恢复
            showToast("笔记已被删除", "当前笔记已从磁盘移除", true);
        } else {
            // 有未保存编辑：write() 会重建文件，提示一次即可
            showToast("笔记已被外部删除",
                      "保留的本地编辑会在保存时重新创建该文件", true);
        }
        return;
    }
    if (m.value() > lastKnownMtime_) {
        if (!editorDirty_) {
            // 无本地改动：静默重载
            reloadCurrentNote(true);
        } else {
            // 有未保存编辑：弹「重载 / 保留本地」选择（只弹一次）
            conflictOpen_ = true;
        }
    }
}

void MarkdownModule::reloadCurrentNote(bool silent) {
    auto r = service_->read(currentPath_);
    if (r.isFail()) {
        if (!silent) {
            showToast("重载失败", r.errorMessage(), true);
        }
        return;
    }
    // 步骤 1（setEditorText 之前）：记下活动块内容身份五元组
    struct CapturedBlock {
        bool valid = false;
        MdBlockKind kind = MdBlockKind::Paragraph;
        uint64_t hash = 0;
        int stableId = -1;
        size_t start = 0;
        size_t local = 0;   // 块内字节偏移（步骤 3 重定位要写回锚）
    } captured;
    if (mode_ == ViewMode::Live && liveCursorValid_ &&
        !blockModel_.blocks().empty()) {
        const size_t idx = mdLiveActiveBlockIndex(blockModel_, liveCursorByte_);
        const MdBlock& b = blockModel_.blocks()[idx];
        captured = {true, b.kind, b.hash, b.stableId, b.start,
                    mdLiveBlockLocalOffset(b, liveCursorByte_)};
    }
    setEditorText(r.value());
    // 步骤 3（setEditorText 之后、resetLiveUndoBaseline 之前）：两分支都
    // 显式写 liveCursorByte_，撤销基线才读到重定位后的字节。
    if (captured.valid) {
        const std::vector<MdBlock>& bs = blockModel_.blocks();
        const size_t newIdx =
            mdFindBlockByContent(bs, captured.kind, captured.hash,
                                 captured.stableId, captured.start);
        if (newIdx != std::string::npos) {
            // 命中：liveCursorByte_ = 新块起点 + 块内光标（钳进块长）。
            // liveInputStateFresh_ 与 liveFocusPending_ 成对（释放清焦点须同帧补发）。
            const MdBlock& nb = bs[newIdx];
            const size_t len = nb.end > nb.start ? nb.end - nb.start : 0;
            liveActiveBlock_ = newIdx;
            liveInputStart_ = nb.start;
            liveCursorByte_ = nb.start + std::min(captured.local, len);
            liveCursorValid_ = true;
            liveInputStateFresh_ = true;
            liveFocusPending_ = true;
        } else {
            // 零命中：内容被删改消失 / 文档变空。钳位到旧光标对应新块（保守选择）；
            // 文档变空时 blocks()[0] 越界，单列分支归零。
            if (bs.empty()) {
                liveActiveBlock_ = 0;
                liveInputStart_ = 0;
                liveCursorByte_ = 0;
            } else {
                const size_t clamped =
                    std::min(liveCursorByte_, blockModel_.text().size());
                liveActiveBlock_ = blockModel_.blockIndexAt(clamped);
                liveInputStart_ = bs[liveActiveBlock_].start;
                liveCursorByte_ = clamped;
            }
            liveCursorValid_ = true;
            liveInputStateFresh_ = true;
            liveFocusPending_ = true;
        }
    }
    // 外部重载 = 撤销基线换代（不换代时 Ctrl+Z 会把外部改动整篇盖回本地旧版）
    if (mode_ == ViewMode::Live) {
        resetLiveUndoBaseline();
    } else {
        liveUndoSeeded_ = false;
    }
    lastSavedText_ = editorText_;
    editorDirty_ = false;
    outlineDirty_ = true;
    activeOutlineIdx_ = -1;
    syncCurrentMtime();
    if (mode_ == ViewMode::Preview) syncPreviewOnEnter_ = true;
    if (silent) {
        // 静默重载必须弹 toast（屏幕内容变了）；冲突路径不弹
        //（action 点击后 toast 必关，立刻再弹会被 onDismiss 连带清掉）。
        showToast("检测到外部修改，已自动重载", currentPath_, false);
    }
    LOG_INFO("重载笔记: " + currentPath_);
}

void MarkdownModule::resolveConflict(bool reloadFromDisk) {
    conflictOpen_ = false;
    if (reloadFromDisk) {
        // 重载本身即可见反馈（action 点击后 toast 必关，立刻再弹会被清掉）
        reloadCurrentNote(false);
    } else {
        // 保留本地：基准 mtime 推到磁盘当前值，未保存编辑走自动保存链路
        syncCurrentMtime();
        LOG_INFO("冲突处理：保留本地编辑 " + currentPath_);
    }
}

void MarkdownModule::openFileContextMenu(const std::string& relPath, const eui::PointerEvent& ev) {
    ctxMenuPath_ = relPath;
    const core::MenuAnchor anchor = menuAnchorForPointerInPage(ev);
    ctxMenuX_ = anchor.x;
    ctxMenuY_ = anchor.y;
    ctxMenuOpen_ = true;
}

void MarkdownModule::openDirContextMenu(const std::string& relPath, const eui::PointerEvent& ev) {
    dirCtxPath_ = relPath;
    const core::MenuAnchor anchor = menuAnchorForPointerInPage(ev);
    ctxMenuX_ = anchor.x;
    ctxMenuY_ = anchor.y;
    dirCtxOpen_ = true;
}

void MarkdownModule::drawNoteContextMenu(eui::Ui& ui,
                         const eui::Screen& screen,
                         const core::theme::Tokens& t,
                         const components::theme::ThemeColorTokens& componentTheme) {
    // M-1（UAT2 一审）：右键菜单属 popup，统一 wechat 皮肤；页面暖橙
    // componentTheme 只喂页面主体。
    const auto wcTheme = core::theme::componentTokensWeChat(t);
// 无右键目标时跳过 build 并强制置关：否则组件不渲染 dismiss 层，
// 点击外部无处触发 onOpenChange(false)。
    if (ctxMenuPath_.empty()) {
        ctxMenuOpen_ = false;
        return;
    }
    components::contextMenu(ui, "md.ctx")
        .open(ctxMenuOpen_)
        .screen(screen.width, screen.height)
        .position(ctxMenuX_, ctxMenuY_)
        .autoWidth()  // 宽度按最宽项自适应
        .items({"重命名", "删除笔记"})
        .theme(wcTheme)
        .onSelect([this](int index) {
            if (index == 0) {
                renameOpen_ = true;
                renameTargetPath_ = ctxMenuPath_;
                renameInput_ = ctxMenuPath_;  // 预填相对路径（含文件夹）
                renameError_.clear();         // 打开清残留错误
            } else if (index == 1) {
                deleteConfirmOpen_ = true;
                deleteTargetPath_ = ctxMenuPath_;
            }
        })
        .onOpenChange([this](bool v) { ctxMenuOpen_ = v; })
        .build();
}

void MarkdownModule::drawFolderContextMenu(eui::Ui& ui,
                            const eui::Screen& screen,
                            const core::theme::Tokens& t,
                            const components::theme::ThemeColorTokens& componentTheme) {
    // 与笔记右键菜单同款 wechat 皮肤；两菜单互斥（笔记菜单开着时强制关目录菜单）
    const auto wcTheme = core::theme::componentTokensWeChat(t);
    if (ctxMenuOpen_) {
        dirCtxOpen_ = false;
        return;
    }
    if (dirCtxPath_.empty()) {
        dirCtxOpen_ = false;
        return;
    }
    components::contextMenu(ui, "md.dir.ctx")
        .open(dirCtxOpen_)
        .screen(screen.width, screen.height)
        .position(ctxMenuX_, ctxMenuY_)
        .autoWidth()  // 宽度按最宽项自适应
        .items({"新建子目录", "新建子笔记", "重命名", "删除"})
        .theme(wcTheme)
        .onSelect([this](int index) {
            if (index == 0) {
                // 新建子目录：弹窗的上级目录预置为右键的文件夹，并展开它
                collapsedDirs_.erase(dirCtxPath_);
                newFolderOpen_ = true;
                newFolderName_.clear();
                newFolderError_.clear();
                newFolderParentDir_ = dirCtxPath_;
                newFolderParentOpen_ = false;
            } else if (index == 1) {
                // 新建子笔记：新建落点改到右键的文件夹，并展开它
                collapsedDirs_.erase(dirCtxPath_);
                newItemTargetDir_ = dirCtxPath_;
                selectDir(dirCtxPath_);
                openNewNoteDialog();
            } else if (index == 2) {
                dirRenameOpen_ = true;
                dirRenameTargetPath_ = dirCtxPath_;
                // 同级改名：预填文件夹名本身（不含父路径，含 / 会报错）
                const size_t slash = dirCtxPath_.rfind('/');
                dirRenameInput_ =
                    slash == std::string::npos
                        ? dirCtxPath_
                        : dirCtxPath_.substr(slash + 1);
                dirRenameError_.clear();  // 打开清残留错误
            } else if (index == 3) {
                // 删除判定：递归数 .md；非空 → 明确提示不静默，空 → 确认弹窗
                if (!service_) return;
                auto cnt = service_->folderNoteCount(dirCtxPath_);
                if (cnt.isFail()) {
                    showToast("删除失败",
                              "「" + dirCtxPath_ + "」" + cnt.errorMessage(),
                              true);
                    return;
                }
                if (cnt.value() > 0) {
                    showToast("不能删除",
                              "文件夹「" + dirCtxPath_ + "」内有 " +
                                  std::to_string(cnt.value()) + " 篇笔记",
                              true);
                    return;
                }
                dirDeleteConfirmOpen_ = true;
                dirDeleteTargetPath_ = dirCtxPath_;
            }
        })
        .onOpenChange([this](bool v) { dirCtxOpen_ = v; })
        .build();
}

std::vector<components::ContextMenuItem> MarkdownModule::buildTableMenuItems() {
    std::vector<components::ContextMenuItem> rowItems;
    for (int r = 1; r <= 6; ++r) {
        std::vector<components::ContextMenuItem> colItems;
        for (int c = 1; c <= 8; ++c) {
            colItems.emplace_back(std::to_string(c) + " 列");
        }
        rowItems.emplace_back(std::to_string(r) + " 行", std::move(colItems));
    }
    std::vector<components::ContextMenuItem> items;
    items.emplace_back("插入表格", std::move(rowItems));
    return items;
}

void MarkdownModule::drawEditorContextMenu(eui::Ui& ui,
                           const eui::Screen& screen,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& componentTheme) {
    // M-1（UAT2 一审）：编辑器右键菜单属 popup，统一 wechat 皮肤。
    const auto wcTheme = core::theme::componentTokensWeChat(t);
    // 只在打开时构建（关闭态无覆盖层残留）
    if (!editorCtxOpen_) return;
    components::contextMenu(ui, "md.editor.ctx")
        .open(editorCtxOpen_)
        .screen(screen.width, screen.height)
        .position(editorCtxX_, editorCtxY_)
        .autoWidth()  // 宽度按最宽项自适应
        .items(buildTableMenuItems())
        .theme(wcTheme)
        .onSelectPath([this, &ui](const std::vector<int>& path) {
            // path = [0(插入表格), 行下标, 列下标]
            if (path.size() == 3 && path[0] == 0) {
                insertTableAtCursor(ui, path[1] + 1, path[2] + 1);
            }
        })
        .onOpenChange([this](bool v) { editorCtxOpen_ = v; })
        .build();
}

void MarkdownModule::insertTableAtCursor(eui::Ui& ui, int rows, int cols) {
    using InputState = components::input_detail::InputModel::InputState;
    if (!service_ || currentPath_.empty()) return;
    if (ui.find(kEditorInputHitId) == nullptr) return;  // 预览态无编辑器
    auto& st = ui.state<InputState>(kEditorInputId);
    EditBuffer buf{st.text, st.cursor, st.selectionStart, st.selectionEnd};
    module::markdown::applyInsertTable(buf, rows, cols);
    commitEditBuffer(ui, buf);
    ui.requestFocus(kEditorInputHitId);
}

void MarkdownModule::drawDeleteDialog(eui::Ui& ui,
                      const eui::Screen& screen,
                      const core::theme::Tokens& t,
                      const components::theme::ThemeColorTokens& componentTheme) {
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
    const float h = 200.0f;

    ui_kit::DialogFrame frame;
    frame.id = "md.dialog.del";
    frame.width = ui_kit::DialogWidth::Standard;
    frame.height = h;
    frame.open = deleteConfirmOpen_;
    frame.onOpenChange = [this](bool v) { deleteConfirmOpen_ = v; };

    ui_kit::drawDialogFrame(ui, screen, frame, core::theme::componentTokensWeChat(t), [&] {
            ui_kit::drawDialogTitle(ui, "md.dialog.del.title", w, "删除笔记",
                                    20.0f, t.colors.textPrimary);
            ui_kit::drawDialogText(
                ui, "md.dialog.del.body", w,
                "将删除笔记「" + deleteTargetPath_ + "」及其磁盘文件，不可恢复。",
                t.sizes.fontSizeBody - 1.0f, t.colors.textSecondary,
                {.y = 60.0f, .height = 40.0f, .lineHeight = 20.0f, .wrap = true});

            ui_kit::DialogButtonSpec cancel;
            cancel.label = "取消";
            cancel.fontSize = t.sizes.fontSizeBody - 1.0f;
            cancel.style = ui_kit::DialogButtonStyle::SecondaryThemed;
            cancel.onClick = [this] { deleteConfirmOpen_ = false; };
            ui_kit::DialogButtonSpec confirm;
            confirm.label = "删除";
            confirm.fontSize = t.sizes.fontSizeBody - 1.0f;
            confirm.style = ui_kit::DialogButtonStyle::DangerThemed;
            confirm.onClick = [this] { doDeleteNote(); };
            ui_kit::drawDialogButtons(ui, "md.dialog.del", w, h, cancel, confirm, t,
                                      core::theme::componentTokensWeChat(t));
    });
}

void MarkdownModule::drawRenameDialog(eui::Ui& ui,
                      const eui::Screen& screen,
                      const core::theme::Tokens& t,
                      const components::theme::ThemeColorTokens& componentTheme) {
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
    const float h = 220.0f;
    // M-1（UAT2 一审）：同类的第七处——重命名弹窗输入框此前也漏吃页面暖橙，
    // 逐处 Read 核对 popup 控件时补上（frame/buttons 本已是 wechat）。
    const auto wcTheme = core::theme::componentTokensWeChat(t);

    ui_kit::DialogFrame frame;
    frame.id = "md.dialog.rename";
    frame.width = ui_kit::DialogWidth::Standard;
    frame.height = h;
    frame.open = renameOpen_;
    frame.onOpenChange = [this](bool v) { renameOpen_ = v; };

    ui_kit::drawDialogFrame(ui, screen, frame, core::theme::componentTokensWeChat(t), [&] {
            ui_kit::drawDialogTitle(ui, "md.dialog.rename.title", w, "重命名笔记",
                                    20.0f, t.colors.textPrimary);
            ui_kit::drawDialogText(ui, "md.dialog.rename.hint", w,
                                   "相对笔记目录；填「工作/新名」可同时移动。"
                                   "笔记一律以 .md 结尾：不写会自动补上",
                                   t.sizes.fontSizeSmall, t.colors.textTertiary,
                                   {.y = 56.0f, .height = 18.0f, .lineHeight = 18.0f});
            components::input(ui, "md.dialog.rename.input")
                .position(28.0f, 84.0f)
                .size(w - 56.0f, 44.0f)
                .value(renameInput_)
                .placeholder("新名字或新路径")
                .fontSize(t.sizes.fontSizeBody)
                .theme(wcTheme)
                .onChange([this](const std::string& v) {
                    renameInput_ = v;
                    renameError_.clear();  // 错误随下一次输入清除
                })
                .onEnter([this] { commitRenameNote(); })
                // Esc=取消（关弹窗不改名）
                .onCancel([this] { renameOpen_ = false; })
                .build();
            // 校验/落盘失败内嵌红字，弹窗保持打开让用户改输入
            ui_kit::drawDialogText(ui, "md.dialog.rename.error", w, renameError_,
                                   12.0f, t.colors.errorText,
                                   {.y = 134.0f, .height = 18.0f, .lineHeight = 18.0f,
                                    .skipIfEmpty = true});
            ui_kit::DialogButtonSpec cancel;
            cancel.label = "取消";
            cancel.fontSize = t.sizes.fontSizeBody;
            cancel.style = ui_kit::DialogButtonStyle::SecondaryThemed;
            cancel.onClick = [this] { renameOpen_ = false; };
            ui_kit::DialogButtonSpec confirm;
            confirm.label = "重命名";
            confirm.fontSize = t.sizes.fontSizeBody;
            confirm.style = ui_kit::DialogButtonStyle::PrimaryThemed;
            confirm.onClick = [this] { commitRenameNote(); };
            ui_kit::drawDialogButtons(ui, "md.dialog.rename", w, h, cancel, confirm, t,
                                      core::theme::componentTokensWeChat(t));
    });
}

void MarkdownModule::doDeleteNote() {
    const std::string path = deleteTargetPath_;
    deleteConfirmOpen_ = false;
    deleteTargetPath_.clear();
    if (!service_ || path.empty()) return;
    auto r = service_->remove(path);
    if (r.isFail()) {
        LOG_ERROR("删除笔记失败 " + path + ": " + r.errorMessage());
        showToast("删除失败", "「" + path + "」" + r.errorMessage(), true);
        return;
    }
    // 删的是当前笔记 → 清空编辑器并清 dirty：文件已不存在，未保存编辑作废，
    // 不做「删了又写回去」的复活。
    if (currentPath_ == path) {
        currentPath_.clear();
        setEditorText(std::string());
        lastSavedText_.clear();
        editorDirty_ = false;
        saveFailed_ = false;
        saveViewState();  // 删的是当前笔记 → 视图状态同步清空
        saveBackoffMs_ = 0;
        outlineDirty_ = true;
        activeOutlineIdx_ = -1;
        currentTags_.clear();
        lastKnownMtime_ = 0;
        conflictOpen_ = false;
    }
    // 方案 A：从标签页列表中移除已删除笔记
    auto it = std::find(openNotes_.begin(), openNotes_.end(), path);
    if (it != openNotes_.end()) openNotes_.erase(it);
    refreshList();
    showToast("已删除笔记", fs::path(path).filename().string(), false);
    LOG_INFO("已删除笔记: " + path);
    core::event::bus().emit(core::event::kMdDeleted);
}

void MarkdownModule::commitRenameNote() {
    std::string target = renameInput_;
    while (!target.empty() && (target.front() == ' ' || target.front() == '\t')) {
        target.erase(target.begin());
    }
    while (!target.empty() &&
           (target.back() == ' ' || target.back() == '\t' ||
            target.back() == '\r' || target.back() == '\n')) {
        target.pop_back();
    }
    if (target.empty()) {
        // 空名内嵌红字（统一各弹窗校验反馈），弹窗保持打开让用户重填
        renameError_ = "新名字不能为空";
        return;
    }
    if (!service_ || renameTargetPath_.empty()) {
        renameOpen_ = false;
        return;
    }
    if (!MarkdownService::isSafeNewNotePath(target)) {
        renameError_ =
            "名称不合法：不支持绝对路径、盘符（如 C:）或 ..，请用相对路径";
        return;  // 弹窗保持打开让用户改
    }
    // 扩展名只在 service 层规范化，且是追加而非替换 ——
    // 索引/树/搜索只认 .md，替换会静默丢用户敲进去的字符。
    auto r = service_->rename(renameTargetPath_, target);
    if (r.isFail()) {
        LOG_ERROR("重命名笔记失败 " + renameTargetPath_ + " → " + target +
                  ": " + r.errorMessage());
        renameError_ = "重命名失败：" + r.errorMessage();
        return;
    }
    const std::string oldPath = renameTargetPath_;
    const std::string newPath = r.value();
    renameOpen_ = false;
    renameTargetPath_.clear();
    renameInput_.clear();
    renameError_.clear();
    if (currentPath_ == oldPath) {
        // 编辑器继续跟随（未保存编辑的自动保存写到新路径）
        currentPath_ = newPath;
        syncCurrentMtime();
        saveViewState();  // 重命名后恢复新路径
    }
    // 方案 A：标签页列表中的旧路径也同步重命名为新路径
    for (auto& p : openNotes_) {
        if (p == oldPath) p = newPath;
    }
    refreshList();
    showToast("已重命名", newPath, false);
    LOG_INFO("重命名笔记: " + oldPath + " → " + newPath);
    core::event::bus().emit(core::event::kMdRenamed);
}

void MarkdownModule::drawFolderDeleteDialog(eui::Ui& ui,
                             const eui::Screen& screen,
                             const core::theme::Tokens& t,
                             const components::theme::ThemeColorTokens& componentTheme) {
    // 复用笔记删除确认弹窗同款 ui_kit 基建（frame/title/text/buttons 四件）
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
    const float h = 200.0f;

    ui_kit::DialogFrame frame;
    frame.id = "md.dialog.deldir";
    frame.width = ui_kit::DialogWidth::Standard;
    frame.height = h;
    frame.open = dirDeleteConfirmOpen_;
    frame.onOpenChange = [this](bool v) { dirDeleteConfirmOpen_ = v; };

    ui_kit::drawDialogFrame(ui, screen, frame, core::theme::componentTokensWeChat(t), [&] {
            ui_kit::drawDialogTitle(ui, "md.dialog.deldir.title", w, "删除文件夹",
                                    20.0f, t.colors.textPrimary);
            ui_kit::drawDialogText(
                ui, "md.dialog.deldir.body", w,
                "将删除文件夹「" + dirDeleteTargetPath_ + "」及其全部子文件夹，不可恢复。",
                t.sizes.fontSizeBody - 1.0f, t.colors.textSecondary,
                {.y = 60.0f, .height = 40.0f, .lineHeight = 20.0f, .wrap = true});

            ui_kit::DialogButtonSpec cancel;
            cancel.label = "取消";
            cancel.fontSize = t.sizes.fontSizeBody - 1.0f;
            cancel.style = ui_kit::DialogButtonStyle::SecondaryThemed;
            cancel.onClick = [this] { dirDeleteConfirmOpen_ = false; };
            ui_kit::DialogButtonSpec confirm;
            confirm.label = "删除";
            confirm.fontSize = t.sizes.fontSizeBody - 1.0f;
            confirm.style = ui_kit::DialogButtonStyle::DangerThemed;
            confirm.onClick = [this] { doDeleteFolder(); };
            ui_kit::drawDialogButtons(ui, "md.dialog.deldir", w, h, cancel, confirm, t,
                                      core::theme::componentTokensWeChat(t));
    });
}

void MarkdownModule::drawFolderRenameDialog(eui::Ui& ui,
                             const eui::Screen& screen,
                             const core::theme::Tokens& t,
                             const components::theme::ThemeColorTokens& componentTheme) {
    // 复用笔记重命名弹窗同款 ui_kit 基建；差异只在标题/提示/同级改名校验
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
    const float h = 220.0f;
    const auto wcTheme = core::theme::componentTokensWeChat(t);

    ui_kit::DialogFrame frame;
    frame.id = "md.dialog.renamedir";
    frame.width = ui_kit::DialogWidth::Standard;
    frame.height = h;
    frame.open = dirRenameOpen_;
    frame.onOpenChange = [this](bool v) { dirRenameOpen_ = v; };

    ui_kit::drawDialogFrame(ui, screen, frame, core::theme::componentTokensWeChat(t), [&] {
            ui_kit::drawDialogTitle(ui, "md.dialog.renamedir.title", w, "重命名文件夹",
                                    20.0f, t.colors.textPrimary);
            ui_kit::drawDialogText(ui, "md.dialog.renamedir.hint", w,
                                   "同级改名：只改文件夹名，新名不能包含 /",
                                   t.sizes.fontSizeSmall, t.colors.textTertiary,
                                   {.y = 56.0f, .height = 18.0f, .lineHeight = 18.0f});
            components::input(ui, "md.dialog.renamedir.input")
                .position(28.0f, 84.0f)
                .size(w - 56.0f, 44.0f)
                .value(dirRenameInput_)
                .placeholder("新文件夹名")
                .fontSize(t.sizes.fontSizeBody)
                .theme(wcTheme)
                .onChange([this](const std::string& v) {
                    dirRenameInput_ = v;
                    dirRenameError_.clear();  // 错误随下一次输入清除
                })
                .onEnter([this] { commitRenameFolder(); })
                // Esc=取消（关弹窗不改名）
                .onCancel([this] { dirRenameOpen_ = false; })
                .build();
            // 校验/落盘失败内嵌红字，弹窗保持打开让用户改输入
            ui_kit::drawDialogText(ui, "md.dialog.renamedir.error", w, dirRenameError_,
                                   12.0f, t.colors.errorText,
                                   {.y = 134.0f, .height = 18.0f, .lineHeight = 18.0f,
                                    .skipIfEmpty = true});
            ui_kit::DialogButtonSpec cancel;
            cancel.label = "取消";
            cancel.fontSize = t.sizes.fontSizeBody;
            cancel.style = ui_kit::DialogButtonStyle::SecondaryThemed;
            cancel.onClick = [this] { dirRenameOpen_ = false; };
            ui_kit::DialogButtonSpec confirm;
            confirm.label = "重命名";
            confirm.fontSize = t.sizes.fontSizeBody;
            confirm.style = ui_kit::DialogButtonStyle::PrimaryThemed;
            confirm.onClick = [this] { commitRenameFolder(); };
            ui_kit::drawDialogButtons(ui, "md.dialog.renamedir", w, h, cancel, confirm, t,
                                      core::theme::componentTokensWeChat(t));
    });
}

void MarkdownModule::doDeleteFolder() {
    const std::string path = dirDeleteTargetPath_;
    dirDeleteConfirmOpen_ = false;
    dirDeleteTargetPath_.clear();
    if (!service_ || path.empty()) return;
    auto r = service_->deleteFolder(path);
    if (r.isFail()) {
        LOG_ERROR("删除文件夹失败 " + path + ": " + r.errorMessage());
        showToast("删除失败", "「" + path + "」" + r.errorMessage(), true);
        return;
    }
    // 选中目录/新建落点若在被删子树内 → 复位到根目录（落点保持有效）。
    // selectedDir_ 的状态语义不变，只做失效兜底。
    if (selectedDir_ == path || selectedDir_.rfind(path + "/", 0) == 0) {
        selectDir("");
    }
    refreshList();
    showToast("已删除文件夹", path, false);
    LOG_INFO("已删除文件夹: " + path);
}

void MarkdownModule::commitRenameFolder() {
    std::string target = dirRenameInput_;
    while (!target.empty() && (target.front() == ' ' || target.front() == '\t')) {
        target.erase(target.begin());
    }
    while (!target.empty() &&
           (target.back() == ' ' || target.back() == '\t' ||
            target.back() == '\r' || target.back() == '\n')) {
        target.pop_back();
    }
    if (target.empty()) {
        dirRenameError_ = "新名字不能为空";
        return;
    }
    if (!service_) {
        dirRenameOpen_ = false;
        return;
    }
    if (dirRenameTargetPath_.empty()) {
        // 根目录不能重命名（服务层同文案兜底）：内嵌红字提示，不静默关弹窗——
        // 用户动作必须有反馈（与删除路径的 toast「根目录不能删除」口径一致）
        dirRenameError_ = "根目录不能重命名";
        return;
    }
    if (target.find('/') != std::string::npos) {
        // 同级改名的界面侧校验（service 层同规则兜底）
        dirRenameError_ = "文件夹只支持同级改名，新名不能包含 /";
        return;  // 弹窗保持打开让用户改
    }
    if (!MarkdownService::isSafeNewNotePath(target)) {
        dirRenameError_ =
            "名称不合法：不支持绝对路径、盘符（如 C:）或 ..";
        return;
    }
    const std::string oldPath = dirRenameTargetPath_;
    auto r = service_->renameFolder(oldPath, target);
    if (r.isFail()) {
        LOG_ERROR("重命名文件夹失败 " + oldPath + " → " + target + ": " +
                  r.errorMessage());
        dirRenameError_ = "重命名失败：" + r.errorMessage();
        return;  // 弹窗保持打开让用户改
    }
    const std::string newPath = r.value();
    dirRenameOpen_ = false;
    dirRenameTargetPath_.clear();
    dirRenameInput_.clear();
    dirRenameError_.clear();
    // 笔记路径随目录名变化 → 索引以磁盘为准对齐（旧路径行被 rescan 剪掉）
    auto rs = service_->rescan();
    if (rs.isFail()) {
        LOG_ERROR("文件夹重命名后 rescan 失败: " + rs.errorMessage());
    }
    // 选中目录/新建落点若在被改名子树内 → 跟随新路径（状态语义不变）
    if (selectedDir_ == oldPath || selectedDir_.rfind(oldPath + "/", 0) == 0) {
        selectDir(newPath + selectedDir_.substr(oldPath.size()));
    }
    refreshList();
    showToast("已重命名", newPath, false);
    LOG_INFO("重命名文件夹: " + oldPath + " → " + newPath);
    core::event::bus().emit(core::event::kMdRenamed);
}

} // namespace module::markdown
