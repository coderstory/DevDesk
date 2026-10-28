// Markdown 模块·文件树块（MarkdownModule 成员函数定义）：目录树列 / 笔记标签条 /
// 边界把手 / 列表刷新 / 打开笔记 / 视图状态持久化 / 新建笔记与新建目录。
// 拆分第一步（抽编译单元）产物：类定义在 markdown_module_internal.h，
// 本文件只承载成员函数体，纯代码搬移，逻辑与签名零变化。

#include "markdown_module_internal.h"

#include <utility>  // std::move / std::pair（check-std-includes：不靠传递包含）

namespace module::markdown {

void MarkdownModule::drawTreeHandle(eui::Ui& ui,
                    float pad,
                    float bodyH,
                    float sideW,
                    float gap,
                    const core::theme::Tokens& t) {
    // 把手在两端之间 lerp：sideW=224 时右贴树右缘（pad+224-handleW），
    // sideW=0 时左贴编辑列左缘（pad+gap）；动画途中平滑过渡，无跳变。
    const float lerpT = sideW / kTreePanelWidth;
    const float handleX =
        (pad + gap) +
        (kTreePanelWidth - kTreeHandleW - gap) * lerpT;
    const float handleY = pad + ui_kit::kHeaderH + t.sizes.spacingLarge +
                          bodyH * 0.5f - kTreeHandleH * 0.5f;
    // 箭头方向在中点两侧各持一态，但点击只调 toggleTreePanel → 目标态反向；
    // 不依赖箭头方向判当前态。折半为分界：>120px 显示左箭头（收起语义），
    // <=120px 显示右箭头（展开语义），动画全程方向逐步翻转无突然跳动。
    const bool expanded = sideW > kTreePanelWidth * 0.5f;
    ui.stack("md.tree.handle")
        .position(handleX, handleY)
        .size(kTreeHandleW, kTreeHandleH)
        .zIndex(6)
        .content([&] {
            // UAT6 MDA：把手底——品牌绿底（wechat.primary #36D399），hover 加深
            const float hoverBrighter = 0.08f;
            ui.rect("md.tree.handle.bg")
                .size(kTreeHandleW, kTreeHandleH)
                .color(t.colors.primary)
                .hoverColor(eui::Color{
                    t.colors.primary.r + hoverBrighter,
                    t.colors.primary.g + hoverBrighter,
                    t.colors.primary.b + hoverBrighter,
                    1.0f})
                .radius(t.sizes.radiusSmall)  // 四角同圆（方案 A 门把手）
                .onClick([this] { toggleTreePanel(); })
                .build();
            // 箭头：展开态 = 左箭头（收起）F053，折叠态 = 右箭头（展开）F054——镜像对称
            ui.text("md.tree.handle.arrow")
                .size(kTreeHandleW, kTreeHandleH)
                .icon(expanded ? 0xF053u : 0xF054u)
                .fontSize(core::theme::scaled(kTreeHandleArrowSize))
                .lineHeight(kTreeHandleH)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.onPrimary)
                .build();
        })
        .build();
}

