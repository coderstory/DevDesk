// Markdown 模块·预览块（MarkdownModule 成员函数定义）：预览列 / 块流 / 实时态
// 单输入编辑 / 大纲面板 / 同步滚动 / 块级 undo-redo。
// 拆分第一步（抽编译单元）产物：类定义在 markdown_module_internal.h，
// 本文件只承载成员函数体，纯代码搬移，逻辑与签名零变化。

#include "markdown_module_internal.h"

#include <utility>  // std::move（check-std-includes：不靠传递包含）

namespace module::markdown {

void MarkdownModule::drawOutlinePanel(eui::Ui& ui,
                      const eui::Screen& screen,
                      const core::theme::Tokens& t,
                      const components::theme::ThemeColorTokens& componentTheme) {
    if (!outlineOpen_) return;
    const float pad = t.sizes.spacingLarge * 2.0f;  // 32 —— S4.2 弹性内边距
    const float panelW = 264.0f;
    // 2026-10-25 批G 同拍板：大纲浮面板不再底贴底，补 32 下距与主体对齐
    const float panelH = screen.height - pad - ui_kit::kHeaderH -
                         t.sizes.spacingLarge - pad;
    const float x = screen.width - pad - panelW;
    const float y = pad + ui_kit::kHeaderH + t.sizes.spacingLarge;

    ui.stack("md.outline.panel")
        .position(x, y)
        .size(panelW, panelH)
        .zIndex(10)
        .content([&] {
            // UAT3 T1：大纲浮面板原为暖橙 surfaceElevated 米色底（米黄根源之一），
            // 随页面主体整体 wechat 化——白卡底 + 一级分割线边框 + wechat 文字档。
            ui.rect("md.outline.panel.bg")
                .size(panelW, panelH)
                .color(ui_kit::cardSurface(t))
                .radius(t.sizes.radiusLarge)
                .border(1.0f, t.colors.borderDefault)
                .build();
            ui.text("md.outline.panel.title")
                .position(16.0f, 10.0f)
                .size(120.0f, 24.0f)
                .text("大纲")
                .fontSize(t.sizes.fontSizeBody)
                .lineHeight(24.0f)
                .fontWeight(700)
                .color(t.colors.textPrimary)
                .build();
            components::button(ui, "md.outline.panel.close")
                .position(panelW - 44.0f, 8.0f)
                .size(32.0f, 28.0f)
                .text("✕")
                .fontSize(t.sizes.fontSizeSmall)
                .theme(componentTheme, false)
                .textColor(t.colors.textSecondary)
                .radius(t.sizes.radius)
                .onClick([this] { outlineOpen_ = false; })
                .build();

            if (outline_.empty()) {
                ui.text("md.outline.panel.empty")
                    .position(16.0f, 52.0f)
                    .size(panelW - 32.0f, 60.0f)
                    .text(currentPath_.empty() ? "先打开一篇笔记"
                                               : "没有标题\n用 # 开始一行即可加入大纲")
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(18.0f)
                    .wrap()
                    .color(t.colors.textTertiary)
                    .build();
                return;
            }

            // 当前节：光标行所在的上一个标题
            const int active = activeOutlineIndex(ui);

            size_t outlineKey = 0;
            for (const auto& h : outline_) {
                outlineKey = outlineKey * 1315423911u +
                             std::hash<std::string>{}(h.title) +
                             static_cast<size_t>(h.level);
            }
            components::scrollView(ui, "md.outline.scroll")
                .position(8.0f, 44.0f)
                .size(panelW - 16.0f, std::max(0.0f, panelH - 52.0f))
                .gap(0.0f)
                .step(40.0f)
                .theme(componentTheme)
                .contentKey("md.outline.scroll." + std::to_string(outlineKey))
                .content([&](eui::Ui& inner, float contentWidth,
                             float viewportHeight) {
                    (void)viewportHeight;
                    const float rowW = std::min(contentWidth, panelW - 16.0f);
                    for (size_t i = 0; i < outline_.size(); ++i) {
                        const MdHeading& h = outline_[i];
                        const std::string id =
                            "md.outline.item." + std::to_string(i);
                        const bool current = static_cast<int>(i) == active;
                        inner.stack(id)
                            .size(rowW, 30.0f)
                            .content([&] {
                                // UAT3 T1：行底/悬停/文字全部 wechat 化
                                //（选中 = 浅灰 selectedBg + 绿字，未选 = 白卡底 + 一级文字）
                                inner.rect(id + ".row")
                                    .size(rowW, 30.0f)
                                    .color(current ? t.colors.primarySoftBg
                                                   : ui_kit::cardSurface(t))
                                    .hoverColor(t.colors.hover)
                                    .radius(t.sizes.radiusSmall)
                                    .onClick([this, i, &ui, componentTheme] {
                                        jumpToHeading(ui, i, componentTheme);
                                    })
                                    .build();
                                inner.text(id + ".label")
                                    .position(12.0f + static_cast<float>(h.level - 1) * 14.0f,
                                              0.0f)
                                    .size(std::max(0.0f, rowW - 20.0f -
                                                   static_cast<float>(h.level - 1) * 14.0f),
                                          30.0f)
                                    .text(h.title.empty() ? "(无标题)" : h.title)
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .lineHeight(30.0f)
                                    .verticalAlign(eui::VerticalAlign::Center)
                                    .color(current ? t.colors.primary
                                                   : t.colors.textPrimary)
                                    .fontWeight(current ? 700 : 400)
                                    .build();
                            })
                            .build();
                    }
                })
                .build();
        })
        .build();
}

void MarkdownModule::drawPreviewColumn(eui::Ui& ui,
                       float bodyH,
                       float midW,
                       const core::theme::Tokens& t,
                       const components::theme::ThemeColorTokens& componentTheme,
                       float gap) {
    (void)gap;
    ui.stack("md.preview").size(midW, bodyH).content([&] {
        ui.rect("md.preview.bg")
            .size(midW, bodyH)
            .color(t.colors.background)
            .build();
        // 白纸卡（批M，与编辑器列同款）；空态不铺卡
        if (!currentPath_.empty()) {
            ui.rect("md.preview.paper")
                .position(8.0f, 8.0f)
                .size(midW - 16.0f, std::max(0.0f, bodyH - 16.0f))
                .color(t.colors.surface)
                .radius(t.sizes.radiusLarge)
                .border(1.0f, t.colors.borderDefault)
                .build();
        }

        float pvX = 20.0f;
        float pvW = midW - 40.0f;
        if (mode_ == ViewMode::Preview) {
            // 阅读态：内容列 clamp 760px 居中
            pvW = std::min(pvW, kPreviewColumnWidth);
            pvX = std::max(0.0f, (midW - pvW) * 0.5f);
        }
        const float pvY = 16.0f;
        const float pvH = std::max(0.0f, bodyH - pvY - 16.0f);

        if (currentPath_.empty()) {
            ui.text("md.preview.hint")
                .position(pvX, pvY)
                .size(pvW, 40.0f)
                .text("选择笔记后在这里看到渲染结果")
                .fontSize(t.sizes.fontSizeBody)
                .lineHeight(20.0f)
                .color(t.colors.textTertiary)
                .build();
            return;
        }

        const components::MarkdownStyle style =
            scaledPreviewStyle(componentTheme);

        // 滚动位置来源：Split = 同步滚动目标（编辑器驱动）；Preview = previewOffset_
        float scrollOffset = previewOffset_;
        if (mode_ == ViewMode::Split) {
            scrollOffset = computeSyncOffset(ui, pvW, pvH, style);
        } else if (syncPreviewOnEnter_) {
            previewOffset_ = computeSyncOffset(ui, pvW, pvH, style);
            scrollOffset = previewOffset_;
            syncPreviewOnEnter_ = false;
        }

        auto scroll = components::scrollView(ui, "md.preview.scroll")
            .position(pvX, pvY)
            .size(pvW, pvH)
            .gap(t.sizes.spacing)
            .step(56.0f)
            .theme(componentTheme)
            .offset(scrollOffset)
            .contentKey("md.preview.scroll." + currentPath_ + "." +
                        std::to_string(previewTextHash_));
        if (mode_ == ViewMode::Preview) {
            // 仅预览态允许手动滚动（滚轮写回 previewOffset_）
            scroll.onChange([this](float v) { previewOffset_ = v; });
        }
        scroll.content([&](eui::Ui& inner, float contentWidth, float viewportHeight) {
                (void)viewportHeight;
                lastPvContentWidth_ = contentWidth;  // 供同步滚动 estimateHeight 用
                // 逐块渲染与实时态共用 drawBlockFlow；点击定位要写模块 Ui，
                // 故闭包捕获外层 ui。预览/双栏态注入点传空实现。
                drawBlockFlow(inner, contentWidth, t, style, "md.preview",
                              [this, &ui](size_t k, float y) {
                                  handlePreviewBlockClick(ui, k, y);
                              },
                              {});
            })
            .build();
        // 直写实例 offset（绕开 .offset() 的 init-only 守卫）：
        // Split 每帧推送 computeSyncOffset，Preview 推送 previewOffset_（滚轮已同值回写）。
        if (mode_ == ViewMode::Split || mode_ == ViewMode::Preview) {
            ui.setScrollOffset("md.preview.scroll", scrollOffset);
        }
    }).build();
}

void MarkdownModule::drawBlockFlow(eui::Ui& ui,
                   float contentWidth,
                   const core::theme::Tokens& t,
                   const components::MarkdownStyle& style,
                   const std::string& prefix,
                   const std::function<void(size_t, float)>& onBlockTap,
                   const BlockFlowHooks& hooks) {
    if (blockModel_.text() != editorText_) {
        blockModel_.update(editorText_);  // 防御：理论不可达
    }
    // 色表注入（每帧）：主题切换时缓存旧色条目自动作废
    previewCodeCache_.setColors(mdHighlightColors(t));
    // 逐块 hit/hover 几何表（与同步滚动同一张）。键全等时零成本；Preview/Live 态是唯一重建点。
    rebuildSyncMap(contentWidth, style);
    const auto& blocks = blockModel_.blocks();
    std::vector<int> activeCodeIds;  // 本帧 CodeFence 块，供缓存回收
    // UAT3 T1：块 hover 底衬随页面主体 wechat 化（浅灰，同 surfaceHover 口径）。
    const eui::Color hoverTint = t.colors.hover;
    ui.column(prefix + ".blocks")
        .width(contentWidth)
        .gap(style.blockGap)
        .content([&] {
            for (size_t k = 0; k < blocks.size(); ++k) {
                const MdBlock& b = blocks[k];
                const bool hovered = previewHover_.hovered(b.stableId);
                const float hk = syncMap_.blockHeight(k);
                // 包裹高默认取几何表 H_k，注入点可覆盖：实时态活动块改取 input
                // 贴合高（源码行数与渲染高不等，用 H_k 会切成输入框内部滚动）。
                const float wrapH =
                    hooks.wrapHeight ? hooks.wrapHeight(k, b, hk, contentWidth)
                                     : hk;
                // 逐块交互包裹
                // 上游无逐块命中/指针事件，这里给每块包固定高 stack：hover 底衬最下、
                // 内容居中、透明 MouseArea 覆盖层最上；透明 mouseArea 不劫持滚动。
                const std::string wrapId =
                    prefix + ".blk." + std::to_string(b.stableId);
                ui.stack(wrapId)
                    .width(contentWidth)
                    .height(wrapH)
                    .content([&] {
                        // hover 底衬（CodeFence bg 不透明，走混色参数由块自身呈现）
                        ui.rect(wrapId + ".hl")
                            .size(contentWidth, wrapH)
                            .radius(style.radius)
                            .color(hovered
                                       ? hoverTint
                                       : eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                            .build();
                        // 注入点：视图态可自绘本块内容（返回 true = 已画，跳过下方渲染通路）。
                        bool contentDrawn =
                            hooks.drawContent &&
                            hooks.drawContent(ui, k, b, wrapId, contentWidth,
                                              wrapH);
                        if (!contentDrawn &&
                            b.kind == MdBlockKind::CodeFence) {
                            // CodeFence 自绘：上游代码块单色且不暴露预解析注入。
                            // 未变块命中 (stableId, hash) 缓存零重算。
                            const MdPreviewCodeHighlight* hl =
                                previewCodeCache_.get(b.stableId, b.hash);
                            if (!hl) {
                                MdPreviewCodeHighlight fresh =
                                    mdPreviewCodeHighlight(
                                        blockModel_.text().substr(
                                            b.start, b.end - b.start),
                                        mdHighlightColors(t));
                                if (fresh.ok) {
                                    previewCodeCache_.put(
                                        b.stableId, b.hash, std::move(fresh));
                                    hl = previewCodeCache_.get(b.stableId,
                                                               b.hash);
                                }
                            }
                            if (hl) {
                                activeCodeIds.push_back(b.stableId);
                                drawPreviewCodeBlock(ui, wrapId + ".code", *hl,
                                                     contentWidth, style,
                                                     hovered, hoverTint);
                                contentDrawn = true;
                            }
                        }
                        if (!contentDrawn) {
                            components::markdown(
                                ui,
                                prefix + ".block." +
                                    std::to_string(b.stableId))
                                .markdown(blockModel_.text().substr(
                                    b.start, b.end - b.start))
                                .width(contentWidth)
                                .wrapContentHeight()
                                .style(style)
                                .build();
                        }
                        // 透明 hit 覆盖层；onTap 的 y 为区域局部坐标，反查回编辑器行
                        auto hit =
                            components::mouseArea(ui, wrapId + ".hit")
                                .size(contentWidth, wrapH);
                        if (onBlockTap) {
                            hit.onTap([onBlockTap, k](
                                          const components::MouseEvent& ev) {
                                onBlockTap(k, static_cast<float>(ev.y));
                            });
                        }
                        hit.onHover([this, sid = b.stableId](bool h) {
                               if (h) previewHover_.enter(sid);
                               else previewHover_.leave(sid);
                           })
                            .build();
                        // 注入点收尾：实时态活动块在此关覆盖层命中，
                        // 否则覆盖层（命中取最上层）会抢走 input 的点击与焦点。
                        if (hooks.afterHitOverlay) {
                            hooks.afterHitOverlay(ui, k, wrapId + ".hit");
                        }
                    })
                    .build();
            }
        })
        .build();
    previewCodeCache_.pruneActive(activeCodeIds);
}

int MarkdownModule::liveCaretDisplayLine(const MdBlock& b, const std::string& blockText,
                         int local, float textWidth, float fontSize) {
    if (liveCaret_.stableId == b.stableId && liveCaret_.hash == b.hash &&
        liveCaret_.local == local && liveCaret_.width == textWidth &&
        liveCaret_.fontSize == fontSize) {
        return liveCaret_.line;
    }
    using InputModel = components::input_detail::InputModel;
    const auto lines = InputModel::measureLines(blockText, kLiveInputFontFamily,
                                                fontSize, textWidth);
    liveCaret_ = LiveCaretMemo{
        b.stableId, b.hash, local, textWidth, fontSize,
        InputModel::lineIndexFor(lines, local)};
    return liveCaret_.line;
}

void MarkdownModule::liveScrollFollowMaybe(eui::Ui& ui, const MdBlock& b,
                           const std::string& blockText, int localCursor,
                           float contentWidth, float viewportH) {
    if (!liveFollow_) return;                       // 用户滚动后：不干预
    if (viewportH <= 0.0f || contentWidth <= 0.0f) return;
    const float fontSize = mdEditorFontSize();
    const float textWidth =
        std::max(0.0f, contentWidth - kLiveInputInset * 2.0f);
    const float lineH = fontSize * kInputLineHeightFactor;
    const size_t local =
        localCursor <= 0
            ? 0
            : std::min(static_cast<size_t>(localCursor), blockText.size());
    const int caretLine = liveCaretDisplayLine(
        b, blockText, static_cast<int>(local), textWidth, fontSize);
    const float caretDocY = syncMap_.blockTopY(liveActiveBlock_) +
                            kLiveInputInset +
                            static_cast<float>(caretLine) * lineH;
    const MdLiveFollowDecision d = mdLiveFollowOffset(
        caretDocY, viewportH, liveScrollY_, liveMaxOffset_);
    if (!d.pull || d.offset == liveScrollY_) return;   // no-op 抑制
    liveScrollY_ = d.offset;
    // 程序化来源标记：onChange 凭它区分跟随落地与用户滚轮（回写不关 liveFollow_）；
    // 同帧消费，漏清由 drawLiveColumn 开头清零兜底。
    liveProgrammaticScroll_ = true;
    ui.setScrollOffset("md.live.scroll", d.offset);
    LOG_DEBUG("[live] follow: prog=1 applied offset=" +
              std::to_string(d.offset));
    LOG_DEBUG("[live] follow pull: caret=" + std::to_string(caretDocY) +
              " block=" + std::to_string(liveActiveBlock_) +
              " line=" + std::to_string(caretLine) +
              " offset=" + std::to_string(d.offset) + " viewport=" +
              std::to_string(viewportH) + " max=" +
              std::to_string(liveMaxOffset_));
}

void MarkdownModule::drawLiveColumn(eui::Ui& ui,
                    float bodyH,
                    float midW,
                    const core::theme::Tokens& t,
                    const components::theme::ThemeColorTokens& componentTheme) {
    // 整列 compose 耗时日志（DEBUG 级）
    const auto tCompose0 = std::chrono::steady_clock::now();
    // 程序化标记漏清在下帧开头清零（残留会把用户滚轮误判成程序化来源）。
    liveProgrammaticScroll_ = false;
    // 列壳与编辑态共用同一实现；两态都不钳列宽：正文列 = 栏宽 − 2×8px 留白。
    drawTextColumnChrome(
        ui, "md.live", bodyH, midW, t, componentTheme,
        [&](const TextColumnGeometry& g) {
            const components::MarkdownStyle style =
                scaledPreviewStyle(componentTheme);
            // 输入框状态换代（释放点）
            // enterLiveState 置位，本帧先释放再建 input；释放会清焦点，
            // 必须与 liveFocusPending_ 同帧补发配对。id 落在被释放前缀下
            // 会每次切块清焦点 + 清 InputState。
            if (liveInputStateFresh_) {
                liveInputStateFresh_ = false;
                ui.releaseStateScope(kLiveInputId);
                LOG_DEBUG("[live] release scope=md.live.input");
            }
            // 活动块锚（文档字节偏移）
            // 进实时态以编辑器文档光标为初始锚；此后每帧跟着活动 input 真实光标推进。
            LiveBlockCtx live;
            live.theme = &componentTheme;
            bool hasActive = false;
            if (!blockModel_.blocks().empty()) {
                const std::vector<MdBlock>& bs = blockModel_.blocks();
                const auto& lst = ui.state<
                    components::input_detail::InputModel::InputState>(
                    kLiveInputId);
                if (!liveCursorValid_) {
                    const auto& ed = ui.state<
                        components::input_detail::InputModel::InputState>(
                        kEditorInputId);
                    liveCursorByte_ =
                        ed.cursor > 0 ? static_cast<size_t>(ed.cursor) : 0;
                    liveCursorValid_ = true;
                    liveActiveBlock_ =
                        mdLiveActiveBlockIndex(blockModel_, liveCursorByte_);
                    // 本帧 input 尚未与本块同步，基准取块起点；真实落点由换文本帧反向写入。
                    liveInputStart_ = bs[liveActiveBlock_].start;
                    resetLiveUndoBaseline();
                } else if (liveCaretPending_) {
                    // 本帧锚由 setLiveCursor 显式定下；此刻 input 光标还是上一块
                    // 局部坐标，拿它反推会错位，本帧跳过重推。
                } else if (!mdLiveCompositionFrozen(lst.compositionText)) {
                    // 锚必须跟着真实光标走（文档字节 = 输入框文本起点 + 块内光标），
                    // 冻结在进态旧值会让重切后光标跳块首。
                    // 基准取上一帧的 liveInputStart_（重切后文本可跨块）。
                    const size_t local =
                        lst.cursor > 0 ? static_cast<size_t>(lst.cursor) : 0;
                    liveCursorByte_ = liveInputStart_ + local;
                    liveActiveBlock_ =
                        mdLiveActiveBlockIndex(blockModel_, liveCursorByte_);
                    liveInputStart_ = bs[liveActiveBlock_].start;
                }
                // caret 变了 = 一次光标事件（打字 / 方向键 / 点进输入框）→ 恢复跟随；
                // 用户滚轮已关掉它。
                if (lst.cursor != liveCursorSeen_) {
                    liveCursorSeen_ = lst.cursor;
                    liveFollow_ = true;
                }
                // 跟随判定 + 落地须在 scrollView build 之后（见 liveScrollFollowMaybe）。
                live.activeBlock = liveActiveBlock_;
                hasActive = true;
            }
            // 实时态块级特判经注入点传入
            // 活动块 = 源码态 input：wrapHeight 改贴合高、drawContent 改由 input
            // 呈现、afterHitOverlay 关掉覆盖层命中；非活动块走默认路。
            BlockFlowHooks hooks;
            // 单块高度上限：贴合高超过视口 0.7 倍时封顶（封顶后 input 内部滚动，
            // 外层滚轮在该块成死区）。liveInputCapped_ 供 over 探针区分预期与回归。
            hooks.wrapHeight = [this, &live, viewportH = g.h](
                                   size_t k, const MdBlock& b,
                                   float geometryHeight,
                                   float contentWidth) {
                if (k != live.activeBlock) return geometryHeight;
                const float fit =
                    liveFitHeight(b, liveBlockText(b), contentWidth);
                const float h = mdLiveCappedInputHeight(fit, viewportH);
                liveInputCapped_ = h < fit;
                if (liveInputCapped_) {
                    LOG_DEBUG("[live] input cap fit=" +
                              std::to_string(fit) + " h=" + std::to_string(h) +
                              " viewport=" + std::to_string(viewportH) +
                              " stableId=" + std::to_string(b.stableId));
                }
                return h;
            };
            hooks.drawContent = [this, &live, &style](
                                    eui::Ui& inner, size_t k,
                                    const MdBlock& b,
                                    const std::string& wrapId,
                                    float contentWidth,
                                    float wrapHeight) {
                if (k != live.activeBlock) return false;
                drawLiveInputBlock(inner, b, liveBlockText(b), wrapId,
                                   contentWidth, wrapHeight, style, live);
                return true;  // 已画：跳过下方渲染通路
            };
            // 不捕获 this：clang -Wunused-lambda-capture（GCC 不报）。
            hooks.afterHitOverlay = [&live](eui::Ui& inner, size_t k,
                                                  const std::string& hitId) {
                if (k != live.activeBlock) return;
                if (auto* hitEl = inner.find(hitId)) {
                    hitEl->hitTestMode = core::dsl::HitTestMode::None;
                }
            };
            // 单一滚动权威 = 本 scrollView；活动块 input 内容贴合不吃滚轮
            components::scrollView(ui, "md.live.scroll")
                .position(g.x, g.y)
                .size(g.w, g.h)
                .gap(t.sizes.spacing)
                .step(56.0f)
                .theme(componentTheme)
                .offset(liveScrollY_)
                // contentKey 不用：实态点击切块会换 key → scrollView 视为全新内容 → 滚动归零。
                // 若用 contentKey 带 activeBlockStableId，cache 只在点击帧 miss；不带则
                // cache 永久返回旧活动块高度、maxOffset 永误。空字符串走每帧实量，O(blocks)
                // 成本可忽略（rebuildSyncMap 同文零成本、仅 estimateHeight 逐块重付）。
                // 规则性注释：实时模式点击不得重建编辑器子树 / id 必须稳定，否则滚动重置。
                .onChange([this](float v) {
                    // 程序化来源（跟随落地 echo）：只记账、不关跟随
                    if (liveProgrammaticScroll_) {
                        liveProgrammaticScroll_ = false;
                        liveScrollY_ = v;
                        return;
                    }
                    // 用户滚动：记账 + 关跟随（直到下一次光标事件）
                    liveScrollY_ = v;
                    liveFollow_ = false;
                    LOG_DEBUG("[live] follow: wheel disarmed");
                })
                .content([&](eui::Ui& inner, float contentWidth, float viewportHeight) {
                    (void)viewportHeight;
                    // 跟随判据的排版宽（滞后一帧，窗口缩放时才变）
                    liveContentWidth_ = contentWidth;
                    drawBlockFlow(inner, contentWidth, t, style, "md.live",
                                  [this, &ui](size_t k, float y) {
                                      handleLiveBlockClick(ui, k, y);
                                  },
                                  hooks);
                })
                .build();
            // 按框架声明值回读钳位：内容变矮时记账值随之收敛
            if (const auto* svEl = ui.find("md.live.scroll")) {
                liveMaxOffset_ = svEl->scrollMaxOffset;
                liveScrollY_ = svEl->scrollOffset;
            }
            // 跟随判定 + 落地须在 scrollView build + 回读钳位之后。评估守卫：
            // 组字在飞不评、liveCaretPending_ 帧不评、几何表滞后一帧（跨块错位下帧自纠）。
            if (liveFollow_ && !liveCaretPending_ &&
                liveActiveBlock_ < blockModel_.blocks().size()) {
                const auto& lstFollow = ui.state<
                    components::input_detail::InputModel::InputState>(
                    kLiveInputId);
                if (!mdLiveCompositionFrozen(lstFollow.compositionText)) {
                    const MdBlock& ab =
                        blockModel_.blocks()[liveActiveBlock_];
                    liveScrollFollowMaybe(ui, ab, liveBlockText(ab),
                                          lstFollow.cursor,
                                          liveContentWidth_, g.h);
                }
            }
            // 写光标一次性请求已消费，帧末清位（下帧锚由真实光标重推，写了也是同值）。
            liveCaretPending_ = false;

            // 进实时态首帧同帧补发 requestFocus（目标须已在当帧树中），只发
            // <input id>.hit（组件按 .hit 判聚焦，发基 id 会不画 caret）。
            if (liveFocusPending_) {
                liveFocusPending_ = false;
                if (hasActive) ui.requestFocus(kLiveInputHitId);
            }
            // 整列 compose 耗时日志（含 scrollView 测量 pass）
            const auto tCompose1 = std::chrono::steady_clock::now();
            char msBuf[32];
            std::snprintf(msBuf, sizeof(msBuf), "%.2f",
                          std::chrono::duration<double, std::milli>(
                              tCompose1 - tCompose0)
                              .count());
            LOG_DEBUG(std::string("[live] frame compose ms=") + msBuf);
        });
}

void MarkdownModule::drawPreviewCodeBlock(eui::Ui& ui,
                          const std::string& partId,
                          const MdPreviewCodeHighlight& hl,
                          float contentWidth,
                          const components::MarkdownStyle& style,
                          bool hovered,
                          const eui::Color& hoverTint) {
    const float textWidth =
        std::max(0.0f, contentWidth - style.codePadding * 2.0f);
    const float textHeight = components::detail::estimateMarkdownTextHeight(
        hl.displayText, textWidth, style.codeSize, style.codeSize + 6.0f);
    const float blockHeight = textHeight + style.codePadding * 2.0f;
    ui.stack(partId)
        .width(contentWidth)
        .height(blockHeight)
        .content([&] {
            ui.rect(partId + ".bg")
                .size(contentWidth, blockHeight)
                .color(hovered
                           ? mdHoverMix(style.codeBackground, hoverTint, 0.5f)
                           : style.codeBackground)
                .radius(style.radius)
                .build();
            ui.text(partId + ".text")
                .x(style.codePadding)
                .y(style.codePadding)
                .width(textWidth)
                .height(textHeight)
                .text(hl.displayText)
                .fontFamily(style.codeFontFamily)
                .fontSize(style.codeSize)
                .lineHeight(style.codeSize + 6.0f)
                .wrap()
                .color(style.codeText)
                .runs(hl.runs)
                .build();
        })
        .build();
}

std::string MarkdownModule::liveBlockText(const MdBlock& b) const {
    return blockModel_.text().substr(b.start, b.end - b.start);
}

int MarkdownModule::liveActiveBlockStableId() const {
    const std::vector<MdBlock>& bs = blockModel_.blocks();
    return liveActiveBlock_ < bs.size()
               ? bs[liveActiveBlock_].stableId
               : -1;
}

float MarkdownModule::liveFitHeight(const MdBlock& b, const std::string& blockText,
                    float contentWidth) {
    const float fontSize = mdEditorFontSize();
    const float textWidth =
        std::max(0.0f, contentWidth - kLiveInputInset * 2.0f);
    if (liveHeight_.stableId == b.stableId && liveHeight_.hash == b.hash &&
        liveHeight_.width == textWidth && liveHeight_.fontSize == fontSize) {
        return liveHeight_.height;
    }
    const MdLiveLineCountFn lineCount =
        [fontSize](const std::string& text, float width) {
            using InputModel = components::input_detail::InputModel;
            return static_cast<int>(
                InputModel::measureLines(text, kLiveInputFontFamily, fontSize,
                                         width).size());
        };
    liveHeight_ = LiveHeightMemo{b.stableId, b.hash, textWidth, fontSize,
                                 mdLiveInputHeight(blockText, textWidth,
                                                   fontSize, kLiveInputInset,
                                                   lineCount)};
    return liveHeight_.height;
}

void MarkdownModule::syncDocumentFromLiveInput(const std::string& blockText) {
    const std::vector<MdBlock>& blocks = blockModel_.blocks();
    if (liveActiveBlock_ >= blocks.size()) return;   // 防御：无块不写
    const size_t start = blocks[liveActiveBlock_].start;
    const size_t end = blocks[liveActiveBlock_].end;
    const std::string& doc = blockModel_.text();
    if (start > end || end > doc.size()) return;
    // 拼装须在 onEditorTextChanged 之前完成：它进 setEditorText →
    // blockModel_.update，blocks/doc 引用随即失效。
    std::string next;
    next.reserve(doc.size() + blockText.size());
    next.append(doc, 0, start);
    next.append(blockText);
    next.append(doc, end, std::string::npos);
    // 撤销快照须在写文档之前推（= 本次编辑前的那一版）
    pushLiveUndoSnapshot();
    onEditorTextChanged(next);
}

void MarkdownModule::drawLiveInputBlock(eui::Ui& ui,
                        const MdBlock& b,
                        const std::string& blockText,
                        const std::string& wrapId,
                        float contentWidth,
                        float fitHeight,
                        const components::MarkdownStyle& style,
                        const LiveBlockCtx& live) {
    using InputState = components::input_detail::InputModel::InputState;
    InputState& st = ui.state<InputState>(kLiveInputId);
    // 换文本帧：input 走「外部改值」把光标搬到新文本末尾，须在 build 之后
    // 反向写正确落点（早写会被 build 内重载覆盖）。
    const bool swapped = (st.text != blockText);
    components::input(ui, kLiveInputId)
        .position(0.0f, 0.0f)
        .size(contentWidth, fitHeight)
        .value(blockText)
        .placeholder("")  // 活动块恒有源码（空文档无块 = 不建 input）
        .multiline(true)
        .fontSize(mdEditorFontSize())
        .inset(kLiveInputInset)
        .theme(*live.theme)
        .onChange([this](const std::string& v) { syncDocumentFromLiveInput(v); })
        // 块内编辑键 + 块边界键，passthrough 先于内置（返回 true 即拦截）。
        .onKeyEvent([this, &ui](const eui::KeyEvent& event) {
            return handleLiveKey(ui, event);
        })
        // 失焦提交（语义见 commitLiveInputOnBlur）：焦点离开活动 input 时收口。
        .onFocus([this, &ui](bool focused) {
            if (!focused) commitLiveInputOnBlur(ui);
        })
        // 实时态行级高亮：独立实例 liveHighlighter_ 喂块文本（runs 以块首为原点）；
        // 与编辑态混用（交替喂整篇/块文本）会抖动围栏缓存。
        .lineStyles([this, &ui](const std::string& lineText,
                                int lineStartByte, int lineNo) {
            (void)lineNo;
            using InputState =
                components::input_detail::InputModel::InputState;
            auto& st = ui.state<InputState>(kLiveInputId);
            return liveHighlighter_.runsForLine(
                st.preedit ? st.preedit->text : st.text, lineText,
                lineStartByte);
        })
        .build();
    // 写光标两条来路（都在 build 之后）：swapped（换文本帧）/
    // liveCaretPending_（同块定位帧，文本未变须显式写）。
    if (swapped || liveCaretPending_) {
        st.cursor =
            static_cast<int>(mdLiveBlockLocalOffset(b, liveCursorByte_));
        st.selectionStart = st.cursor;
        st.selectionEnd = st.cursor;
        st.dragAnchor = st.cursor;
        st.hasPreferredCursorX = false;
        st.followCaret = true;     // 落点即目标（贴合高度下框内无滚动）
        st.verticalScroll = 0.0f;  // 切块/定位帧清上一块的滚动残余
        st.horizontalScroll = 0.0f;
    }
    // 失焦提交安全判据：输入框文本 == 当前块源。外部重载/撤销换代帧
    // 不满足，故不会把旧文本灌回新文档。
    if (st.text == blockText) liveSyncedRev_ = editorTextRev_;

    // 活动块边框：主色半透明 1px + 透明填充，不参与命中测试
    ui.rect(wrapId + ".frame")
        .size(contentWidth, fitHeight)
        .radius(style.radius)
        .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
        .border(1.0f, components::theme::withAlpha(
                         live.theme->primary, kLiveActiveFrameAlpha))
        .build();
    // 记录式校验：活动 input 应内容贴合（无内部滚动条）。框架不暴露
    // maxVerticalScroll，用「是否注册滚轮处理器」做 0/1 探针。
    const auto* hit = ui.find(kLiveInputHitId);
    const int over = (hit != nullptr && hit->onScroll) ? 1 : 0;
    if (over > 0) {
        if (liveInputCapped_) {
            LOG_DEBUG("[live] input over=1（封顶块内滚动，预期）stableId=" +
                      std::to_string(b.stableId) +
                      " h=" + std::to_string(fitHeight));
        } else {
            LOG_WARN("[live] input over=1 活动块高度未贴合（应 0）: stableId=" +
                     std::to_string(b.stableId) +
                     " 声明高=" + std::to_string(fitHeight));
        }
    } else {
        LOG_DEBUG("[live] input over=0");
    }
}

void MarkdownModule::setLiveCursor(size_t docByte, const char* reason) {
    const size_t size = editorText_.size();
    liveCursorByte_ = docByte < size ? docByte : size;
    liveCursorValid_ = true;
    const std::vector<MdBlock>& bs = blockModel_.blocks();
    if (!bs.empty()) {
        liveActiveBlock_ = mdLiveActiveBlockIndex(blockModel_, liveCursorByte_);
        liveInputStart_ = bs[liveActiveBlock_].start;
        // 块切换前关掉撤销合并组：否则「切走又切回同一块」会并成一次撤销。
        // 只关组不推条目（多推会多出空撤销中间站并作废 redo 链）。
        if (liveUndoSeeded_ && liveActiveBlock_ != liveUndoBlock_) {
            liveUndo_.markBoundary();
            liveUndoBlock_ = liveActiveBlock_;
        }
    }
    liveCaretPending_ = true;
    liveFocusPending_ = true;
    // 定位即跟随。本帧不评估（input 光标还是上一块局部坐标），下一帧按真实落点判一次。
    liveFollow_ = true;
    LOG_DEBUG(std::string("[live] setLiveCursor(") + reason + ") byte=" +
              std::to_string(liveCursorByte_) + " block=" +
              std::to_string(liveActiveBlock_));
}

void MarkdownModule::handleLiveBlockClick(eui::Ui& ui, size_t blockIndex, float localY) {
    if (mode_ != ViewMode::Live || editorText_.empty()) return;
    using InputState = components::input_detail::InputModel::InputState;
    if (mdLiveCompositionFrozen(
            ui.state<InputState>(kLiveInputId).compositionText)) {
        liveFocusPending_ = true;
        LOG_DEBUG("[live] 组字中拒绝切块: 块#" + std::to_string(blockIndex));
        return;
    }
    const MdPreviewJump jump = mdResolvePreviewJump(
        editorText_, blockModel_.blocks(), syncMap_, blockIndex, localY);
    if (!jump.ok) return;
    setLiveCursor(jump.cursorByte, "点击渲染块");
    LOG_INFO("实时态块点击: 块#" + std::to_string(jump.blockIndex) +
             " stableId=" + std::to_string(jump.stableId) +
             " → 文档字节 " + std::to_string(jump.cursorByte) + "（行 " +
             std::to_string(jump.targetLine + 1) + "）");
}

void MarkdownModule::commitLiveInputOnBlur(eui::Ui& ui) {
    using InputState = components::input_detail::InputModel::InputState;
    InputState& st = ui.state<InputState>(kLiveInputId);
    if (mdLiveCompositionFrozen(st.compositionText)) {
        st.compositionText.clear();
        ++st.compositionRevision;
        LOG_DEBUG("[live] 失焦清组字");
    }
    const std::vector<MdBlock>& bs = blockModel_.blocks();
    if (liveActiveBlock_ >= bs.size()) return;
    if (st.text.empty() || liveSyncedRev_ != editorTextRev_) return;
    if (st.text == liveBlockText(bs[liveActiveBlock_])) return;   // 无未提交内容
    const std::string pending = st.text;   // 取副本：上行会换掉 blockModel_ 内部文本
    LOG_DEBUG("[live] 失焦提交未上行文本 len=" + std::to_string(pending.size()));
    syncDocumentFromLiveInput(pending);
}

bool MarkdownModule::handleLiveKey(eui::Ui& ui, const eui::KeyEvent& event) {
    using InputState = components::input_detail::InputModel::InputState;
    if (mode_ != ViewMode::Live) return false;
    if (!event.isDown()) return false;   // 释放/重复：内置只认按下（同 handleEditorKey）
    InputState& st = ui.state<InputState>(kLiveInputId);
    if (mdLiveCompositionFrozen(st.compositionText)) return false;

    const bool shortcut = event.modifiers.shortcut();
    const bool alt = event.modifiers.alt;
    const bool plain = !shortcut && !event.modifiers.shift && !alt &&
                       !event.modifiers.super;

    // 撤销 / 重做走模块层快照栈，无论有无可用快照一律消费（交回内置会走
    // framework per-instance 栈，存块局部文本，单输入下跨块混叠）。
    if (shortcut && !alt) {
        const bool redoKey = event.key == eui::InputKey::Y ||
                             (event.key == eui::InputKey::Z &&
                              event.modifiers.shift);
        if (redoKey) {
            applyLiveRedo();
            return true;
        }
        if (event.key == eui::InputKey::Z) {
            applyLiveUndo();
            return true;
        }
    }

    // 块内按键：与编辑态同一批纯函数
    EditBuffer buf{st.text, st.cursor, st.selectionStart, st.selectionEnd};
    bool handled = false;
    if (event.key == eui::InputKey::Tab && !shortcut && !alt) {
        if (!applyTabTable(buf, event.modifiers.shift)) {
            applyTab(buf, event.modifiers.shift);
        }
        handled = true;
    } else if (event.key == eui::InputKey::Enter && plain) {
        handled = applyEnterList(buf);   // 列表行续行；其余交回内置插 '\n'
    } else if (shortcut && !event.modifiers.shift && !alt) {
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
    if (handled) {
        commitLiveEditBuffer(ui, buf);
        return true;
    }

    // 块边界键：文档字节偏移 = 输入框文本起点 + 块内光标；
    // 带 Ctrl/Alt/Super 的其它组合一律不碰。
    if (shortcut || alt || event.modifiers.super) return false;
    const std::vector<MdBlock>& blocks = blockModel_.blocks();
    if (blocks.empty()) return false;
    const size_t local = st.cursor > 0 ? static_cast<size_t>(st.cursor) : 0;
    const size_t docByte = liveInputStart_ + local;
    const bool hasSelection = st.selectionStart != st.selectionEnd;

    if (event.key == eui::InputKey::Up || event.key == eui::InputKey::Down) {
        // Shift+↑↓ 与块内选区照旧（跨块选区不做）
        if (event.modifiers.shift || hasSelection) return false;
        const int dir = (event.key == eui::InputKey::Up) ? -1 : 1;
        const size_t target =
            mdLiveVerticalBlockTarget(editorText_, blocks, docByte, dir);
        if (target == kMdLiveNoConsume) return false;   // 首块 ↑ / 末块 ↓：内置
        setLiveCursor(target, dir < 0 ? "跨块上移" : "跨块下移");
        LOG_DEBUG(std::string("[live] 跨块") + (dir < 0 ? "上移" : "下移") +
                  " byte=" + std::to_string(docByte) + " → " +
                  std::to_string(target));
        return true;
    }
    if (event.key == eui::InputKey::Backspace || event.key == eui::InputKey::Delete) {
        if (hasSelection) return false;   // 块内选区删除交回内置
        MdLiveBoundaryEdit edit;
        const int dir = (event.key == eui::InputKey::Backspace) ? -1 : 1;
        if (!mdLiveBoundaryEdit(editorText_, blocks, docByte, dir, edit)) {
            return false;   // 不在块边界 / 首块退格 / 文末删除 / 普通字符删除
        }
        applyLiveBoundaryEdit(edit, dir < 0 ? "块首退格" : "块尾删除");
        return true;
    }
    return false;
}

void MarkdownModule::commitLiveEditBuffer(eui::Ui& ui, EditBuffer& buf) {
    using InputState = components::input_detail::InputModel::InputState;
    InputState& st = ui.state<InputState>(kLiveInputId);
    if (buf.text == st.text) {
        // 纯光标/选区移动：只更新位置，不进 undo / 不 bump 修订号。
        if (buf.cursor == st.cursor && buf.selStart == st.selectionStart &&
            buf.selEnd == st.selectionEnd) {
            return;
        }
        st.cursor = buf.cursor;
        st.selectionStart = buf.selStart;
        st.selectionEnd = buf.selEnd;
        st.dragAnchor = buf.cursor;
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
    st.dragAnchor = buf.cursor;
    st.hasPreferredCursorX = false;
    st.followCaret = true;
    syncDocumentFromLiveInput(st.text);
}

void MarkdownModule::applyLiveBoundaryEdit(const MdLiveBoundaryEdit& edit, const char* reason) {
    if (edit.eraseEnd <= edit.eraseStart || edit.eraseEnd > editorText_.size()) {
        return;
    }
    std::string next = editorText_;
    next.erase(edit.eraseStart, edit.eraseEnd - edit.eraseStart);
    pushLiveUndoSnapshot();   // 快照 = 改之前的那一版
    onEditorTextChanged(next);
    setLiveCursor(edit.cursor, reason);
    LOG_INFO(std::string("实时态边界键(") + reason + "): 删 [" +
             std::to_string(edit.eraseStart) + "," +
             std::to_string(edit.eraseEnd) + ") 光标 " +
             std::to_string(edit.cursor));
}

void MarkdownModule::pushLiveUndoSnapshot() {
    if (!liveUndoSeeded_) return;   // 基线未就位（空文档 / 尚未进实时态）
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    const size_t capBefore = liveUndo_.capacity();
    liveUndo_.push(editorText_, liveCursorByte_,
                   liveActiveBlock_ == liveUndoBlock_, nowMs);
    liveUndoBlock_ = liveActiveBlock_;
    if (liveUndo_.capacity() != capBefore) {
        // 文档 > 2MB 时撤销栈降级；只在档位切换那一次记日志
        LOG_WARN("[live] 撤销栈降级：文档 " + std::to_string(editorText_.size()) +
                 " 字节 > 2MB，容量 " + std::to_string(capBefore) + " → " +
                 std::to_string(liveUndo_.capacity()));
    }
}

void MarkdownModule::applyLiveUndo() {
    std::string text = editorText_;
    size_t cursor = liveCursorByte_;
    if (!liveUndo_.undo(text, cursor)) {
        LOG_DEBUG("[live] Ctrl+Z: 无可撤销快照");
        return;
    }
    LOG_INFO("实时态撤销: " + std::to_string(editorText_.size()) + " → " +
             std::to_string(text.size()) + " 字节, 光标 " + std::to_string(cursor));
    onEditorTextChanged(text);
    setLiveCursor(cursor, "撤销");
}

void MarkdownModule::applyLiveRedo() {
    std::string text = editorText_;
    size_t cursor = liveCursorByte_;
    if (!liveUndo_.redo(text, cursor)) {
        LOG_DEBUG("[live] Ctrl+Y: 无可重做快照");
        return;
    }
    LOG_INFO("实时态重做: " + std::to_string(editorText_.size()) + " → " +
             std::to_string(text.size()) + " 字节, 光标 " + std::to_string(cursor));
    onEditorTextChanged(text);
    setLiveCursor(cursor, "重做");
}

void MarkdownModule::resetLiveUndoBaseline() {
    liveUndo_.reset(editorText_, liveCursorByte_);
    liveUndoBlock_ = mdLiveActiveBlockIndex(blockModel_, liveCursorByte_);
    liveUndoSeeded_ = true;
}

float MarkdownModule::computeSyncOffset(eui::Ui& ui,
                        float pvW,
                        float pvH,
                        const components::MarkdownStyle& style) {
    if (editorText_.empty()) return 0.0f;
    const auto& st = ui.state<components::input_detail::InputModel::InputState>(kEditorInputId);
    const float topLine =
        st.verticalScroll /
        (mdEditorFontSize() * kInputLineHeightFactor);
    const float width = lastPvContentWidth_ > 0.0f ? lastPvContentWidth_ : pvW;
    rebuildSyncMap(width, style);
    const float y = syncMap_.previewYForEditorLine(topLine);
    return std::max(0.0f, y - pvH * 0.2f);
}

void MarkdownModule::rebuildSyncMap(float width, const components::MarkdownStyle& style) {
    if (blockModel_.text() != editorText_) {
        blockModel_.update(editorText_);  // 防御：理论不可达（同预览路径）
    }
    syncMap_.rebuild(
        editorText_, blockModel_.blocks(), editorTextRev_, width,
        style.blockGap,
        [this, width, &style](const MdBlock& b) {
            // 与预览逐块渲染同一份块源码/宽度/style ⇒ 测量与预览布局同源。
            return components::MarkdownBuilder::estimateHeight(
                editorText_.substr(b.start, b.end - b.start), width, style);
        });
}

void MarkdownModule::handlePreviewBlockClick(eui::Ui& ui, size_t blockIndex, float localY) {
    if (mode_ == ViewMode::Preview || editorText_.empty()) return;
    const MdPreviewJump jump = mdResolvePreviewJump(
        editorText_, blockModel_.blocks(), syncMap_, blockIndex, localY);
    if (!jump.ok) return;
    auto& st =
        ui.state<components::input_detail::InputModel::InputState>(kEditorInputId);
    st.cursor = static_cast<int>(jump.cursorByte);
    st.selectionStart = st.cursor;
    st.selectionEnd = st.cursor;
    st.dragAnchor = st.cursor;
    st.hasPreferredCursorX = false;
    st.followCaret = true;
    ui.requestFocus(kEditorInputHitId);  // 帧末消费（Runtime 机制，同回车聚焦）
    LOG_INFO("预览块点击: 块#" + std::to_string(jump.blockIndex) +
             " stableId=" + std::to_string(jump.stableId) +
             " kind=" +
             mdBlockKindName(blockModel_.blocks()[jump.blockIndex].kind) +
             " → 编辑器行 " + std::to_string(jump.targetLine + 1));
}

int MarkdownModule::activeOutlineIndex(eui::Ui& ui) {
    if (outline_.empty()) return -1;
    int line;
    if (mode_ == ViewMode::Preview) {
        return activeOutlineIdx_;
    }
    if (mode_ == ViewMode::Live) {
        line = byteOffsetToLine(editorText_, liveCursorByte_);
    } else {
        const auto& st = ui.state<components::input_detail::InputModel::InputState>(kEditorInputId);
        line = byteOffsetToLine(st.text, static_cast<size_t>(st.cursor));
    }
    int idx = -1;
    for (size_t i = 0; i < outline_.size(); ++i) {
        if (outline_[i].lineNo <= line) idx = static_cast<int>(i);
        else break;
    }
    return idx;
}

void MarkdownModule::jumpToHeading(eui::Ui& ui, size_t outlineIndex,
                   const components::theme::ThemeColorTokens& componentTheme) {
    if (outlineIndex >= outline_.size()) return;
    const MdHeading& h = outline_[outlineIndex];
    activeOutlineIdx_ = static_cast<int>(outlineIndex);
    if (mode_ == ViewMode::Preview) {
        const float width = lastPvContentWidth_;
        previewOffset_ = components::MarkdownBuilder::estimateHeight(
            editorText_.substr(0, h.byteOffset), width,
            scaledPreviewStyle(componentTheme));
        syncPreviewOnEnter_ = false;
        return;
    }
    // 实时态唯一文档级光标写点 = setLiveCursor（写 kEditorInputId 是死写：
    // 该输入不在帧树，且绕过换块/写块内光标/补发聚焦整条链路）。
    if (mode_ == ViewMode::Live) {
        setLiveCursor(h.byteOffset, "大纲跳转");
        return;
    }
    auto& st = ui.state<components::input_detail::InputModel::InputState>(kEditorInputId);
    st.cursor = static_cast<int>(h.byteOffset);
    st.selectionStart = st.cursor;
    st.selectionEnd = st.cursor;
    st.dragAnchor = st.cursor;
    st.hasPreferredCursorX = false;
    st.followCaret = true;
}

} // namespace module::markdown