void MarkdownModule::drawNoteTabBar(eui::Ui& ui,
                    float contentW,
                    const core::theme::Tokens& t,
                    const components::theme::ThemeColorTokens& componentTheme) {
    if (openNotes_.empty()) return;  // 无标签时不创建元素，不占列 gap
    constexpr float kTabBarH = 32.0f;       // S2.2 块面 Tab（弹窗级）
    constexpr float kTabMinW = 80.0f;
    constexpr float kTabMaxW = 200.0f;
    ui.row("md.note.tabs")
        .size(contentW, kTabBarH)
        .gap(0.0f)
        .content([&] {
            for (size_t i = 0; i < openNotes_.size(); ++i) {
                const auto& path = openNotes_[i];
                bool active = (path == currentPath_);
                // 从路径取无扩展名的短名作 tab 标签
                fs::path p(path);
                std::string name = p.filename().replace_extension("").string();
                if (name.empty()) name = path;
                // 估宽后钳位
                const float nameW = estimateTextWidth(name, 12.0f);
                const float tabW = std::min(kTabMaxW, std::max(kTabMinW, nameW + 42.0f));
                ui.stack("md.note.tab." + std::to_string(i))
                    .size(tabW, kTabBarH)
                    .content([&] {
                        // tab 背景（S2.2 块面 Tab：选中 primarySoftBg + H10 描边，未选中透明）
                        ui.rect("md.note.tab." + std::to_string(i) + ".bg")
                            .size(tabW, kTabBarH)
                            .color(active ? t.colors.primarySoftBg
                                          : eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                            .hoverColor(t.colors.hover)
                            .radius(8.0f)
                            .border(active ? 1.0f : 0.0f, t.colors.borderDefault)
                            .onClick([this, path] {
                                if (path != currentPath_) openNote(path);
                            })
                            .build();
                        // tab 名称
                        ui.text("md.note.tab." + std::to_string(i) + ".name")
                            .position(8.0f, 0.0f)
                            .size(tabW - 32.0f, kTabBarH)
                            .text(name)
                            .fontSize(core::theme::scaled(13.0f))
                            .lineHeight(kTabBarH)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .color(active ? t.colors.primary
                                          : t.colors.textSecondary)
                            .fontWeight(active ? 600 : 400)
                            .build();
                        // 关闭钮（×），hover 浅灰底
                        ui.stack("md.note.tab." + std::to_string(i) + ".close")
                            .position(tabW - 28.0f, (kTabBarH - 24.0f) * 0.5f)
                            .size(24.0f, 24.0f)
                            .content([&] {
                                ui.rect("md.note.tab." + std::to_string(i) + ".close.bg")
                                    .size(24.0f, 24.0f)
                                    .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                                    .hoverColor(t.colors.hover)
                                    .radius(12.0f)
                                    .onClick([this, path] { closeTab(path); })
                                    .build();
                                ui.text("md.note.tab." + std::to_string(i) + ".close.icon")
                                    .size(24.0f, 24.0f)
                                    .text("\xC3\x97")  // ×
                                    .fontSize(core::theme::scaled(14.0f))
                                    .lineHeight(core::theme::scaled(24.0f))
                                    .horizontalAlign(eui::HorizontalAlign::Center)
                                    .verticalAlign(eui::VerticalAlign::Center)
                                    .color(t.colors.textTertiary)
                                    .build();
                            })
                            .build();
                    })
                    .build();
            }
        })
        .build();
}

void MarkdownModule::closeTab(const std::string& path) {
    auto it = std::find(openNotes_.begin(), openNotes_.end(), path);
    if (it == openNotes_.end()) return;
    openNotes_.erase(it);
    if (path == currentPath_) {
        if (!openNotes_.empty()) {
            openNote(openNotes_.back());
        } else {
            currentPath_.clear();
            editorText_.clear();
            editorDirty_ = false;
            lastSavedText_.clear();
            outlineDirty_ = true;
            currentTags_.clear();
            tagInput_.clear();
        }
    }
}

void MarkdownModule::drawTreeColumn(eui::Ui& ui,
                    float bodyH,
                    float sideW,
                    const core::theme::Tokens& t,
                    const components::theme::ThemeColorTokens& componentTheme) {
    // 单 stack 列 = bg rect + 绝对定位内容；bg 与 stack 两侧尺寸必须精确重合，
    // 测试 md_tree_column_layout_semantics 锁定（错位会静默溢出）。
    // D-R3-3 b：完全收起时本列仍以 0 宽占位（保持 md.body 行间隙公式与右缘
    // 对齐不变），但不画 bg/内容——展开入口移交边界把手 drawTreeHandle。
    ui.stack("md.tree").size(sideW, bodyH).content([&] {
        if (sideW <= kTreePanelSnapEps) return;
        // 批M（2026-10-25 用户拍板：树/编辑同底辨识度低）：树区改浅灰面板
        // （hover 令牌 #F3F4F6，暗色 #353231），编辑/预览为白纸卡——灰底托纸卡分层。
        // 右侧 1px 分隔线保留。
        ui.rect("md.tree.bg")
            .size(sideW, bodyH)
            .color(t.colors.hover)
            .build();
        ui.rect("md.tree.divider")
            .position(sideW - 1.0f, 0.0f)
            .size(1.0f, bodyH)
            .color(t.colors.hover)
            .build();
        // 收起中 / 展开动画未到门槛：只画面板壳（bg/divider），内容不画——
        // 窄板上搜索框/按钮的固定几何会破版；内容左锚定，展开末段自然滑入。
        if (treePanelCollapsed_ || sideW < kTreePanelContentMinW) return;
        // 方案 A：面板头只有标题——折叠钮已由边界把手接管（drawTreeHandle），
        // 头行 y 8..40；搜索框行 y 48，标签/列表基准同旧。
        ui.text("md.tree.head.title")
            .position(16.0f, 8.0f)
            .size(sideW - 32.0f, 32.0f)
            .text("目录")
            .fontSize(core::theme::scaled(kTreeFontSize))
            .lineHeight(32.0f)
            .fontWeight(700)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textPrimary)
            .build();
        // 搜索框 + 右侧两个图标按钮（+笔记/+文件夹，见下方）。
        const float searchW = sideW - 96.0f;  // 16 左 + 4 gap + 60 btns + 16 右
        components::input(ui, "md.search")
            .position(16.0f, 48.0f)
            .size(searchW, 40.0f)
            .value(searchQuery_)
            .placeholder("搜索笔记…")
            .fontSize(core::theme::scaled(kTreeFontSize))
            .inset(32.0f)
            .theme(componentTheme)
            .onChange([this](const std::string& v) {
                searchQuery_ = v;
                refreshList();
            })
            .build();
        ui.text("md.search.icon")
            .position(28.0f, 59.0f)
            .size(18.0f, 18.0f)
            .icon(0xF002)  // fa-magnifying-glass
            .fontSize(core::theme::scaled(kTreeFontSize))
            .lineHeight(18.0f)
            .color(t.colors.textTertiary)
            .build();
        // 清空按钮（补充项 §3.2.4）：搜索非空时显示「×」，点击清空。
        if (!searchQuery_.empty()) {
            ui.rect("md.search.clear")
                .position(16.0f + searchW - 28.0f, 48.0f)
                .size(28.0f, 40.0f)
                .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                .hoverColor(t.colors.hover)
                .onClick([this] {
                    searchQuery_.clear();
                    refreshList();
                })
                .build();
            ui.text("md.search.clear.icon")
                .position(16.0f + searchW - 28.0f, 48.0f)
                .size(28.0f, 40.0f)
                .text("×")
                .fontSize(core::theme::scaled(14.0f))
                .lineHeight(core::theme::scaled(40.0f))
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textTertiary)
                .build();
        }
        // +笔记 / +文件夹 图标按钮（从面板底部上移到搜索框右侧，§3.3.3）
        // 0xF067 = fa-plus；onClick 接原回调不变。UAT3 D-R3-3：折叠键上移面板头后
        // 两键贴右缘（右 padding 16 与左对称），行内不再有第三键。
        const float btnX0 = sideW - 76.0f;
        const float btnX1 = sideW - 44.0f;
        components::button(ui, "md.tree.new")
            .position(btnX0, 48.0f)
            .size(28.0f, 40.0f)
            .text("+")
            .fontSize(t.sizes.fontSizeBody)
            .theme(componentTheme, false)
            .textColor(t.colors.textSecondary)
            .radius(t.sizes.radius)
            .onClick([this] { openNewNoteDialog(); })
            .build();
        components::button(ui, "md.tree.newfolder")
            .position(btnX1, 48.0f)
            .size(28.0f, 40.0f)
            .text("📁")
            .fontSize(core::theme::scaled(12.0f))
            .theme(componentTheme, false)
            .textColor(t.colors.textSecondary)
            .radius(t.sizes.radius)
            .onClick([this] {
                newFolderOpen_ = true;
                newFolderName_.clear();
                newFolderError_.clear();
                newFolderParentDir_ = newItemParentDir();
                newFolderParentOpen_ = false;
            })
            .build();

        // D-R3-3 a：搜索框行高 40（y48..88），标签区下移保持 8px 呼吸
        const float tagY = 96.0f;
        float tagAreaH = 0.0f;
        if (!allTags_.empty()) {
            ui.text("md.tags.title")
                .position(16.0f, tagY)
                .size(sideW - 32.0f, 14.0f)
                .text("标签")
                .fontSize(core::theme::scaled(10.0f))
                .lineHeight(core::theme::scaled(14.0f))
                .fontWeight(700)
                .color(t.colors.textTertiary)
                .build();
            const float fs = 11.0f;
            const float rowH = 24.0f;
            // UAT2 F批：chip 左/右缘与面板 padding 16 对齐；间距 4→6
            float cx = 16.0f;
            float cy = tagY + 18.0f;
            int row = 0;
            for (size_t i = 0; i < allTags_.size(); ++i) {
                const auto& tc = allTags_[i];
                const std::string label =
                    tc.name + "·" + std::to_string(tc.noteCount);
                const std::string id = "md.tags.filter." + std::to_string(i);
                const float chipW = 16.0f + estimateTextWidth(label, fs);
                if (cx + chipW > sideW - 16.0f) {
                    cx = 16.0f;
                    cy += rowH + 6.0f;
                    if (++row >= 3) break;
                }
                const bool active = tagFilter_ == tc.name;
                ui.stack(id)
                    .position(cx, cy)
                    .size(chipW, rowH)
                    .content([&] {
                        ui.rect(id + ".bg")
                            .size(chipW, rowH)
                            .color(active ? t.colors.primary : t.colors.hover)
                            .hoverColor(t.colors.primarySoftBg)
                            .radius(t.sizes.radiusSmall)
                            .onClick([this, name = tc.name] {
                                toggleTagFilter(name);
                            })
                            .build();
                        ui.text(id + ".name")
                            .position(0.0f, 0.0f)
                            .size(chipW, rowH)
                            .text(label)
                            .fontSize(fs)
                            .lineHeight(rowH)
                            .horizontalAlign(eui::HorizontalAlign::Center)
                            // 文本盒 == 胶囊高，须显式垂直居中（缺它墨迹从行框顶起排）
                            .verticalAlign(eui::VerticalAlign::Center)
                            .color(active ? t.colors.onPrimary
                                          : t.colors.textSecondary)
                            .build();
                    })
                    .build();
                cx += chipW + 6.0f;
            }
            tagAreaH = 18.0f + static_cast<float>(row + 1) * (rowH + 6.0f);
        }

        // 搜索时树展平为命中文件列表；空搜索 = 二级目录树
        // D-R3-3 a：搜索框行下移后基准 68→100（保持与搜索框底 12px 呼吸）
        const float listY = 100.0f + tagAreaH;
        // 新建按钮已上移到搜索框右侧，底部不再预留按钮区——只留 16px 底 padding。
        const float listH = std::max(0.0f, bodyH - listY - 16.0f);

        if (!listError_.empty()) {
            ui.text("md.tree.error")
                .position(16.0f, listY + 8.0f)
                .size(sideW - 32.0f, 60.0f)
                .text("列表加载失败：\n" + listError_ +
                      "\n每 5 秒自动重试")
                .fontSize(t.sizes.fontSizeSmall)
                .lineHeight(18.0f)
                .wrap()
                .color(t.colors.errorText)
                .build();
        } else if (notes_.empty()) {
            if (searchQuery_.empty() && tagFilter_.empty()) {
                drawTreeList(ui, listY, listH, sideW, t, componentTheme);
                // 有文件夹时只留树：提示行会压住第一个文件夹行，空文件夹本身就是内容
                if (allFolders_.empty()) {
                    ui.text("md.tree.empty")
                        .position(36.0f, listY + 40.0f)
                        .size(sideW - 48.0f, 40.0f)
                        .text("还没有笔记\n点右上角 + 开始")
                        .fontSize(t.sizes.fontSizeSmall)
                        .lineHeight(18.0f)
                        .wrap()
                        .color(t.colors.textTertiary)
                        .build();
                }
            } else {
                ui.text("md.tree.empty")
                    .position(16.0f, listY + 8.0f)
                    .size(sideW - 32.0f, 40.0f)
                    .text("没有匹配的笔记")
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(18.0f)
                    .wrap()
                    .color(t.colors.textTertiary)
                    .build();
            }
        } else if (!searchQuery_.empty()) {
            drawNoteList(ui, listY, listH, sideW, t, componentTheme);
        } else {
            drawTreeList(ui, listY, listH, sideW, t, componentTheme);
        }
        // +笔记/+文件夹 已上移到搜索框右侧（见上方），底部不再有按钮。
    }).build();
}

void MarkdownModule::drawNoteList(eui::Ui& ui,
                  float listY,
                  float listH,
                  float sideW,
                  const core::theme::Tokens& t,
                  const components::theme::ThemeColorTokens& componentTheme) {
    const float rowW = sideW - 16.0f;
    // contentKey 用逐条路径组合 hash：同数量替换内容也触发重建
    size_t listKey = 0;
    for (const auto& n : notes_) {
        listKey = listKey * 1315423911u +
                  std::hash<std::string>{}(noteDisplayPath(n));
    }
    components::scrollView(ui, "md.tree.scroll")
        .position(8.0f, listY)
        .size(sideW - 16.0f, listH)
        .gap(4.0f)  // UAT2 F批：行间留白 0→4
        .step(40.0f)
        .theme(componentTheme)
        .contentKey("md.tree.scroll." + std::to_string(listKey))
        .content([&](eui::Ui& inner, float contentWidth, float viewportHeight) {
            (void)viewportHeight;
            const float w = std::min(contentWidth, rowW);
            for (size_t i = 0; i < notes_.size(); ++i) {
                const NoteMeta& n = notes_[i];
                const std::string path = noteDisplayPath(n);
                const std::string id = "md.tree.item." + std::to_string(i);
                drawFileTreeItem(inner, id, w, t, 0xF15C, noteDisplayName(n),
                                 path == currentPath_, 0.0f,
                                 [this, path] { openNote(path); },
                                 [this, path](const eui::PointerEvent& ev,
                                              const eui::Rect&) {
                                     openFileContextMenu(path, ev);
                                 }, searchQuery_);
            }
        })
        .build();
}

void MarkdownModule::drawTreeList(eui::Ui& ui,
                  float listY,
                  float listH,
                  float sideW,
                  const core::theme::Tokens& t,
                  const components::theme::ThemeColorTokens& componentTheme) {
    const std::vector<MdTreeRow> rows = buildTreeRows(notes_, allFolders_,
                                                      collapsedDirs_);
    const float rowW = sideW - 16.0f;
    // contentKey：行内容（类型 + 名字 + 路径）组合 hash，内容变即重建
    size_t treeKey = 0;
    for (const auto& r : rows) {
        treeKey = treeKey * 1315423911u +
                  std::hash<std::string>{}(r.name + "\x1f" + r.relPath) +
                  (r.isFolder ? 1u : 0u) + static_cast<size_t>(r.depth);
    }
    components::scrollView(ui, "md.tree.scroll")
        .position(8.0f, listY)
        .size(sideW - 16.0f, listH)
        .gap(4.0f)  // UAT2 F批：行间留白 0→4
        .step(40.0f)
        .theme(componentTheme)
        .contentKey("md.tree.scroll." + std::to_string(treeKey))
        .content([&](eui::Ui& inner, float contentWidth, float viewportHeight) {
            (void)viewportHeight;
            const float w = std::min(contentWidth, rowW);
            for (size_t i = 0; i < rows.size(); ++i) {
                const MdTreeRow& r = rows[i];
                const std::string id = "md.tree.node." + std::to_string(i);
                if (r.isFolder) {
                    // 目录行：行体点击 = 选中目录，箭头命中区 = 收起/展开
                    //（命中取最上层兄弟，故命中区须叠在行体之上）。缩进每级 16px。
                    // H29：文件夹行不画选中视觉（无底色、无左侧绿竖线）——selectedDir_
                    // 仍是「新建笔记落点」状态锚与跨会话持久化状态，去的是视觉不是状态。
                    const float indent = static_cast<float>(r.depth) * 16.0f;
                    const bool expanded = collapsedDirs_.count(r.relPath) == 0;
                    inner.stack(id)
                        .size(w, kTreeRowHeight)
                        .content([&] {
                            inner.rect(id + ".row")
                                .size(w, kTreeRowHeight)
                                .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                                .hoverColor(ui_kit::cardSurface(t))
                                .radius(t.sizes.radiusSmall)
                                .onClick([this, dir = r.relPath] { selectDir(dir); })
                                .onContextMenu([this, dir = r.relPath](
                                                   const eui::PointerEvent& ev,
                                                   const eui::Rect&) {
                                    openDirContextMenu(dir, ev);
                                })
                                .build();
                            // 展开/收起命中区（叠在行体之上，命中取最上层）。
                            // UAT2 F批：26→46（箭头+文件夹图标整区可点）+ hover
                            // 反馈（原全透明无提示；三态同款 app.sidebar.toggle）。
                            // 宽度止于名称区起点（46+indent）：命中取最上层兄弟，
                            // 再宽会把行体的 selectDir（点名称选中目录）整个吞掉。
                            inner.rect(id + ".toggle")
                                .position(indent, 0.0f)
                                .size(44.0f, kTreeRowHeight)
                                .states(eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                                        t.colors.hover,
                                        t.colors.primarySoftBg)
                                .radius(t.sizes.radiusSmall)
                                .onClick([this, dir = r.relPath] {
                                    if (collapsedDirs_.count(dir) != 0) {
                                        collapsedDirs_.erase(dir);
                                    } else {
                                        collapsedDirs_.insert(dir);
                                    }
                                    saveViewState();  // 目录折叠跨会话记忆
                                })
                                // 右键菜单也接在命中区上：44px 区覆盖箭头+图标，
                                // 只在行体上接会漏掉整区右键
                                .onContextMenu([this, dir = r.relPath](
                                                   const eui::PointerEvent& ev,
                                                   const eui::Rect&) {
                                    openDirContextMenu(dir, ev);
                                })
                                .build();
                            inner.text(id + ".arrow")
                                .position(0.0f + indent, 0.0f)
                                .size(16.0f, kTreeRowHeight)
                                .text(expanded ? "▾" : "▸")
                                .fontSize(core::theme::scaled(kTreeArrowFontSize))
                                .lineHeight(kTreeRowHeight)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .color(t.colors.textTertiary)
                                .build();
                            inner.text(id + ".ficon")
                                .position(20.0f + indent, 0.0f)
                                .size(16.0f, kTreeRowHeight)
                                .icon(0xF07B)  // fa-folder
                                .fontSize(core::theme::scaled(kTreeFontSize))
                                .lineHeight(kTreeRowHeight)
                                .verticalAlign(eui::VerticalAlign::Center)
                                // H29：文件夹图标不随选中变色（与行底色/竖线同批去视觉）
                                .color(t.colors.textTertiary)
                                .build();
                            inner.text(id + ".name")
                                .position(44.0f + indent, 0.0f)
                                .size(std::max(0.0f, w - 52.0f - indent),
                                      kTreeRowHeight)
                                .text(r.name)
                                .fontSize(core::theme::scaled(kTreeFontSize))
                                .lineHeight(kTreeRowHeight)
                                .verticalAlign(eui::VerticalAlign::Center)
                                // 同文件行：深字（S2.5 primary 字冲突待裁决）
                                .color(t.colors.textPrimary)
                                .fontWeight(700)
                                .build();
                        })
                        .build();
                } else {
                    // 文件行缩进：16px 内边距基准 + (depth-1)*16 层级缩进（S2.5 列表水平内边距）
                    const float indent =
                        0.0f + static_cast<float>(std::max(0, r.depth - 1)) * 16.0f;
                    drawFileTreeItem(inner, id, w, t, 0xF15C, r.name,
                                     r.relPath == currentPath_,
                                     indent,
                                     [this, path = r.relPath] { openNote(path); },
                                     [this, path = r.relPath](
                                         const eui::PointerEvent& ev,
                                         const eui::Rect&) {
                                         openFileContextMenu(path, ev);
                                     });
                }
            }
        })
        .build();
}

void MarkdownModule::openNewNoteDialog() {
    newNoteOpen_ = true;
    newNoteName_.clear();
    newNoteError_.clear();
}

void MarkdownModule::refreshList() {
    if (!service_) {
        notes_.clear();
        allFolders_.clear();
        allTags_.clear();
        return;
    }
    // 磁盘全层目录实时枚举（含空目录），树据此显示空文件夹行
    if (auto fr = service_->listAllFolders(); fr.isOk()) {
        allFolders_ = std::set<std::string>(fr.value().begin(), fr.value().end());
    }
    auto r = searchQuery_.empty() ? service_->listAll()
                                  : service_->search(searchQuery_);
    if (r.isFail()) {
        LOG_ERROR("刷新笔记列表失败: " + r.errorMessage());
        showToast("笔记列表加载失败", r.errorMessage(), true);
        listError_ = r.errorMessage();
        notes_.clear();  // 不拿旧数据伪装成当前态
        return;
    }
    listError_.clear();
    notes_ = std::move(r.value());
    // 树只显示含当前筛选标签的笔记
    if (!tagFilter_.empty()) {
        auto fr = service_->notesWithTag(tagFilter_);
        if (fr.isOk()) {
            std::set<std::string> keep;
            for (const auto& n : fr.value()) {
                keep.insert(n.relativePath.generic_string());
            }
            notes_.erase(
                std::remove_if(notes_.begin(), notes_.end(),
                               [&](const NoteMeta& n) {
                                   return keep.count(
                                              n.relativePath.generic_string()) == 0;
                               }),
                notes_.end());
        } else {
            LOG_WARN("标签筛选失败: " + fr.errorMessage());
        }
    }
    refreshTags();
}

void MarkdownModule::openNote(const std::string& relPath) {
    saveEditor();
    // saveEditor 失败时 editorDirty_ 保持 true：还有未落盘改动就阻止切换并
    // 提示（保数据优先，改到别处会静默丢弃编辑）。
    if (editorDirty_) {
        showToast("有未保存的改动",
                  "「" + currentPath_ + "」保存失败，切换会丢失这些编辑；"
                  "请先处理保存问题或撤销改动",
                  true);
        return;
    }
    if (!service_) return;

    auto r = service_->read(relPath);
    if (r.isFail()) {
        LOG_ERROR("读取笔记失败 " + relPath + ": " + r.errorMessage());
        showToast("读取失败", "「" + relPath + "」" + r.errorMessage(), true);
        return;
    }
    currentPath_ = relPath;
    setEditorText(r.value());
    lastSavedText_ = editorText_;
    editorDirty_ = false;
    outlineDirty_ = true;
    activeOutlineIdx_ = -1;
    previewHover_.reset();  // 换笔记 = 块身份换代，旧 hover 作废
    // 换笔记 = 实时态状态群换代（见 enterLiveState）
    enterLiveState("换笔记");
    tagInput_.clear();  // 清掉上一篇残留的半截输入
    if (auto tr = service_->tagsForNote(relPath); tr.isOk()) {
        currentTags_ = std::move(tr.value());
    } else {
        currentTags_.clear();
    }
    syncCurrentMtime();  // 外部修改检测的 mtime 基准
    if (mode_ == ViewMode::Preview) syncPreviewOnEnter_ = true;
    // 新建条目的父目录跟随当前打开笔记的所在目录
    // 选中不级联：文件夹选中标记仅精确匹配，不因打开笔记而上溯选中父目录
    newItemTargetDir_ = parentDirOf(relPath);
    // 方案 A：维护已打开笔记标签页列表；重复打开时保持原位不重复追加
    auto it = std::find(openNotes_.begin(), openNotes_.end(), relPath);
    if (it == openNotes_.end()) {
        openNotes_.push_back(relPath);
    }
    saveViewState();  // 选中笔记跨会话记忆
    LOG_INFO("打开笔记: " + relPath);
}

void MarkdownModule::loadViewState() {
    viewStore_ = std::make_unique<MarkdownViewStateStore>(::core::appDatabase());
    const MarkdownViewState st = viewStore_->load();
    if (st.mode != static_cast<int>(mode_)) {
        // 直接赋值而非 setMode：启动期无需再落一次视图状态
        mode_ = static_cast<ViewMode>(st.mode);
        if (mode_ == ViewMode::Preview) syncPreviewOnEnter_ = true;
        if (mode_ == ViewMode::Live) enterLiveState("启动恢复视图");
    }
    collapsedDirs_.insert(st.collapsedDirs.begin(), st.collapsedDirs.end());
    treePanelCollapsed_ = st.treePanelCollapsed;  // UAT2 F批：面板折叠态恢复
    // D-R3-3 c：启动即按折叠态把动画宽直接钉到目标（无起播动画，首帧即收敛）
    treePanelWidth_ = treePanelCollapsed_ ? 0.0f : kTreePanelWidth;
    // 目录已被外部删除时不在此校验，由 newItemParentDir 兜底
    selectedDir_ = st.selectedDir;
    newItemTargetDir_ = selectedDir_;  // 启动时与选中态同步，后续由 openNote / selectDir 分别更新
    if (st.lastPath.empty()) return;
    const bool exists =
        std::any_of(notes_.begin(), notes_.end(),
                    [&](const NoteMeta& n) {
                        return n.relativePath.generic_string() == st.lastPath;
                    });
    if (!exists) {
        LOG_INFO("Markdown: 上次笔记已不存在，跳过恢复: " + st.lastPath);
        return;
    }
    openNote(st.lastPath);
    LOG_INFO("Markdown: 已恢复上次笔记: " + st.lastPath);
}

void MarkdownModule::saveViewState() const {
    if (!viewStore_) return;
    MarkdownViewState st;
    st.lastPath = currentPath_;
    st.mode = static_cast<int>(mode_);
    st.collapsedDirs.assign(collapsedDirs_.begin(), collapsedDirs_.end());
    st.selectedDir = selectedDir_;
    st.treePanelCollapsed = treePanelCollapsed_;
    auto r = viewStore_->save(st);
    if (r.isFail()) {
        LOG_WARN("Markdown: 视图状态落盘失败（不影响编辑）: " + r.errorMessage());
    }
}

void MarkdownModule::commitNewNote(eui::Ui& ui) {
    std::string name = newNoteName_;
    while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
        name.erase(name.begin());
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
        name.pop_back();
    }
    // 空名提交 = 建「无标题笔记」撞名自动编号，不报错；显式命名撞名则直接打开。
    const bool defaultedName = name.empty();
    if (defaultedName) name = "无标题笔记";
    if (!service_) return;

    // 父目录 = 当前选中目录（失效时回根目录兜底）；名字可再带子路径，两者叠加。
    const std::string parent = newItemParentDir();

    // 路径安全校验（service 层有同规则兜底，这里给友好提示）
    std::string relPath = parent.empty() ? name : parent + "/" + name;
    if (!MarkdownService::isSafeNewNotePath(relPath)) {
        // 校验错误内嵌红字
        newNoteError_ =
            "笔记名不合法：不支持绝对路径、盘符（如 C:）或 ..，请用相对路径";
        return;
    }

    // 无扩展名时补 .md（与 MarkdownService::create 一致）
    relPath = withMdExtension(relPath);

    // 显式命名已存在 → 直接打开；默认名走「永远新建」编号
    if (!defaultedName && service_->read(relPath).isOk()) {
        newNoteOpen_ = false;
        refreshList();
        openNote(relPath);
        showToast("笔记已存在", "已直接打开 " + relPath, false);
        if (currentPath_ == relPath) ui.requestFocus(kEditorInputHitId);
        return;
    }
    if (defaultedName) {
        // 默认名撞名 → 「无标题笔记 N.md」自动编号；只换文件名、保留父目录
        for (int i = 2; service_->fileMtime(relPath).isOk(); ++i) {
            if (i > 999) {
                newNoteError_ = "新建失败：同名无标题笔记过多，请手动命名";
                return;
            }
            const std::string numbered =
                "无标题笔记 " + std::to_string(i) + ".md";
            relPath = parent.empty() ? numbered : parent + "/" + numbered;
        }
    }

    auto r = service_->create(relPath);
    if (r.isFail()) {
        LOG_ERROR("新建笔记失败 " + relPath + ": " + r.errorMessage());
        newNoteError_ = "新建失败：" + r.errorMessage();
        return;
    }
    newNoteOpen_ = false;
    refreshList();
    openNote(relPath);
    showToast("已创建笔记", relPath, false);
    // 新建后自动 focus 编辑器（requestFocus 帧末消费）；
    // openNote 守卫失败（未保存改动阻塞切换）时不聚焦。
    if (currentPath_ == relPath) ui.requestFocus(kEditorInputHitId);
    LOG_INFO("新建笔记: " + relPath);
    core::event::bus().emit(core::event::kMdCreated);
}

std::string MarkdownModule::parentDirOf(const std::string& relPath) {
    const size_t slash = relPath.rfind('/');
    return slash == std::string::npos ? std::string() : relPath.substr(0, slash);
}

bool MarkdownModule::dirExistsOnDisk(const std::string& dir) const {
    return dir.empty() || allFolders_.count(dir) != 0;
}

std::string MarkdownModule::newItemParentDir() const {
    if (newItemTargetDir_.empty()) return std::string();          // 根目录
    if (dirExistsOnDisk(newItemTargetDir_)) return newItemTargetDir_;
    LOG_WARN("Markdown: 新建目标目录已不存在（外部删除？），回退到根目录: " +
             newItemTargetDir_);
    return std::string();                                         // 兜底
}

void MarkdownModule::selectDir(const std::string& dir) {
    if (selectedDir_ == dir) return;
    selectedDir_ = dir;
    newItemTargetDir_ = dir;
    saveViewState();
}

void MarkdownModule::toggleTreePanel() {
    treePanelCollapsed_ = !treePanelCollapsed_;
    saveViewState();
}

std::string MarkdownModule::withMdExtension(std::string name) {
    if (name.size() >= 3 && name.compare(name.size() - 3, 3, ".md") == 0) {
        return name;
    }
    return name + ".md";
}

std::vector<std::pair<std::string, std::string>> MarkdownModule::folderParentCandidates() const {
    std::vector<std::pair<std::string, std::string>> out;
    out.emplace_back("根目录", std::string());
    for (const auto& d : allFolders_) {
        if (d.empty()) continue;
        out.emplace_back(d, d);
    }
    return out;
}

int MarkdownModule::folderParentIndexFor(
    const std::vector<std::pair<std::string, std::string>>& candidates,
    const std::string& dir) {
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i].second == dir) return static_cast<int>(i);
    }
    return 0;
}

void MarkdownModule::commitNewFolder() {
    std::string name = newFolderName_;
    // 去掉首尾空白与多余的尾部 '/'
    while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
        name.erase(name.begin());
    }
    while (!name.empty() &&
           (name.back() == ' ' || name.back() == '\t' || name.back() == '/')) {
        name.pop_back();
    }
    if (name.empty()) {
        // 校验错误内嵌红字
        newFolderError_ = "文件夹名不能为空";
        return;
    }
    if (!service_) return;

    // 上级目录 = 存下的路径（非下标）+ 名字；路径不在清单 → 落回根目录并出声
    const auto candidates = folderParentCandidates();
    const std::string parent =
        candidates[static_cast<size_t>(folderParentIndexFor(candidates,
                                                           newFolderParentDir_))]
            .second;
    if (parent != newFolderParentDir_) {
        LOG_WARN("Markdown: 上级目录已不存在（外部删除？），改在根目录创建: " +
                 newFolderParentDir_);
    }
    const std::string relPath = parent.empty() ? name : parent + "/" + name;

    auto r = service_->createFolder(relPath);
    if (r.isFail()) {
        LOG_ERROR("新建文件夹失败 " + relPath + ": " + r.errorMessage());
        newFolderError_ = "新建失败：" + r.errorMessage();
        return;
    }
    newFolderOpen_ = false;
    newFolderParentOpen_ = false;
    refreshList();  // listAllFolders 磁盘枚举 → 树里立即出现空文件夹行
    // 新建的目录 = 新的选中目录（接着「+ 笔记」即落在此夹）
    selectDir(relPath);
    showToast("已创建文件夹", relPath, false);
    LOG_INFO("新建文件夹: " + relPath);
}

} // namespace module::markdown
