// 智能体模块·UI 块（AgentModule 成员函数定义）：任务弹窗渲染（composeTaskDialog）、
// AI 草稿子弹窗（composeDraftDialog + 开关）、删除确认弹窗与真删除、详情弹窗开关态
// 与消息展开态、消息行渲染（renderMessageRow）。
// 弹窗关闭态不建内容节点（存在性即开关状态，测试据此判开合）；弹窗字段标签一律
// .caption：dropdown 内部占用 <id>.label，同 id 会串位。
// 拆分（04 号票）产物：类定义在 agent_module.h，跨 TU 共享助手在
// agent_module_internal.h；本文件只承载成员函数体与 TU 私有助手，
// 纯代码搬移，逻辑与签名零变化。

#include "agent_module_internal.h"
#include "module_agent/agent_task_templates.h"

#include <ui_kit/glass.h>       // glassFill/glassShadowColor（池170 v2 弹窗壳）
#include <ui_kit/materials.h>   // itemSurface（池127 L2 卡面）

#include <cstddef>
#include <core/str_util.h>      // toLowerAscii（搜索不区分大小写折叠）

namespace module::agent {

namespace {

// 弹窗字段标签：显式垂直居中（光靠 lineHeight 不居中）+ 显式定位；id 用 .caption 避开 dropdown 的 .label
void fieldCaption(::eui::Ui& ui, const std::string& id, const std::string& text,
                  float x, float y, float width, float height,
                  const ::core::Color& color) {
    ui.text(id)
        .position(x, y)
        .size(width, height)
        .text(text)
        .fontSize(core::theme::scaled(12.0f))
        .lineHeight(core::theme::scaled(20.0f))
        .verticalAlign(eui::VerticalAlign::Center)
        .color(color)
        .build();
}

// 消息收起态预览：解码纯文本前 120 字节，统一走 message_blocks
std::string messagePreview(const AgentMessageRow& m) {
    return messagePreviewFromJson(m.contentJson, 120);
}

// 任务弹窗几何（UAT5 重设计：弹性宽 = max(720, 窗口/2)，demo 智能体面板的紧凑行高）
// kDialogGap/kDialogLabelW/kDialogRowH 已收进 agent_ui_shared.h（与详情弹窗共用真源）
constexpr float kTaskDialogH = 520.0f;        // UAT6 竖向导航案：标题栏/底栏各 44 带 + 内容区 432
constexpr float kGroupTitleH = 20.0f;        // 分组标题高（14px 加粗 + 行高 20）

// AI 草稿子弹窗几何（Standard 460 宽；y 均为本弹窗面板内坐标，与任务弹窗互不影响）。
// UAT6 重排：输入加高 88→140、结果行让位下移、按钮组迁底部按钮带（行顶 y=h-60、
// 行底距弹窗底 20，与 dialog_form 同口径）并整体居中。
constexpr float kDraftDialogH = 380.0f;
constexpr float kDraftInputH = 140.0f;
constexpr float kDraftBtnH = 40.0f;          // 底部按钮带按钮高（dialog_form 同档）
constexpr float kDraftBtnW = 110.0f;
constexpr float kDraftBtnGap = 12.0f;
constexpr float kDraftResultY = 244.0f;      // 结果行顶（输入底 232 + 12 呼吸）

// 迭代上限容错解析：非数字忽略（输入中间态）、>1000 截断防溢出、无数字 → 0（保存时 clamp）
int parseIterInput(const std::string& text) {
    int value = 0;
    bool any = false;
    for (const char c : text) {
        if (c < '0' || c > '9') continue;
        any = true;
        value = value * 10 + (c - '0');
        if (value > 1000) return 1000;
    }
    return any ? value : 0;
}

} // namespace

// 打开详情弹窗：每开一次回到「最近一次 run」（selectedRunId_ 清空，refreshData 回落首条）。
// 布局态一并清零（回 tab 0「执行记录」、消息区收起），弹窗不记忆上一次的分支状态
void AgentModule::openTaskDetail(int64_t taskId) {
    detailTaskId_ = taskId;
    selectedRunId_.reset();
    detailMessages_.clear();
    detailMessagesError_.clear();
    detailPlan_.reset();
    detailPlanKey_.clear();
    // 消息展开态按 seq 定位且只在 run 内有意义：换任务即清
    expandedMsgSeqs_.clear();
    autoExpandedSeq_.reset();
    detailTab_ = 0;
    messagesOpen_ = false;
    ++msgScrollVersion_;
    tasksDirty_ = true;
}

// 关详情弹窗：只退弹窗态，不触发重查
void AgentModule::closeTaskDetail() {
    detailTaskId_.reset();
    detailTaskRow_.reset();
    selectedRunId_.reset();
    detailMessages_.clear();
    detailMessagesError_.clear();
    detailPlan_.reset();
    detailPlanKey_.clear();
    expandedMsgSeqs_.clear();
    autoExpandedSeq_.reset();
    detailTab_ = 0;
    messagesOpen_ = false;
    ++msgScrollVersion_;
}

// 默认展开最近一条带 text 的 assistant 消息；只在 seq 变化时改展开集，手动收起不被每帧顶回
void AgentModule::applyDefaultMessageExpansion() {
    std::optional<int64_t> target;
    for (auto it = detailMessages_.rbegin(); it != detailMessages_.rend(); ++it) {
        if (it->role != "assistant") continue;
        const std::vector<MessageBlock> blocks = decodeMessageBlocks(it->contentJson);
        for (const auto& b : blocks) {
            if (b.type == MessageBlock::Type::Text && !b.text.empty()) {
                target = it->seq;
                break;
            }
        }
        if (target) break;
    }
    if (target == autoExpandedSeq_) return;
    if (autoExpandedSeq_) expandedMsgSeqs_.erase(*autoExpandedSeq_);
    autoExpandedSeq_ = target;
    if (target) {
        expandedMsgSeqs_.insert(*target);
        ++msgScrollVersion_;   // 内容高变了 → scrollView 重测
    }
}

void AgentModule::toggleMessageExpanded(int64_t seq) {
    if (expandedMsgSeqs_.count(seq) > 0) {
        expandedMsgSeqs_.erase(seq);
    } else {
        expandedMsgSeqs_.insert(seq);
    }
    ++msgScrollVersion_;
}

// 消息卡渲染（池162 重设计）：卡面 = L2 itemSurface + 1px glassBorder + 果冻按压，
// 内边距 12；.head（徽章 pill + 时间 + 弹簧 + 展开/收起 + [复制] chip 48×24，头高 24）、
// 收起态 .preview、展开态逐块 .blk.<j>（Text/Plain 裸文本、ToolUse 浅染内嵌块 28 高、
// ToolResult 左 3px 语义条：红仅错误）；正文/预览/工具输出统一 fsS/20（spec §9.2），
// text 块 maxWidth 必须显式给（measureTextSize 只认 element.maxWidth）。
// 整卡可点开合：点击落 .hit 命中面（z1），头行 chips 在 z2 各自拦截不冒泡到卡面
void AgentModule::renderMessageRow(
    ::eui::Ui& inner, float cw, const std::string& prefix, size_t index,
    const AgentMessageRow& msg, int64_t refSec, const ::core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& tokens) {
    const float fsS = t.sizes.fontSizeSmall;
    // §9.2 消息行高下限 20：fontScale <1 时 lhS 派生值会更小，钳回 20 防拥挤
    const float msgLh = std::max(fsS * 1.4f, 20.0f);
    const std::string mid = prefix + ".msg." + std::to_string(index);
    const bool expanded = expandedMsgSeqs_.count(msg.seq) > 0;
    const float cardPad = core::theme::scaled(12.0f);
    const float contentW = std::max(0.0f, cw - 2.0f * cardPad);
    // 高度估算给卡内宽（文字换行按内容宽算），卡总高 = 内容高 + 2×pad
    const float contentH =
        estimateMessageHeight(msg, expanded, contentW, fsS, msgLh);
    const float cardH = contentH + 2.0f * cardPad;
    const std::vector<MessageBlock> blocks =
        decodeMessageBlocks(msg.contentJson);
    const std::string badge = messageRoleBadge(msg);
    const bool toolRole = badge == "工具";
    const bool pressed = pressedMsgSeqs_.count(msg.seq) > 0;
    constexpr float kMsgTimeW = 110.0f;   // MM-DD HH:MM:SS（14 字符）@13 ≈ 96
    constexpr float kChipW = 48.0f;       // 展开/收起与复制同规格（§3.3 同卡同几何）
    constexpr float kCardRadius = 6.0f;
    const float badgeFs = core::theme::scaled(12.0f);
    const float badgeH = core::theme::scaled(20.0f);
    const float badgeW = core::TextPrimitive::measureTextWidth(
                            badge, "", badgeFs, 600) +
                        core::theme::scaled(16.0f);   // pill 内边距 8×2
    const eui::Color kTransparent{0.0f, 0.0f, 0.0f, 0.0f};

    inner.stack(mid)
        .size(cw, cardH)
        .content([&] {
            // 卡面（L2 itemSurface，池127 档）：果冻按压 0.96 / 160ms 回弹（可点项）
            inner.rect(mid + ".bg")
                .size(cw, cardH)
                .color(ui_kit::itemSurface(t))
                .radius(kCardRadius)
                .border(1.0f, ui_kit::glassBorder(t))
                .transformOrigin(0.5f, 0.5f)
                .scale(pressed ? 0.96f : 1.0f)
                .transition(core::Transition::make(0.18f, core::Ease::OutBack))
                .animate(core::AnimProperty::Transform)
                .build();
            // 命中面：整卡点击开合 + 按下集合代报（卡底每帧读出 scale 目标值）
            inner.rect(mid + ".hit")
                .size(cw, cardH)
                .zIndex(1)
                .states(kTransparent, kTransparent, kTransparent)
                .onPress([this, seq = msg.seq](const core::PointerEvent&,
                                               const core::Rect&) {
                    pressedMsgSeqs_.insert(seq);
                })
                .onRelease([this, seq = msg.seq](const core::PointerEvent&,
                                                 const core::Rect&) {
                    pressedMsgSeqs_.erase(seq);
                })
                .onClick([this, seq = msg.seq] { toggleMessageExpanded(seq); })
                .build();
            inner.column(mid + ".body")
                .position(cardPad, cardPad)
                .size(contentW, contentH)
                .zIndex(2)
                .gap(kMsgGap)
                .content([&] {
                    // 头行（24）：badge → time → 弹簧 → toggle → copy（copy 仅展开态）
                    inner.row(mid + ".head")
                        .size(contentW, kMsgHeaderH)
                        .gap(8.0f)
                        .alignItems(eui::Align::CENTER)
                        .content([&] {
                            inner.stack(mid + ".badge.wrap")
                                .size(badgeW, badgeH)
                                .content([&] {
                                    inner.rect(mid + ".badge.bg")
                                        .size(badgeW, badgeH)
                                        .radius(t.sizes.radiusPill)
                                        .color(toolRole ? t.colors.primarySoftBg
                                                        : tokens.surface)
                                        .build();
                                    inner.text(mid + ".badge")
                                        .size(badgeW, badgeH)
                                        .text(badge)
                                        .fontSize(badgeFs)
                                        .lineHeight(badgeH)
                                        .fontWeight(600)
                                        .horizontalAlign(eui::HorizontalAlign::Center)
                                        .verticalAlign(eui::VerticalAlign::Center)
                                        .color(toolRole ? t.colors.primaryText
                                                        : t.colors.textSecondary)
                                        .build();
                                })
                                .build();
                            inner.text(mid + ".time")
                                .size(kMsgTimeW, badgeH)
                                .text(messageTimeText(msg.createdAt, refSec))
                                .fontSize(badgeFs)
                                .lineHeight(badgeH)
                                .wrap(false)
                                .maxWidth(kMsgTimeW)
                                .color(t.colors.textTertiary)
                                .build();
                            // 弹簧：把 chips 推到卡右缘（行无 flex，按剩余宽现算占位）
                            const int chipCount = expanded ? 2 : 1;
                            const float springW = std::max(
                                0.0f,
                                contentW - badgeW - kMsgTimeW -
                                    static_cast<float>(chipCount) * kChipW -
                                    static_cast<float>(2 + chipCount) * 8.0f);
                            inner.rect(mid + ".spring")
                                .size(springW, badgeH)
                                .color(kTransparent)
                                .build();
                            actionChip(inner, mid + ".toggle",
                                       expanded ? "收起" : "展开", kChipW,
                                       /*primary=*/false, /*enabled=*/true, tokens,
                                       [this, seq = msg.seq] {
                                           toggleMessageExpanded(seq);
                                       }, badgeFs, core::theme::scaled(24.0f));
                            if (expanded) {
                                actionChip(inner, mid + ".copy", "复制", kChipW,
                                           /*primary=*/false, /*enabled=*/true, tokens,
                                           [this, text = messagePlainText(blocks)] {
                                               // setClipboardText 返回 void 无法校验 → LOG 留痕
                                               core::window::setClipboardText(text);
                                               LOG_INFO("Agent: 已复制消息全文 " +
                                                        std::to_string(text.size()) +
                                                        " 字节到剪贴板");
                                               toast_.show("已复制",
                                                           "消息全文已复制到剪贴板");
                                           }, badgeFs, core::theme::scaled(24.0f));
                            }
                        })
                        .build();
                    if (!expanded) {
                        inner.text(mid + ".preview")
                            .size(contentW, msgLh)
                            .text(messagePreview(msg))
                            .fontSize(fsS)
                            .lineHeight(msgLh)
                            .wrap(false)
                            .maxWidth(contentW)
                            .color(t.colors.textSecondary)
                            .onClick([this, seq = msg.seq] {
                                toggleMessageExpanded(seq);
                            })
                            .build();
                        return;
                    }
                    if (blocks.empty()) {
                        inner.text(mid + ".blk.empty")
                            .size(contentW, msgLh)
                            .text("（空消息）")
                            .fontSize(fsS)
                            .lineHeight(msgLh)
                            .color(t.colors.textTertiary)
                            .build();
                        return;
                    }
                    for (size_t j = 0; j < blocks.size(); ++j) {
                        const MessageBlock& b = blocks[j];
                        const std::string bid = mid + ".blk." + std::to_string(j);
                        switch (b.type) {
                            case MessageBlock::Type::Text:
                            case MessageBlock::Type::Plain: {
                                inner.text(bid)
                                    .size(contentW,
                                          estimateTextBlockHeight(
                                              b.text, contentW, fsS, msgLh))
                                    .text(b.text)
                                    .fontSize(fsS)
                                    .lineHeight(msgLh)
                                    .wrap(true)
                                    .maxWidth(contentW)
                                    .color(b.type == MessageBlock::Type::Plain
                                               ? t.colors.textTertiary
                                               : t.colors.textPrimary)
                                    .build();
                                break;
                            }
                            case MessageBlock::Type::ToolUse: {
                                // 浅染内嵌块（§3.3）：高 28、radius 6、扳手 + 名称
                                // 深字档 + args 摘要截 120（三级字）
                                const float toolBlockH = core::theme::scaled(28.0f);
                                inner.stack(bid + ".wrap")
                                    .size(contentW, toolBlockH)
                                    .content([&] {
                                        inner.rect(bid + ".wrap.bg")
                                            .size(contentW, toolBlockH)
                                            .color(t.colors.primarySoftBg)
                                            .radius(kCardRadius)
                                            .build();
                                        const float iconX = core::theme::scaled(8.0f);
                                        const float iconW = core::theme::scaled(12.0f);
                                        inner.text(bid + ".icon")
                                            .position(iconX, 0.0f)
                                            .size(iconW, toolBlockH)
                                            .icon(::eui::utf8(0xF0AD))  // fa-wrench
                                            .fontSize(iconW)
                                            .lineHeight(toolBlockH)
                                            .verticalAlign(eui::VerticalAlign::Center)
                                            .color(t.colors.primaryText)
                                            .hitTestMode(
                                                ::core::dsl::HitTestMode::None)
                                            .build();
                                        const std::string nameLine =
                                            "工具 · " + b.toolName;
                                        const float nameW = std::min(
                                            core::TextPrimitive::measureTextWidth(
                                                nameLine, "", iconW, 600),
                                            std::max(
                                                0.0f,
                                                contentW - iconX - iconW - 16.0f));
                                        inner.text(bid)
                                            .position(iconX + iconW +
                                                          core::theme::scaled(8.0f),
                                                      0.0f)
                                            .size(nameW, toolBlockH)
                                            .text(nameLine)
                                            .fontSize(iconW)
                                            .lineHeight(toolBlockH)
                                            .fontWeight(600)
                                            .wrap(false)
                                            .maxWidth(nameW)
                                            .verticalAlign(eui::VerticalAlign::Center)
                                            .color(t.colors.primaryText)
                                            .hitTestMode(
                                                ::core::dsl::HitTestMode::None)
                                            .build();
                                        const float argsX =
                                            iconX + iconW + core::theme::scaled(8.0f) +
                                            nameW + core::theme::scaled(8.0f);
                                        inner.text(bid + ".args")
                                            .position(argsX, 0.0f)
                                            .size(std::max(0.0f, contentW - argsX),
                                                  toolBlockH)
                                            .text(toolArgsSummary(b.text, 120))
                                            .fontSize(iconW)
                                            .lineHeight(toolBlockH)
                                            .wrap(false)
                                            .maxWidth(std::max(
                                                0.0f, contentW - argsX))
                                            .verticalAlign(eui::VerticalAlign::Center)
                                            .color(t.colors.textTertiary)
                                            .hitTestMode(
                                                ::core::dsl::HitTestMode::None)
                                            .build();
                                    })
                                    .build();
                                break;
                            }
                            case MessageBlock::Type::ToolResult: {
                                // 左 3px 语义条（§3.3）：正常=中性灰边、错误=红；
                                // 红仅错误，正常输出不占用操作绿
                                const float barW = 3.0f;
                                const float textX = barW + core::theme::scaled(10.0f);
                                const float textW =
                                    std::max(0.0f, contentW - textX);
                                const float outH = estimateTextBlockHeight(
                                    b.text, textW, fsS, msgLh);
                                const float blockH = msgLh + kMsgGap + outH;
                                inner.stack(bid + ".wrap")
                                    .size(contentW, blockH)
                                    .content([&] {
                                        inner.rect(bid + ".wrap.bar")
                                            .size(barW, blockH)
                                            .color(b.isError ? t.colors.error
                                                             : t.colors.borderDefault)
                                            .build();
                                        inner.text(bid)
                                            .position(textX, 0.0f)
                                            .size(textW, msgLh)
                                            .text(b.isError ? "工具输出（错误）"
                                                            : "工具输出")
                                            .fontSize(core::theme::scaled(12.0f))
                                            .lineHeight(msgLh)
                                            .wrap(false)
                                            .maxWidth(textW)
                                            .color(b.isError ? t.colors.errorText
                                                             : t.colors.textTertiary)
                                            .hitTestMode(
                                                ::core::dsl::HitTestMode::None)
                                            .build();
                                        inner.text(bid + ".out")
                                            .position(textX, msgLh + kMsgGap)
                                            .size(textW, outH)
                                            .text(b.text)
                                            .fontSize(fsS)
                                            .lineHeight(msgLh)
                                            .wrap(true)
                                            .maxWidth(textW)
                                            .color(b.isError ? t.colors.errorText
                                                             : t.colors.textSecondary)
                                            .hitTestMode(
                                                ::core::dsl::HitTestMode::None)
                                            .build();
                                    })
                                    .build();
                                break;
                            }
                        }
                    }
                })
                .build();
        })
        .build();
}

// 任务弹窗重设计（UAT5，demo task-new-dialog-redesign.html 智能体面板）：
// 任务名称大输入 → 触发 segment 六选（demo 只画了三选，但 TaskDef/存量任务有 6 类
// 触发——砍 UI 等于改写存量任务语义，全部保留；选中 = 绿底白字）→ 状态行
//（cron+模板 / 闹钟时间 / 事件 chips，互斥）→ 指令 textarea 全宽（标签行右缘
//「✨ AI 起草」绿 chip，行为接既有草稿功能）→ 执行记忆 → 模型槽位|迭代上限|只跑
// 一次 → 安全组（破坏性开关独立红组保持 spec §8 item 5）→ 底部 取消|创建/保存。
// 只动布局不动功能：字段与 TaskDef 一一对应。弹性宽 = max(720, 窗口/2)（demo 最小档）。
// 元素 id 契约：触发 segment 沿用 item.N（原下拉项名；可点面 .item.N.bg，
// test 的 callDropdownChange 有 .bg 回落）；slot/iter/limit/destructive/
// UAT6 步骤条重设计：顶部 4 步节点条（快递详情风）+ 每步只显示该步配置 + 底部常驻按钮
//（取消/上一步/下一步(创建)/测试）。字段与数据模型零增删，纯布局重排。
void AgentModule::composeTaskDialog(
    ::eui::Ui& ui, const ::eui::Screen& screen, const ::core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& themeTokens) {
    const float w = std::max(640.0f, screen.width * 0.5f);
    const float h = kTaskDialogH;
    // 竖向导航案几何：标题栏/底栏各占上下 44 带，左导航固定 160（弹窗宽仍弹性），内容区夹于其间
    constexpr float kVertNavW = 160.0f;        // 左导航宽
    constexpr float kVertContentX = 164.0f;    // 内容区左缘 = 导航右缘 + 呼吸
    constexpr float kVertContentPad = 16.0f;   // 内容区四边内距
    constexpr float kContentY = 44.0f;         // 标题栏高 = 内容区顶 = 底栏高
    const float kContentW = w - kVertContentX - kVertContentPad * 2.0f;  // 字段有效宽 = W-196
    const float x0 = kVertContentX + kVertContentPad;                    // 字段左缘 = 180
    const bool isSchedule = editTriggerIdx_ == kTriggerScheduleIdx;
    const bool isEvent = editTriggerIdx_ == kTriggerEventIdx;
    const bool isOnce = editTriggerIdx_ == kTriggerOnceIdx;
    const bool isEdit = editingTaskId_.has_value();

    // --- 步骤定义 ---
    constexpr const char* kStepLabels[] = {"基础信息", "触发", "指令", "执行与安全"};
    constexpr int kStepCount = 4;

    // 逐步校验（W4 C1）：下一步按钮按当前步表单校验启用；出错标节点不弹全局错误
    const auto stepValidFor = [&](int i) -> bool {
        if (i == 0) return true;  // 名称可留空（自动取名）
        if (i == 1) {
            if (isSchedule) return parseCron(editCron_).isOk();
            if (isOnce) return !editOnceAtText_.empty()
                                && parseOnceAtLocalText(editOnceAtText_).isOk();
            if (isEvent) return std::any_of(editEvents_.begin(), editEvents_.end(),
                                            [](bool v) { return v; });
            return true;  // manual / resident 等无需配置
        }
        if (i == 2) return !editGoal_.empty();
        return true;  // step 3 执行与安全
    };

    // 试运行结果收割：dryRun_ done 后才 join + 显结果（线程安全：只用原子 done 作同步点）
    if (dryRun_ && dryRun_->done.load(std::memory_order_acquire)) {
        if (dryRun_->thread.joinable()) dryRun_->thread.join();
        // run 结局从最后一次 agent_run 行判定（completed/failed/limited/cancelled）
        testRunResult_ = "已完成试运行，查看执行记录页确认结果";
        dryRun_.reset();
    }

    // 触发提示文案（仅 step 1 用；与旧版同源）
    std::string triggerHint;
    if (isSchedule) {
        triggerHint = cronPreview_.empty() ? kTriggerHints[kTriggerScheduleIdx] : "";
    } else if (isOnce) {
        triggerHint = oncePreview_.empty() ? kTriggerHints[kTriggerOnceIdx] : "";
    } else if (editTriggerIdx_ >= 0 && editTriggerIdx_ < kTriggerCount) {
        triggerHint = kTriggerHints[editTriggerIdx_];
    }

    components::dialog(ui, "agent.dialog.task")
        .open(taskDialogOpen_)
        .screen(screen.width, screen.height)
        .size(w, h)
        .theme(themeTokens)
        .onOpenChange([this](bool v) {
            if (!v) closeTaskDialog();
        })
        .content([&] {
            if (!taskDialogOpen_) return;
            // 标题栏独立段 (0,0) W×44：标题带内垂直居中（行高 28），底部 1px 分隔线
            ui_kit::drawDialogTitle(ui, "agent.dialog.task.title", w,
                                    isEdit ? "编辑任务" : "新建智能任务",
                                    ui_kit::kDialogTitleFontSize, t.colors.textPrimary, {.y = 8.0f});
            ui.rect("agent.dialog.task.titlebar.edge")
                .position(0.0f, kContentY - 1.0f)
                .size(w, 1.0f)
                .color(t.colors.borderDefault)
                .build();

            // ✕ 关闭（带内垂直居中）
            ui.text("agent.dialog.task.close")
                .position(w - 56.0f, 8.0f)
                .size(core::theme::scaled(28.0f), core::theme::scaled(28.0f))
                .text("✕")
                .fontSize(core::theme::scaled(14.0f))
                .lineHeight(core::theme::scaled(28.0f))
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textTertiary)
                .onClick([this] { closeTaskDialog(); })
                .build();

            // ====== 左侧竖向导航 (0,44) 160×(H-44)：nav-item 高 56，序号圆 r=11 + 标签/副标竖排 ======
            // 四态：done=#E6FBF2 底+#36D399 字+✓ / cur=#36D399 填充+白字+左3px绿条
            //       cur-err=#EF4444 10% 底+errorText 红字+!+左3px红条 / todo=hover 底+textSecondary 字
            {
                // 侧栏底（#FAFAF9）+ 右缘 1px 边线
                ui.rect("agent.dialog.task.nav")
                    .position(0.0f, kContentY)
                    .size(kVertNavW, h - kContentY)
                    .color(t.colors.background)
                    .build();
                ui.rect("agent.dialog.task.nav.edge")
                    .position(kVertNavW, kContentY)
                    .size(1.0f, h - kContentY)
                    .color(t.colors.borderDefault)
                    .build();

                constexpr float kVertNavItemH = 56.0f;
                // bug154③：节点条间原 0 间隙贴死成整条色带——上下 12 / 相邻 8
                // （theme spacing token），节点块独立可辨
                constexpr float kVertNavTopPad = 12.0f;
                constexpr float kVertNavGap = 8.0f;
                constexpr float kVertNumD = 22.0f;   // 序号圆直径（r=11）
                constexpr const char* kStepSubs[] = {
                    "任务名称", "六类 + cron", "目标 / 系统提示词", "模型 / 限次 / 安全组"};
                for (int i = 0; i < kStepCount; ++i) {
                    std::string nodeId = "agent.dialog.task.step." + std::to_string(i);
                    const float navY = kContentY + kVertNavTopPad +
                                       static_cast<float>(i) *
                                           (kVertNavItemH + kVertNavGap);
                    // 当前步校验失败 → 节点标红（#EF4444 10% 底 + errorText 红字 + !）
                    const bool stepInvalid = (i == currentStep_) && !stepValidFor(i);
                    const bool done = i < currentStep_;
                    const bool cur = i == currentStep_;
                    const auto itemBg = done ? t.colors.primarySoftBg
                        : stepInvalid ? eui::mixColor(t.colors.background, t.colors.error, 0.10f)
                        : cur ? t.colors.primary : t.colors.hover;
                    const auto itemFg = done ? t.colors.primary
                        : stepInvalid ? t.colors.errorText
                        : cur ? t.colors.onPrimary : t.colors.textSecondary;
                    // 可点面（可点元素 id 本身带 onClick，测试锚 <nodeId>.bg）
                    ui.rect(nodeId + ".bg")
                        .position(0.0f, navY)
                        .size(kVertNavW, kVertNavItemH)
                        .color(itemBg)
                        .onClick([this, i] { currentStep_ = i; })
                        .build();
                    // cur / cur-err 左 3px 态条
                    if (cur) {
                        ui.rect(nodeId + ".bar")
                            .position(0.0f, navY)
                            .size(3.0f, kVertNavItemH)
                            .color(stepInvalid ? t.colors.error : t.colors.primary)
                            .build();
                    }
                    // 序号圆（done=✓ / cur-err=!，其余显序号）
                    const float numY = navY + (kVertNavItemH - kVertNumD) * 0.5f;
                    ui.rect(nodeId + ".num")
                        .position(14.0f, numY)
                        .size(kVertNumD, kVertNumD)
                        .radius(kVertNumD * 0.5f)
                        .color(done ? t.colors.primarySoftBg
                            : stepInvalid ? t.colors.error
                            : cur ? t.colors.primary : t.colors.hover)
                        .build();
                    ui.text(nodeId + ".num.text")
                        .position(14.0f, numY)
                        .size(kVertNumD, kVertNumD)
                        .text(done ? "✓" : stepInvalid ? "!" : std::to_string(i + 1))
                        .fontSize(core::theme::scaled(12.0f))
                        .lineHeight(kVertNumD)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(done ? t.colors.primary
                            : (stepInvalid || cur) ? t.colors.onPrimary
                                                   : t.colors.textSecondary)
                        .build();
                    // 标签 + 副标竖排（圆右 8px，块在 56 高内垂直居中）
                    ui.text(nodeId + ".label")
                        .position(44.0f, navY + 12.0f)
                        .size(kVertNavW - 44.0f - 14.0f, core::theme::scaled(18.0f))
                        .text(kStepLabels[i])
                        .fontSize(core::theme::scaled(14.0f))
                        .lineHeight(core::theme::scaled(18.0f))
                        .fontWeight(600)
                        .wrap(false)
                        .color(itemFg)
                        .build();
                    ui.text(nodeId + ".sub")
                        .position(44.0f, navY + 30.0f)
                        .size(kVertNavW - 44.0f - 14.0f, core::theme::scaled(14.0f))
                        .text(kStepSubs[i])
                        .fontSize(core::theme::scaled(12.0f))
                        .lineHeight(core::theme::scaled(14.0f))
                        .wrap(false)
                        // 白字只压 valid 当前步绿底；无效态粉底仍走 textSecondary
                        //（白字压粉底仅 1.18:1；label 走 errorText 为 5.46:1）
                        .color((cur && !stepInvalid) ? t.colors.onPrimary
                                                     : t.colors.textSecondary)
                        .build();
                }
            }

            // ====== 内容区（按 currentStep_ 切显） ======
            {
                // --- Step 0: 基础信息（名称） ---
                if (currentStep_ == 0) {
                    fieldCaption(ui, "agent.dialog.task.name.caption", "任务名称",
                                 x0, kContentY + kVertContentPad, kDialogLabelW, 24.0f,
                                 t.colors.textSecondary);
                    components::input(ui, "agent.dialog.task.name")
                        .position(x0, kContentY + kVertContentPad + 28.0f)
                        .size(kContentW, 40.0f)
                        .value(editName_)
                        .placeholder("给任务起个名字，留空则按任务目标自动取名")
                        .fontSize(t.sizes.fontSizeBody)
                        .theme(themeTokens)
                        .onChange([this](const std::string& v) { editName_ = v; })
                        .onCancel([this] { closeTaskDialog(); })
                        .build();

                    // 出厂模板（仅新建态渲染）：点击一键套用名称/触发类型/cron/提示词，
                    // 之后各步仍可自由改；编辑态不渲染——套用是「从零起稿」动作，
                    // 在既有任务上整体覆盖字段只会毁配置
                    if (!isEdit) {
                        constexpr float kTplCaptionY =
                            kContentY + kVertContentPad + 28.0f + 40.0f + 24.0f;
                        fieldCaption(ui, "agent.dialog.task.tpl.caption",
                                     "出厂模板（点击套用，可再修改）", x0,
                                     kTplCaptionY, kContentW, 16.0f,
                                     t.colors.textSecondary);
                        // bug179①：模板按钮改流式排布——原等分单行在模板数增长后
                        // 每格 <30px，4 字文本全部互相叠写成一片涂鸦。按文字实测
                        // 宽给按钮宽，flow 自动换行，间距用 spacing token 8。
                        constexpr float kTplGap = 8.0f;
                        ui.flow("agent.dialog.task.tpl")
                            .position(x0, kTplCaptionY + 24.0f)
                            .width(kContentW)
                            .gap(kTplGap)
                            .lineGap(kTplGap)
                            .content([&] {
                                for (int i = 0; i < kTaskTemplateCount; ++i) {
                                    const float itemW =
                                        core::TextPrimitive::measureTextWidth(
                                            kTaskTemplates[i].name, "",
                                            core::theme::scaled(12.0f), 500) +
                                        core::theme::scaled(24.0f);
                                    components::button(
                                        ui, "agent.dialog.task.tpl.item." +
                                                std::to_string(i))
                                        .size(itemW, 32.0f)
                                        .text(kTaskTemplates[i].name)
                                        .fontSize(core::theme::scaled(12.0f))
                                        .colors(t.colors.background, t.colors.hover,
                                                t.colors.primarySoftBg)
                                        .textColor(t.colors.textPrimary)
                                        .radius(t.sizes.radius)
                                        .border(1.0f, t.colors.borderDefault)
                                        .onClick([this, i] { applyTaskTemplate(i); })
                                        .build();
                                }
                            })
                            .build();
                    }
                }

                // --- Step 1: 触发 ---
                if (currentStep_ == 1) {
                    // bug179②：原垂直居中在 432 高内容区里给两行控件上方留出
                    // ~140px 大空白——表单语义应顶格上对齐，余白自然沉底
                    const float s1y = kContentY + kVertContentPad;
                    // 触发 segment（六类触发平分整行；保持旧 id）
                    fieldCaption(ui, "agent.dialog.task.trigger.caption", "触发方式",
                                 x0, s1y, kDialogLabelW, 16.0f,
                                 t.colors.textSecondary);
                    {
                        constexpr float kGap = 6.0f;
                        const float opW = (kContentW - kGap * (kTriggerCount - 1)) /
                                         static_cast<float>(kTriggerCount);
                        ui.stack("agent.dialog.task.trigger")
                            .position(x0, s1y + 20.0f)
                            .size(kContentW, 32.0f)
                            .content([&] {
                                for (int i = 0; i < kTriggerCount; ++i) {
                                    const bool on = editTriggerIdx_ == i;
                                    components::button(
                                        ui, "agent.dialog.task.trigger.item." +
                                                std::to_string(i))
                                        .position(static_cast<float>(i) * (opW + kGap),
                                                   0.0f)
                                        .size(opW, 32.0f)
                                        .text(kTriggerLabels[i])
                                        .fontSize(core::theme::scaled(12.0f))
                                        .colors(on ? t.colors.primary
                                                   : t.colors.background,
                                                on ? t.colors.primaryHover
                                                   : t.colors.hover,
                                                on ? t.colors.primaryPressed
                                                   : t.colors.primarySoftBg)
                                        .textColor(on ? t.colors.onPrimary
                                                      : t.colors.textPrimary)
                                        .radius(t.sizes.radius)
                                        .border(1.0f,
                                                on ? t.colors.primary
                                                   : t.colors.borderDefault)
                                        .onClick([this, i] {
                                            editTriggerIdx_ = i;
                                            if (i == kTriggerOnceIdx) refreshOncePreview();
                                            else if (i == kTriggerScheduleIdx) refreshCronPreview();
                                        })
                                        .build();
                                }
                            })
                            .build();
                    }

                    // 触发配置行（cron / 闹钟 / 事件 chips）
                    const float cfgY = s1y + 60.0f;
                    if (isSchedule) {
                        fieldCaption(ui, "agent.dialog.task.cron.caption", "cron 表达式",
                                     x0, cfgY, kDialogLabelW, kDialogRowH,
                                     t.colors.textSecondary);
                        const float cronX = x0 + kDialogLabelW;
                        const float tplW = 110.0f;
                        const float cronW = kContentW - kDialogLabelW - tplW - kDialogGap;
                        components::input(ui, "agent.dialog.task.cron")
                            .position(cronX, cfgY)
                            .size(cronW, kDialogRowH)
                            .value(editCron_)
                            .placeholder("分 时 日 月 周，例如 0 9 * * 1-5")
                            .fontSize(t.sizes.fontSizeBody)
                            .theme(themeTokens)
                            .onChange([this](const std::string& v) { setEditCronDraft(v); })
                            .onCancel([this] { closeTaskDialog(); })
                            .build();
                        ui.stack("agent.dialog.task.cron.template.wrap")
                            .position(cronX + cronW + kDialogGap, cfgY)
                            .size(tplW, kDialogRowH)
                            .content([&] {
                                std::vector<std::string> items;
                                items.reserve(kCronTemplateCount);
                                for (int i = 0; i < kCronTemplateCount; ++i)
                                    items.emplace_back(kCronTemplates[i].label);
                                components::dropdown(ui, "agent.dialog.task.cron.template")
                                    .size(tplW, kDialogRowH)
                                    .theme(themeTokens)
                                    .items(std::move(items))
                                    .placeholder("选择模板…")
                                    .selected(editCronTemplateIdx_)
                                    .open(editCronTemplateOpen_)
                                    .onOpenChange([this](bool v) { editCronTemplateOpen_ = v; })
                                    .onChange([this](int idx) {
                                        if (idx >= 0 && idx < kCronTemplateCount) {
                                            setEditCronDraft(kCronTemplates[idx].expr);
                                            editCronTemplateIdx_ = idx;
                                        }
                                    })
                                    .build();
                            })
                            .build();
                    }
                    if (isOnce) {
                        fieldCaption(ui, "agent.dialog.task.once.caption", "闹钟时间",
                                     x0, cfgY, kDialogLabelW, kDialogRowH,
                                     t.colors.textSecondary);
                        components::input(ui, "agent.dialog.task.once")
                            .position(x0 + kDialogLabelW, cfgY)
                            .size(kContentW - kDialogLabelW, kDialogRowH)
                            .value(editOnceAtText_)
                            .placeholder("YYYY-MM-DD HH:MM（本地时间，24 小时制）")
                            .fontSize(t.sizes.fontSizeBody)
                            .theme(themeTokens)
                            .onChange([this](const std::string& v) { setEditOnceDraft(v); })
                            .onCancel([this] { closeTaskDialog(); })
                            .build();
                    }
                    if (isEvent) {
                        fieldCaption(ui, "agent.dialog.task.event.caption", "监听事件",
                                     x0, cfgY, kDialogLabelW, kDialogRowH,
                                     t.colors.textSecondary);
                        const float evX = x0 + kDialogLabelW;
                        const float evW = (kContentW - kDialogLabelW - kDialogGap * 2.0f) /
                                          static_cast<float>(kEventCount);
                        for (int i = 0; i < kEventCount; ++i) {
                            const bool on = editEvents_[i];
                            components::button(
                                ui, "agent.dialog.task.event." + std::to_string(i))
                                .position(evX + static_cast<float>(i) * (evW + kDialogGap),
                                          cfgY)
                                .size(evW, 32.0f)
                                .text(kEventLabels[i])
                                .fontSize(core::theme::scaled(12.0f))
                                .colors(on ? eui::mixColor(t.colors.surface,
                                                           t.colors.primary, 0.12f)
                                           : t.colors.background,
                                        t.colors.hover,
                                        t.colors.primarySoftBg)
                                .textColor(on ? t.colors.primaryText
                                              : t.colors.textSecondary)
                                .radius(t.sizes.radius)
                                .border(1.0f,
                                        on ? eui::mixColor(t.colors.surface,
                                                          t.colors.primary, 0.24f)
                                           : t.colors.borderDefault)
                                .onClick([this, i] { editEvents_[i] = !editEvents_[i]; })
                                .build();
                        }
                    }

                    // 触发提示 / 预览行
                    const float hintY = cfgY + 44.0f;
                    ui_kit::drawDialogText(ui, "agent.dialog.task.trigger.hint", w,
                                           triggerHint, t.sizes.fontSizeSmall,
                                           t.colors.textTertiary,
                                           {.x = x0, .y = hintY, .width = kContentW, .height = 12.0f,
                                            .lineHeight = 12.0f, .skipIfEmpty = true});
                    ui_kit::drawDialogText(ui, "agent.dialog.task.cron.preview", w,
                                           cronPreview_,
                                           t.sizes.fontSizeSmall,
                                           cronPreviewIsError_ ? t.colors.errorText
                                                               : t.colors.textTertiary,
                                           {.x = x0, .y = hintY, .width = kContentW, .height = 12.0f,
                                            .lineHeight = 12.0f, .skipIfEmpty = true});
                    ui_kit::drawDialogText(ui, "agent.dialog.task.once.preview", w,
                                           oncePreview_,
                                           t.sizes.fontSizeSmall,
                                           oncePreviewIsError_ ? t.colors.errorText
                                                               : t.colors.textTertiary,
                                           {.x = x0, .y = hintY, .width = kContentW, .height = 12.0f,
                                            .lineHeight = 12.0f, .skipIfEmpty = true});
                }

                // --- Step 2: 指令 ---
                if (currentStep_ == 2) {
                    const float s2y = kContentY + kVertContentPad;
                    // 指令标签行 + AI 起草 Outline 次按钮（白色底 + 主色边框，W4 C1）
                    fieldCaption(ui, "agent.dialog.task.goal.caption",
                                 "指令（发给 AI 的 prompt）", x0, s2y,
                                 kContentW - 116.0f, 24.0f, t.colors.textSecondary);
                    {
                        const float chipW = 96.0f;
                        const float chipH = 32.0f;
                        components::button(ui, "agent.dialog.task.aichip")
                            .position(w - kVertContentPad - chipW, s2y)
                            .size(chipW, chipH)
                            .text("✎ AI 起草")
                            .fontSize(core::theme::scaled(13.0f))
                            .colors(t.colors.background,
                                    t.colors.primarySoftBg,
                                    t.colors.primarySoftBg)
                            .textColor(t.colors.primary)
                            .radius(t.sizes.radius)
                            .border(1.0f, t.colors.primary)
                            .onClick([this] { openDraftDialog(); })
                            .build();
                    }
                    // 指令 textarea：标题行 chip 高 32（底=s2y+32），留 8px 呼吸不压输入框
                    const float goalInY = s2y + 40.0f;
                    constexpr float kStepGoalH = 96.0f;
                    components::input(ui, "agent.dialog.task.goal")
                        .position(x0, goalInY)
                        .size(kContentW, kStepGoalH)
                        .value(editGoal_)
                        .placeholder("描述这个任务要做什么，例如：把逾期的待办全部完成")
                        .fontSize(t.sizes.fontSizeBody)
                        .multiline(true)
                        .scrollbar(true)
                        .theme(themeTokens)
                        .onChange([this](const std::string& v) { editGoal_ = v; })
                        .onCancel([this] { closeTaskDialog(); })
                        .build();
                    ui_kit::drawDialogText(
                        ui, "agent.dialog.task.goal.valid", w,
                        editGoal_.empty() ? "请填写任务目标" : "",
                        t.sizes.fontSizeSmall, t.colors.errorText,
                        {.x = x0, .y = goalInY + kStepGoalH + 4.0f, .height = 14.0f,
                         .lineHeight = 14.0f, .skipIfEmpty = true});

                    // 执行记忆已并入系统提示词（W4 C3）：只保留 systemPrompt 单字段
                    // 系统提示词
                    const float sysY = goalInY + kStepGoalH + 24.0f;
                    fieldCaption(ui, "agent.dialog.task.sysprompt.caption",
                                 "系统提示词（发给 AI 的环境上下文，可编辑）",
                                 x0, sysY, kContentW, 16.0f, t.colors.textSecondary);
                    // 弹性填充到内容区底：底栏带顶 = h − kContentY（底栏按钮起始 Y）；
                    // 内容区下沿留 12px，再避让锚在底栏顶上方 18px 的试运行结果行 → 共让 24px
                    const float kStepSysMinH =
                        (h - kContentY - 24.0f) - (sysY + 20.0f);
                    components::input(ui, "agent.dialog.task.sysprompt")
                        .position(x0, sysY + 20.0f)
                        .size(kContentW, kStepSysMinH)
                        .value(editSystemPrompt_)
                        .placeholder("系统提示词，运行时注入；留空则使用自动生成的默认提示词")
                        .fontSize(t.sizes.fontSizeBody)
                        .multiline(true)
                        .scrollbar(true)
                        .theme(themeTokens)
                        .onChange([this](const std::string& v) { editSystemPrompt_ = v; })
                        .onCancel([this] { closeTaskDialog(); })
                        .build();
                }

                // --- Step 3: 执行与安全 ---
                if (currentStep_ == 3) {
                    const float s3y = kContentY + kVertContentPad;
                    // 模型槽位（左）| 迭代上限（中）| 只跑一次（右）
                    const float slotDdW = 116.0f;
                    fieldCaption(ui, "agent.dialog.task.slot.caption", "模型槽位",
                                 x0, s3y, kDialogLabelW, kDialogRowH,
                                 t.colors.textSecondary);
                    ui.stack("agent.dialog.task.slot.wrap")
                        .position(x0 + kDialogLabelW + kDialogGap, s3y)
                        .size(slotDdW, kDialogRowH)
                        .content([&] {
                            components::dropdown(ui, "agent.dialog.task.slot")
                                .size(slotDdW, kDialogRowH)
                                .theme(themeTokens)
                                .items(std::vector<std::string>(
                                    kModelSlotLabels, kModelSlotLabels + kModelSlotCount))
                                .selected(slotIndexOf(editSlot_))
                                .open(editSlotOpen_)
                                .onOpenChange([this](bool v) { editSlotOpen_ = v; })
                                .onChange([this](int idx) {
                                    if (idx >= 0 && idx < kModelSlotCount)
                                        editSlot_ = kModelSlotKeys[idx];
                                })
                                .build();
                        })
                        .build();
                    {
                        const float iterLabelX = x0 + kDialogLabelW + kDialogGap +
                                                 slotDdW + kDialogGap;
                        fieldCaption(ui, "agent.dialog.task.iter.caption", "迭代上限",
                                     iterLabelX, s3y, kDialogLabelW, kDialogRowH,
                                     t.colors.textSecondary);
                        components::input(ui, "agent.dialog.task.iter")
                            .position(iterLabelX + kDialogLabelW + kDialogGap, s3y)
                            .size(80.0f, kDialogRowH)
                            .value(std::to_string(editIter_))
                            .fontSize(t.sizes.fontSizeBody)
                            .theme(themeTokens)
                            .onChange([this](const std::string& v) {
                                editIter_ = parseIterInput(v);
                            })
                            .onCancel([this] { closeTaskDialog(); })
                            .build();
                        const bool supportsRunLimit = isSchedule || isEvent;
                        if (supportsRunLimit) {
                            const float limitX = iterLabelX + kDialogLabelW +
                                                 kDialogGap + 80.0f + kDialogGap;
                            components::toggleSwitch(ui, "agent.dialog.task.limit")
                                .position(limitX, s3y)
                                .size(kContentW - (limitX - x0), 32.0f)
                                .text(runLimitText(editMaxRuns_ > 0 ? editMaxRuns_ : 1))
                                .checked(editMaxRuns_ > 0)
                                .fontSize(core::theme::scaled(12.0f))
                                .theme(themeTokens)
                                .onChange([this](bool v) {
                                    if (!v) editMaxRuns_ = 0;
                                    else if (editMaxRuns_ < 1) editMaxRuns_ = 1;
                                })
                                .build();
                        }
                    }

                    // meta 提示（保持 "受限" "高危" 关键词）
                    const float hintY = s3y + 52.0f;
                    // bug154④：drawDialogText 宽度缺省 = w-48（自对话框左缘起算），
                    // 而本行 x=x0=180 → 文本右缘冲出弹窗 132px、且与「安全」组标题
                    // 竖向重叠。显式给 kContentW + wrap 两行盒，组标题顺延让位。
                    ui_kit::drawDialogText(
                        ui, "agent.dialog.task.meta.hint", w,
                        "迭代＝单次运行的最多轮数，跑满以「受限」结束、不再重试；"
                        "破坏性操作＝允许删除文件等高危工具，不勾则被拒",
                        t.sizes.fontSizeSmall, t.colors.textTertiary,
                        {.x = x0, .y = hintY, .width = kContentW, .height = 30.0f,
                         .lineHeight = 15.0f, .wrap = true});

                    // 安全组
                    const float safeY = hintY + 36.0f;
                    ui.text("agent.dialog.task.group.danger")
                        .position(x0, safeY)
                        .size(kContentW, kGroupTitleH)
                        .text("安全")
                        .fontSize(core::theme::scaled(14.0f))
                        .lineHeight(kGroupTitleH)
                        .fontWeight(700)
                        .color(t.colors.textPrimary)
                        .build();
                    {
                        const float destructiveY = safeY + kGroupTitleH + 4.0f;
                        if (editDestructive_) {
                            ui.text("agent.dialog.task.destructive.warn")
                                .position(x0, destructiveY)
                                .size(20.0f, core::theme::scaled(32.0f))
                                .text("⚠")
                                .fontSize(core::theme::scaled(14.0f))
                                .lineHeight(core::theme::scaled(32.0f))
                                .verticalAlign(eui::VerticalAlign::Center)
                                .color(t.colors.errorText)
                                .build();
                        }
                        ui.text("agent.dialog.task.destructive.label")
                            .position(x0 + (editDestructive_ ? 24.0f : 0.0f), destructiveY)
                            .size(160.0f, core::theme::scaled(32.0f))
                            .text("允许破坏性操作")
                            .fontSize(core::theme::scaled(12.0f))
                            .lineHeight(core::theme::scaled(32.0f))
                            .wrap(false)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .color(editDestructive_ ? t.colors.errorText
                                                     : t.colors.textSecondary)
                            .build();
                        ui_kit::wswitch(ui, "agent.dialog.task.destructive",
                                        editDestructive_,
                                        x0 + 200.0f, destructiveY + 4.0f, 44.0f, 24.0f, t,
                                        [this](bool v) { editDestructive_ = v; });
                        // bug179⑤：「允许破坏性操作」语义不清——开关旁补说明行
                        // （破坏性指什么、默认行为如何），只加文案不改行为
                        ui_kit::drawDialogText(
                            ui, "agent.dialog.task.destructive.sub", w,
                            "勾选后允许任务执行删除文件等高危工具；默认拒绝并留痕",
                            t.sizes.fontSizeSmall, t.colors.textTertiary,
                            {.x = x0, .y = destructiveY + 36.0f, .width = kContentW,
                             .height = 14.0f, .lineHeight = 14.0f});
                        // 自动记录开关已移除（W4 C3）：行为固定为开启，字段五处清链
                    }
                }
            }

            // ====== 试运行结果行（W4 C1：移除去全局错误行，出错标节点不弹全局错误） ======
            // 锚内容区底缘内侧（底栏上方 18px），新竖向案内容区与底栏间无过渡带
            ui_kit::drawDialogText(
                ui, "agent.dialog.task.test.result", w, testRunResult_,
                t.sizes.fontSizeSmall, t.colors.success,
                {.x = x0, .y = h - kContentY - 18.0f, .width = kContentW, .height = 14.0f,
                 .lineHeight = 14.0f, .skipIfEmpty = true});

            // ====== 底部按钮区（W4 C1 重做：40px 高、8px 间距、居中文字；按钮顺序从右到左：下一步 Filled 最右 → 上一步 Outline → 测试 Outline → 取消 Ghost 最左） ======
            const float btnY = h - kContentY + 6.0f;  // 底栏带高 44：32 钮上下各留 6
            const float btnH = 32.0f;  // bug179④：40 过大，收到与行控件一致的 32
            const float bw = 88.0f;
            const float testW = 104.0f;
            const float kBtnGap = 8.0f;
            const float rightEdge = w - kVertContentPad;   // 与内容区共用右留 16px

            // 从右到左逐按钮排位，cursor 追随后退
            float xCursor = rightEdge;

            // 下一步/创建/保存（Filled，最右）
            const bool isLastStep = currentStep_ == kStepCount - 1;
            const bool canNext = stepValidFor(currentStep_);
            const char* nextLabel = isLastStep ? "保存" : "下一步";  // 末步定死「保存」，不区分编辑/新建态
            const float saveX = xCursor - bw;
            components::button(ui, "agent.dialog.task.save")
                .position(saveX, btnY)
                .size(bw, btnH)
                .text(nextLabel)
                .fontSize(core::theme::scaled(14.0f))
                .colors(canNext ? t.colors.primary
                                : t.colors.background,
                        canNext ? t.colors.primaryHover
                                : t.colors.hover,
                        canNext ? t.colors.primaryPressed
                                : t.colors.primarySoftBg)
                .textColor(canNext ? t.colors.onPrimary
                                   : t.colors.textTertiary)
                .radius(t.sizes.radius)
                .border(1.0f, canNext ? t.colors.primary
                                      : t.colors.borderDefault)
                .onClick(canNext ? std::function<void()>{[this, isLastStep] {
                    if (isLastStep) {
                        saveTaskDialog();
                    } else {
                        currentStep_ = std::min(kStepCount - 1, currentStep_ + 1);
                    }
                }} : std::function<void()>{})
                .build();
            xCursor -= bw + kBtnGap;

            // 上一步（Outline）—— 非首步才渲染
            if (currentStep_ > 0) {
                const float prevX = xCursor - bw;
                components::button(ui, "agent.dialog.task.prev")
                    .position(prevX, btnY)
                    .size(bw, btnH)
                    .text("上一步")
                    .fontSize(core::theme::scaled(14.0f))
                    .colors(t.colors.background,
                            t.colors.hover,
                            t.colors.primarySoftBg)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] { currentStep_ = std::max(0, currentStep_ - 1); })
                    .build();
                xCursor -= bw + kBtnGap;
            }

            // 测试（Outline）—— 仅最后一步渲染
            if (isLastStep) {
                const bool testDisabled = !canNext || dryRun_ != nullptr;
                const std::string testLabel = dryRun_ ? "测试中…" : "测试试运行";
                const float testX = xCursor - testW;
                components::button(ui, "agent.dialog.task.test")
                    .position(testX, btnY)
                    .size(testW, btnH)
                    .text(testLabel)
                    .fontSize(core::theme::scaled(14.0f))
                    .colors(testDisabled ? t.colors.background
                                         : t.colors.background,
                            testDisabled ? t.colors.hover
                                         : t.colors.primary,
                            testDisabled ? t.colors.primarySoftBg
                                         : t.colors.primarySoftBg)
                    .textColor(testDisabled ? t.colors.textTertiary
                                            : t.colors.primary)
                    .radius(t.sizes.radius)
                    .border(1.0f, testDisabled ? t.colors.borderDefault
                                               : t.colors.primary)
                    .onClick(testDisabled ? std::function<void()>{}
                                         : std::function<void()>{[this] { testRunDialog(); }})
                    .build();
                xCursor -= testW + kBtnGap;
            }

            // 取消（Ghost，最左）
            const float cancelX = xCursor - bw;
            components::button(ui, "agent.dialog.task.cancel")
                .position(cancelX, btnY)
                .size(bw, btnH)
                .text("取消")
                .fontSize(core::theme::scaled(14.0f))
                .colors(t.colors.background,
                        t.colors.primarySoftBg,
                        t.colors.primarySoftBg)
                .textColor(t.colors.accentText)
                .radius(t.sizes.radius)
                .border(0.0f, t.colors.borderDefault)
                .onClick([this] { closeTaskDialog(); })
                .build();
        })
        .build();
}

// ---- 池170 v2 工具弹窗 TU 私有助手 ----

constexpr float kToolsHeaderH = 56.0f;           // §3.3 头部区
constexpr float kToolsSearchBoxH = 36.0f;        // §3.6 搜索框高
constexpr float kToolsDescLh = 20.0f;            // 描述行盒（13px/20）
constexpr std::size_t kToolsDescClampLines = 3;  // 描述 >3 行才 clamp+展开（§3.4）

// UTF-8 单码点字节长（首字节口径；坏字节按 1 兜底，只影响一处断行）
std::size_t utf8CodepointLen(const std::string& s, std::size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 0xF0) return 4;
    if (c >= 0xE0) return 3;
    if (c >= 0xC0) return 2;
    return 1;
}

// 按码点贪心换行：与渲染同引擎测量（measureTextWidth，内部带缓存），CJK 逐字断行。
// 返回各行原文切片；渲染逐行走 wrap(false) 文本节点，与测量天然一致。
std::vector<std::string> wrapToolDescLines(const std::string& text, float maxWidth,
                                           float fontSize) {
    std::vector<std::string> lines;
    if (text.empty()) return lines;
    std::string line;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t len = utf8CodepointLen(text, i);
        const std::string cp =
            text.substr(i, std::min(len, text.size() - i));
        if (!line.empty() &&
            core::TextPrimitive::measureTextWidth(line + cp, "", fontSize,
                                                  400) > maxWidth) {
            lines.push_back(std::move(line));
            line.clear();
        }
        line += cp;
        i += len;
    }
    if (!line.empty()) lines.push_back(std::move(line));
    return lines;
}

// 单行末尾塞省略号：整行（含 …）不超 maxWidth 的最长截断（同 message_blocks 口径）
std::string ellipsizeToolLine(const std::string& line, float maxWidth,
                              float fontSize) {
    if (core::TextPrimitive::measureTextWidth(line + "…", "", fontSize, 400) <=
        maxWidth) {
        return line + "…";
    }
    std::string cut = line;
    while (!cut.empty()) {
        while (!cut.empty() &&
               (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80) {
            cut.pop_back();  // 丢 UTF-8 续字节
        }
        if (!cut.empty()) cut.pop_back();  // 丢首字节
        if (core::TextPrimitive::measureTextWidth(cut + "…", "", fontSize,
                                                  400) <= maxWidth) {
            return cut + "…";
        }
    }
    return "…";
}

// 命中区间（字节偏移，升序不重叠；haystack 已小写化，needle 非空）
std::vector<std::pair<std::size_t, std::size_t>> findMatchRanges(
    const std::string& lowerHay, const std::string& needle) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    if (needle.empty()) return out;
    std::size_t pos = 0;
    while ((pos = lowerHay.find(needle, pos)) != std::string::npos) {
        out.emplace_back(pos, pos + needle.size());
        pos += needle.size();
    }
    return out;
}

// 过滤口径（§3.6）：名称 + 说明全文 + 参数名，不区分大小写子串，命中任一即保留整卡
bool toolMatchesQuery(const ToolInfo& tool, const std::string& q) {
    if (q.empty()) return true;
    if (core::str::toLowerAscii(tool.name).find(q) != std::string::npos) {
        return true;
    }
    if (core::str::toLowerAscii(tool.description).find(q) !=
        std::string::npos) {
        return true;
    }
    for (const auto& p : tool.params) {
        if (core::str::toLowerAscii(p).find(q) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// 族图标映射（§3.4，全 FA5 实心集码点禁 emoji；前缀族，兜底 plug）
unsigned int toolFamilyIconGlyph(const std::string& name) {
    const auto startsWith = [&name](const char* p) {
        return name.rfind(p, 0) == 0;
    };
    if (startsWith("fs.")) return 0xF07B;        // folder
    if (startsWith("git.")) return 0xF126;       // code-branch
    if (startsWith("md.")) return 0xF15C;        // file-alt
    if (startsWith("memory.")) return 0xF5DC;    // brain
    if (startsWith("net.")) return 0xF0AC;       // globe
    if (startsWith("app.")) return 0xF1B2;       // cube
    if (startsWith("claude.")) return 0xF544;    // robot
    if (startsWith("todo.")) return 0xF0AE;      // tasks
    if (startsWith("schedule.")) return 0xF017;  // clock
    if (startsWith("bash.") || startsWith("agent.")) return 0xF120;  // terminal
    return 0xF1E6;                               // plug 兜底
}

// 次钮底档（§9：light #F2F2F2 / dark #353231）。light 无该 token 实值，取最接近
// 的 borderMuted #F3F4F6（Δ1/255，禁新增裸 hex）；dark 即 hover 档 #353231（精确）
eui::Color toolNeutralFill(const ::core::theme::Tokens& t) {
    return t.isDark ? t.colors.hover : t.colors.borderMuted;
}

// 弹窗浮层细黑边（§9：light rgba(0,0,0,0.35) / dark borderDefault——暗色纯黑边
// 深底不可见降档，终端页覆盖卡同口径）
eui::Color toolsShellBorder(const ::core::theme::Tokens& t) {
    return t.isDark ? t.colors.borderDefault
                    : eui::Color{0.0f, 0.0f, 0.0f, 0.35f};
}

// 工具卡极淡阴影（§3.4：light rgba(16,54,38,0.05) blur8 offsetY2 / dark 无）
core::Shadow toolCardShadow(const ::core::theme::Tokens& t) {
    core::Shadow s;
    s.enabled = !t.isDark;
    s.offset = {0.0f, 2.0f};
    s.blur = 8.0f;
    s.color = {16.0f / 255.0f, 54.0f / 255.0f, 38.0f / 255.0f, 0.05f};
    return s;
}

// ---- 可用工具弹窗（池170 v2，取代 v1 2 列定高网格版）----
//
// 纯查阅弹窗（无表单无 footer 主次钮）：弹窗壳 w=min(720,可用宽−48)
// h=min(600,可用高−96)（glassFill + 1px 细黑边 + 冷阴影 blur24 + radiusLarge 14，
// §3.2 白卡+细黑边老板裁决）→ 头部 56（标题 + 「N 个」badge 恒全量 | ✕ 28×28）+
// 1px 分割线 + 搜索区（锚顶固定不滚，§3.6：框高 36、左 fa-search、右计数/清除）+
// 列表区（唯一内滚，全宽单列卡、高随内容自适应；描述全文展示 >3 行 clamp+展开，
// 修 bug #7）。N=0 搜索区不渲染走 §3.5 空态；搜索 0 命中走 §3.6 搜索空态。
// 关闭 = ✕ / backdrop / Esc（Esc 有输入先清搜索——模块 onKeyEvent 分支 + 输入框
// onCancel 同语义）。工具注册表快照同步装载（ensureToolsCache），打开即 forceReload
// ——「加载中/错误」态在同步语义下不可达（listRegisteredTools 只枚举进程内注册表，
// 不失败），状态表只落「空态/搜索空态/常态」。
void AgentModule::composeToolsDialog(
    ::eui::Ui& ui, const ::eui::Screen& screen, const ::core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& themeTokens) {
    const float w = std::min(720.0f, std::max(0.0f, screen.width - 48.0f));
    const float h = std::min(600.0f, std::max(0.0f, screen.height - 96.0f));
    const float innerW = std::max(0.0f, w - 32.0f);  // 通栏 pad 16×2
    const eui::Color kTransparent{0.0f, 0.0f, 0.0f, 0.0f};
    const std::string queryLower = core::str::toLowerAscii(toolsSearch_);
    const bool searchActive = !toolsSearch_.empty() && !toolsCache_.empty();

    // 过滤（名称+说明+参数名子串；30+ 条同步过滤，每字符即时生效无需防抖；
    // 空词 = 全量——toolMatchesQuery 对空 q 恒真）
    std::vector<const ToolInfo*> visible;
    for (const auto& tool : toolsCache_) {
        if (toolMatchesQuery(tool, queryLower)) visible.push_back(&tool);
    }

    // 弹窗壳（§3.2）：glassFill + 细黑边 + 冷阴影 blur24 offsetY4 + radiusLarge 14
    components::DialogStyle shell(themeTokens);
    shell.surface = ui_kit::glassFill(t);
    shell.border = toolsShellBorder(t);
    shell.backdrop = components::theme::color(0.0f, 0.0f, 0.0f, 0.40f);
    shell.shadow.enabled = true;
    shell.shadow.offset = {0.0f, 4.0f};
    shell.shadow.blur = 24.0f;
    shell.shadow.color = ui_kit::glassShadowColor(t);
    shell.radius = t.sizes.radiusLarge;

    components::dialog(ui, "agent.dialog.tools")
        .open(toolsDialogOpen_)
        .screen(screen.width, screen.height)
        .size(w, h)
        .style(shell)
        .onOpenChange([this](bool v) {
            if (!v) toolsDialogOpen_ = false;
        })
        .content([&] {
            if (!toolsDialogOpen_) return;
            // 头部 56（§3.3）：「可用工具」16/700 + badge「N 个」（恒全量不随搜索变）
            const float titleFs = core::theme::scaled(16.0f);
            const float titleW = core::TextPrimitive::measureTextWidth(
                "可用工具", "", titleFs, 700);
            ui.text("agent.dialog.tools.title")
                .position(20.0f, (kToolsHeaderH - titleFs) * 0.5f)
                .size(titleW, titleFs)
                .text("可用工具")
                .fontSize(titleFs)
                .lineHeight(titleFs)
                .fontWeight(700)
                .color(t.colors.textPrimary)
                .build();
            const float badgeFs = core::theme::scaled(12.0f);
            const float badgeH = core::theme::scaled(20.0f);
            const std::string badgeText =
                std::to_string(toolsCache_.size()) + " 个";
            const float badgeW =
                core::TextPrimitive::measureTextWidth(badgeText, "", badgeFs,
                                                      600) +
                core::theme::scaled(12.0f);  // pill 内边距 6×2
            const float badgeY = (kToolsHeaderH - badgeH) * 0.5f;
            ui.rect("agent.dialog.tools.badge")
                .position(20.0f + titleW + 8.0f, badgeY)
                .size(badgeW, badgeH)
                .color(t.colors.primarySoftBg)
                .radius(t.sizes.radiusPill)
                .build();
            ui.text("agent.dialog.tools.badge.text")
                .position(20.0f + titleW + 8.0f, badgeY)
                .size(badgeW, badgeH)
                .text(badgeText)
                .fontSize(badgeFs)
                .lineHeight(badgeH)
                .fontWeight(600)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.primaryText)
                .hitTestMode(::core::dsl::HitTestMode::None)
                .build();
            // ✕ 关闭（28×28 右缘内 12，hover 底）：bg 命中面 + FA 码点穿透文本
            const float closeS = core::theme::scaled(28.0f);
            const float closeX = w - 12.0f - closeS;
            const float closeY = (kToolsHeaderH - closeS) * 0.5f;
            ui.rect("agent.dialog.tools.close.bg")
                .position(closeX, closeY)
                .size(closeS, closeS)
                .states(kTransparent, t.colors.hover, t.colors.hover)
                .radius(6.0f)
                .onClick([this] { toolsDialogOpen_ = false; })
                .build();
            ui.text("agent.dialog.tools.close")
                .position(closeX, closeY)
                .size(closeS, closeS)
                .icon(::eui::utf8(0xF00D))  // fa-xmark
                .fontSize(core::theme::scaled(14.0f))
                .lineHeight(closeS)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textSecondary)
                .hitTestMode(::core::dsl::HitTestMode::None)
                .onClick([this] { toolsDialogOpen_ = false; })
                .build();
            // 分割线（§3.3：头部下 1px 通宽 borderDefault）
            ui.rect("agent.dialog.tools.divider")
                .position(0.0f, kToolsHeaderH)
                .size(w, 1.0f)
                .color(t.colors.borderDefault)
                .build();

            // N=0 → §3.5 空态居中；搜索区整体不渲染（无内容可搜，留框即无效控件）
            const float listY = kToolsHeaderH + 1.0f + 12.0f +
                                kToolsSearchBoxH + 8.0f + 8.0f;
            if (toolsCache_.empty()) {
                const float areaY = kToolsHeaderH + 1.0f;
                const float areaH =
                    std::max(0.0f, h - areaY - 16.0f);
                const float iconFs = core::theme::scaled(24.0f);
                const float row1 = core::theme::scaled(20.0f);
                const float row2 = core::theme::scaled(18.0f);
                const float totalH = iconFs + 8.0f + row1 + 4.0f + row2;
                float ey = areaY + std::max(0.0f, (areaH - totalH) * 0.5f);
                ui.text("agent.dialog.tools.empty.icon")
                    .position(16.0f, ey)
                    .size(innerW, iconFs)
                    .icon(::eui::utf8(0xF1E6))  // fa-plug
                    .fontSize(iconFs)
                    .lineHeight(iconFs)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .color(t.colors.textTertiary)
                    .hitTestMode(::core::dsl::HitTestMode::None)
                    .build();
                ey += iconFs + 8.0f;
                ui.text("agent.dialog.tools.empty")
                    .position(16.0f, ey)
                    .size(innerW, row1)
                    .text("暂无可用工具")
                    .fontSize(core::theme::scaled(13.0f))
                    .lineHeight(row1)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .color(t.colors.textSecondary)
                    .hitTestMode(::core::dsl::HitTestMode::None)
                    .build();
                ey += row1 + 4.0f;
                ui.text("agent.dialog.tools.empty.hint")
                    .position(16.0f, ey)
                    .size(innerW, row2)
                    .text("智能体未装配任何工具")
                    .fontSize(core::theme::scaled(12.0f))
                    .lineHeight(row2)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .color(t.colors.textTertiary)
                    .hitTestMode(::core::dsl::HitTestMode::None)
                    .build();
                return;
            }

            // 搜索区（§3.6，锚顶固定不滚）：框高 36 radius 8 通栏；底=次钮底档，
            // 无边框，focus 1px primary 描边（组件底色不随 focus 切换——见偏差清单）
            const float searchY = kToolsHeaderH + 1.0f + 12.0f;
            components::InputStyle searchStyle(themeTokens);
            searchStyle.background = toolNeutralFill(t);
            searchStyle.focused = t.colors.surface;
            searchStyle.border = toolNeutralFill(t);  // 与底同色 = 视觉无边框
            searchStyle.focusBorder = t.colors.primary;
            searchStyle.text = t.colors.textPrimary;
            searchStyle.placeholder = t.colors.textTertiary;
            searchStyle.cursor = t.colors.primary;
            searchStyle.radius = 8.0f;
            searchStyle.shadow = core::Shadow{};
            components::input(ui, "agent.dialog.tools.search")
                .position(16.0f, searchY)
                .size(innerW, kToolsSearchBoxH)
                .value(toolsSearch_)
                .placeholder("搜索工具名称或说明")
                .fontSize(core::theme::scaled(13.0f))
                .inset(35.0f)  // 左图标位；组件 inset 左右对称（右 92 不可达）
                .style(searchStyle)
                .transition(0.12f, core::Ease::OutCubic)
                .onChange([this](const std::string& v) { toolsSearch_ = v; })
                .onCancel([this] {
                    // Esc 优先级（§3.6）：有输入先清搜索（弹窗不关），空输入关弹窗
                    if (!toolsSearch_.empty()) {
                        toolsSearch_.clear();
                    } else {
                        toolsDialogOpen_ = false;
                    }
                })
                .build();
            // 左装饰图标（纯装饰 pointer 穿透；FA5 search 0xF002）
            ui.text("agent.dialog.tools.search.icon")
                .position(16.0f + 11.0f, searchY)
                .size(core::theme::scaled(14.0f), kToolsSearchBoxH)
                .icon(::eui::utf8(0xF002))
                .fontSize(core::theme::scaled(14.0f))
                .lineHeight(kToolsSearchBoxH)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textTertiary)
                .hitTestMode(::core::dsl::HitTestMode::None)
                .build();
            // 右端：计数「n / 全量」（仅过滤生效时显示）+ 清除 ×（有输入才显示）
            if (searchActive) {
                const float clearS = core::theme::scaled(28.0f);
                const float clearX = 16.0f + innerW - 12.0f - clearS;
                const float clearY =
                    searchY + (kToolsSearchBoxH - clearS) * 0.5f;
                const auto clearSearch = [this, &ui] {
                    toolsSearch_.clear();
                    ui.requestFocus("agent.dialog.tools.search.hit");  // 回焦
                };
                const std::string countText =
                    std::to_string(visible.size()) + " / " +
                    std::to_string(toolsCache_.size());
                const float countFs = core::theme::scaled(12.0f);
                const float countW = core::TextPrimitive::measureTextWidth(
                                        countText, "", countFs, 400) +
                                    8.0f;
                ui.text("agent.dialog.tools.search.count")
                    .position(clearX - 8.0f - countW, searchY)
                    .size(countW, kToolsSearchBoxH)
                    .text(countText)
                    .fontSize(countFs)
                    .lineHeight(kToolsSearchBoxH)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textTertiary)
                    .hitTestMode(::core::dsl::HitTestMode::None)
                    .build();
                ui.rect("agent.dialog.tools.search.clear.bg")
                    .position(clearX, clearY)
                    .size(clearS, clearS)
                    .states(kTransparent, t.colors.hover, t.colors.hover)
                    .radius(6.0f)
                    .onClick(clearSearch)
                    .build();
                ui.text("agent.dialog.tools.search.clear")
                    .position(clearX, clearY)
                    .size(clearS, clearS)
                    .icon(::eui::utf8(0xF00D))
                    .fontSize(core::theme::scaled(10.0f))
                    .lineHeight(clearS)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textTertiary)
                    .hitTestMode(::core::dsl::HitTestMode::None)
                    .onClick(clearSearch)
                    .build();
            }

            // 搜索空态（§3.6，0 命中）：search 图标 + 文案 + 「清空搜索」钮居中
            if (searchActive && visible.empty()) {
                const float areaH = std::max(0.0f, h - listY - 16.0f);
                const float iconFs = core::theme::scaled(24.0f);
                const float row1 = core::theme::scaled(20.0f);
                const float row2 = core::theme::scaled(18.0f);
                const float btnFs = core::theme::scaled(12.0f);
                const float btnH = core::theme::scaled(26.0f);
                const float btnW = core::TextPrimitive::measureTextWidth(
                                       "清空搜索", "", btnFs, 500) +
                                   core::theme::scaled(16.0f);
                const float totalH =
                    iconFs + 8.0f + row1 + 4.0f + row2 + 10.0f + btnH;
                float ey = listY + std::max(0.0f, (areaH - totalH) * 0.5f);
                ui.text("agent.dialog.tools.noresult.icon")
                    .position(16.0f, ey)
                    .size(innerW, iconFs)
                    .icon(::eui::utf8(0xF002))
                    .fontSize(iconFs)
                    .lineHeight(iconFs)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .color(t.colors.textTertiary)
                    .hitTestMode(::core::dsl::HitTestMode::None)
                    .build();
                ey += iconFs + 8.0f;
                ui.text("agent.dialog.tools.noresult.title")
                    .position(16.0f, ey)
                    .size(innerW, row1)
                    .text("未找到匹配工具")
                    .fontSize(core::theme::scaled(13.0f))
                    .lineHeight(row1)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .color(t.colors.textSecondary)
                    .hitTestMode(::core::dsl::HitTestMode::None)
                    .build();
                ey += row1 + 4.0f;
                ui.text("agent.dialog.tools.noresult.hint")
                    .position(16.0f, ey)
                    .size(innerW, row2)
                    .text("试试其他关键词")
                    .fontSize(core::theme::scaled(12.0f))
                    .lineHeight(row2)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .color(t.colors.textTertiary)
                    .hitTestMode(::core::dsl::HitTestMode::None)
                    .build();
                ey += row2 + 10.0f;
                const float btnX = 16.0f + (innerW - btnW) * 0.5f;
                const auto clearAndRefocus = [this, &ui] {
                    toolsSearch_.clear();
                    ui.requestFocus("agent.dialog.tools.search.hit");
                };
                ui.rect("agent.dialog.tools.noresult.btn.bg")
                    .position(btnX, ey)
                    .size(btnW, btnH)
                    .states(kTransparent, t.colors.hover, t.colors.hover)
                    .radius(6.0f)
                    .onClick(clearAndRefocus)
                    .build();
                ui.text("agent.dialog.tools.noresult.btn")
                    .position(btnX, ey)
                    .size(btnW, btnH)
                    .text("清空搜索")
                    .fontSize(btnFs)
                    .lineHeight(btnH)
                    .fontWeight(500)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.primaryText)
                    .hitTestMode(::core::dsl::HitTestMode::None)
                    .onClick(clearAndRefocus)
                    .build();
                return;
            }
            // 列表区（§2.2 唯一内滚容器；pad 8 顶 16 底，头部/分割线/搜索区锚顶）
            components::scrollView(ui, "agent.dialog.tools.scroll")
                .position(16.0f, listY)
                .size(innerW, std::max(0.0f, h - listY - 16.0f))
                .gap(8.0f)
                .step(core::theme::scaled(96.0f))
                .theme(themeTokens)
                .contentKey("agent.dialog.tools.scroll.n" +
                            std::to_string(visible.size()) + ".w" +
                            std::to_string(static_cast<int>(innerW)) +
                            ".q" + toolsSearch_ +
                            ".e" + std::to_string(toolsListVersion_))
                .content([&](eui::Ui& inner, float cw, float) {
                    // 全宽单列垂直流（§2.3）：卡高随内容自适应，禁定高网格
                    inner.flow("agent.dialog.tools.list")
                        .width(cw)
                        .gap(8.0f)
                        .lineGap(8.0f)
                        .content([&] {
                            for (size_t k = 0; k < visible.size(); ++k) {
                                renderToolCard(inner, cw, k, *visible[k],
                                               queryLower, t);
                            }
                        })
                        .build();
                })
                .build();
        })
        .build();

    // Esc 清搜索后的焦点回填：Runtime 帧末消费（md 编辑器同款补发口；模块
    // onKeyEvent 分支无 Ui&，置 toolsSearchFocusPending_ 由本函数补发）
    if (toolsDialogOpen_ && toolsSearchFocusPending_) {
        toolsSearchFocusPending_ = false;
        ui.requestFocus("agent.dialog.tools.search.hit");
    }
}

// ---- 工具卡（池170 v2 §3.4）----
//
// 全宽单列、高随内容自适应（禁定高）：pad 12 14 → 头行 28（图标盒 + 族图标 +
// 名 14/600 单行省略 + [chevron]）→ 6 → 描述块（13px/20 wrap，默认全文展示；
// >3 行 clamp 3 行 + 末行省略号 + 「展开/收起」文字钮，整卡点击开合）→ 6 →
// 参数 chips 行（11px/500 flex-wrap 全量，+N 截断退役；无参数不渲染整行）。
// 仅长文卡有 hover（底 hover + 边框加深 active）/按下果冻 0.96/点击态；短文
// 查阅卡零交互。命中词在名称/描述/chips 内原位 primarySoftBg 底色标记（先画
// 底 rect 再画字，字色继承原层级）。
void AgentModule::renderToolCard(
    ::eui::Ui& inner, float cw, size_t visibleIdx, const ToolInfo& tool,
    const std::string& queryLower, const ::core::theme::Tokens& t) {
    const std::string cid =
        "agent.dialog.tools.card." + std::to_string(visibleIdx);
    const float padY = core::theme::scaled(12.0f);
    const float padX = core::theme::scaled(14.0f);
    const float headH = core::theme::scaled(28.0f);
    const float lh = core::theme::scaled(kToolsDescLh);
    const float descFs = core::theme::scaled(13.0f);
    const float bodyW = std::max(0.0f, cw - 2.0f * padX);
    const float hlH = core::theme::scaled(15.0f);  // 高亮底条高（行盒内垂直居中）
    const eui::Color kTransparent{0.0f, 0.0f, 0.0f, 0.0f};

    // 描述换行 → 行数定长文卡与卡高（§3.4：默认全文展示，>3 行才 clamp）
    const std::string descText =
        tool.description.empty() ? std::string("（无描述）") : tool.description;
    const std::vector<std::string> allLines =
        wrapToolDescLines(descText, bodyW, descFs);
    const bool expandable = allLines.size() > kToolsDescClampLines;
    const bool expanded = toolsExpandedNames_.count(tool.name) > 0;
    const bool clamped = expandable && !expanded;
    std::vector<std::string> showLines;
    if (allLines.empty()) {
        showLines.push_back("");  // 至少一行占位（空描述画「（无描述）」单行）
    } else {
        const std::size_t n = clamped ? std::min(allLines.size(),
                                                 kToolsDescClampLines)
                                      : allLines.size();
        showLines.assign(allLines.begin(),
                         allLines.begin() + static_cast<std::ptrdiff_t>(n));
        if (clamped) {
            showLines.back() =
                ellipsizeToolLine(showLines.back(), bodyW, descFs);
        }
    }
    const float descH =
        static_cast<float>(showLines.size()) * lh;

    // 参数 chips 逐枚测宽换行（flex-wrap 全量展示；首枚超宽也放行防死循环）
    const float chipFs = core::theme::scaled(11.0f);
    const float chipH = core::theme::scaled(20.0f);
    const float chipPadX = core::theme::scaled(8.0f);
    const float chipGap = core::theme::scaled(4.0f);
    struct ChipPlace {
        std::size_t idx;
        float x;
        float y;
        float w;
    };
    std::vector<ChipPlace> chipPlaces;
    float chipCursorX = 0.0f;
    float chipCursorY = 0.0f;
    for (std::size_t p = 0; p < tool.params.size(); ++p) {
        const float chipW =
            core::TextPrimitive::measureTextWidth(tool.params[p], "", chipFs,
                                                  500) +
            2.0f * chipPadX;
        if (chipCursorX > 0.0f && chipCursorX + chipW > bodyW) {
            chipCursorX = 0.0f;
            chipCursorY += chipH + chipGap;
        }
        chipPlaces.push_back({p, chipCursorX, chipCursorY, chipW});
        chipCursorX += chipW + chipGap;
    }
    const bool hasParams = !tool.params.empty();
    const float chipsH = hasParams ? chipCursorY + chipH : 0.0f;

    const float toggleH =
        expandable ? core::theme::scaled(24.0f) : 0.0f;
    const float cardH = 2.0f * padY + headH + core::theme::scaled(6.0f) +
                        descH + toggleH +
                        (hasParams ? core::theme::scaled(6.0f) + chipsH : 0.0f);

    // 交互态（仅长文卡）：hover 底/边框加深 + 按下果冻，短文卡零交互态
    const bool hovered = expandable && toolsHoverCard_ == tool.name;
    const bool pressed = expandable && toolsPressedCards_.count(tool.name) > 0;
    const eui::Color cardBase = t.colors.surfaceElevated;  // 暗 #2C2A28 比壳浅半档

    inner.stack(cid)
        .size(cw, cardH)
        .content([&] {
            inner.rect(cid + ".bg")
                .size(cw, cardH)
                .states(cardBase,
                        expandable ? t.colors.hover : cardBase,
                        expandable ? t.colors.hover : cardBase)
                .radius(8.0f)
                .border(1.0f, expandable && hovered ? t.colors.active
                                                    : t.colors.borderDefault)
                .shadow(toolCardShadow(t))
                .transformOrigin(0.5f, 0.5f)
                .scale(pressed ? 0.96f : 1.0f)
                .transition(core::Transition::make(0.18f, core::Ease::OutBack))
                .animate(core::AnimProperty::Transform)
                .build();
            if (expandable) {
                // 命中面：整卡开合 + hover/按下集合代报（bg 每帧读出目标值）
                inner.rect(cid + ".hit")
                    .size(cw, cardH)
                    .zIndex(1)
                    .states(kTransparent, kTransparent, kTransparent)
                    .onHover([this, name = tool.name](bool h) {
                        if (h) {
                            toolsHoverCard_ = name;
                        } else if (toolsHoverCard_ == name) {
                            toolsHoverCard_.clear();
                        }
                    })
                    .onPress([this, name = tool.name](const core::PointerEvent&,
                                                      const core::Rect&) {
                        toolsPressedCards_.insert(name);
                    })
                    .onRelease([this, name = tool.name](const core::PointerEvent&,
                                                        const core::Rect&) {
                        toolsPressedCards_.erase(name);
                    })
                    .onClick([this, name = tool.name] {
                        toggleToolCardExpanded(name);
                    })
                    .build();
            }
            inner.column(cid + ".body")
                .position(padX, padY)
                .size(bodyW, std::max(0.0f, cardH - 2.0f * padY))
                .zIndex(2)
                .gap(core::theme::scaled(6.0f))
                .content([&] {
                    // 头行 28：图标盒 28×28 radius 6 primarySoftBg + 族图标 +
                    // 名（单行省略兜底）+ [chevron（仅长文卡，展开旋转 180°）]
                    const float boxS = headH;
                    const float chevW =
                        expandable ? core::theme::scaled(16.0f) : 0.0f;
                    const float nameW = std::max(
                        0.0f, bodyW - boxS - core::theme::scaled(8.0f) - chevW);
                    inner.row(cid + ".head")
                        .size(bodyW, headH)
                        .gap(core::theme::scaled(8.0f))
                        .alignItems(eui::Align::CENTER)
                        .content([&] {
                            inner.stack(cid + ".icon.wrap")
                                .size(boxS, boxS)
                                .content([&] {
                                    inner.rect(cid + ".icon.box")
                                        .size(boxS, boxS)
                                        .radius(6.0f)
                                        .color(t.colors.primarySoftBg)
                                        .build();
                                    inner.text(cid + ".icon")
                                        .size(boxS, boxS)
                                        .icon(::eui::utf8(
                                            toolFamilyIconGlyph(tool.name)))
                                        .fontSize(core::theme::scaled(14.0f))
                                        .lineHeight(boxS)
                                        .horizontalAlign(
                                            eui::HorizontalAlign::Center)
                                        .verticalAlign(eui::VerticalAlign::Center)
                                        .color(t.colors.primaryText)
                                        .hitTestMode(
                                            ::core::dsl::HitTestMode::None)
                                        .build();
                                })
                                .build();
                            // 名称 + 命中高亮（clip 防省略号态高亮溢出名字区）
                            inner.stack(cid + ".name.wrap")
                                .size(nameW, headH)
                                .clip()
                                .content([&] {
                                    const std::string nameLower =
                                        core::str::toLowerAscii(tool.name);
                                    for (const auto& [ms, me] :
                                         findMatchRanges(nameLower, queryLower)) {
                                        const float px =
                                            core::TextPrimitive::measureTextWidth(
                                                tool.name.substr(0, ms), "",
                                                core::theme::scaled(14.0f), 600);
                                        const float pw =
                                            core::TextPrimitive::measureTextWidth(
                                                tool.name.substr(ms, me - ms),
                                                "", core::theme::scaled(14.0f),
                                                600);
                                        inner.rect(cid + ".name.hl." +
                                                   std::to_string(ms))
                                            .position(px, (headH - hlH) * 0.5f)
                                            .size(pw, hlH)
                                            .radius(2.0f)
                                            .color(t.colors.primarySoftBg)
                                            .build();
                                    }
                                    inner.text(cid + ".name")
                                        .size(nameW, headH)
                                        .text(tool.name)
                                        .fontSize(core::theme::scaled(14.0f))
                                        .lineHeight(headH)
                                        .fontWeight(600)
                                        .wrap(false)
                                        .maxWidth(nameW)
                                        .overflow(core::TextOverflow::Ellipsis)
                                        .verticalAlign(eui::VerticalAlign::Center)
                                        .color(t.colors.textPrimary)
                                        .hitTestMode(
                                            ::core::dsl::HitTestMode::None)
                                        .build();
                                })
                                .build();
                            if (expandable) {
                                inner.text(cid + ".chevron")
                                    .size(chevW, headH)
                                    .icon(::eui::utf8(0xF078))  // fa-chevron-down
                                    .fontSize(core::theme::scaled(12.0f))
                                    .lineHeight(headH)
                                    .horizontalAlign(eui::HorizontalAlign::Center)
                                    .verticalAlign(eui::VerticalAlign::Center)
                                    .color(t.colors.textTertiary)
                                    .rotate(expanded ? 3.14159265f : 0.0f)
                                    .transition(0.15f, core::Ease::OutCubic)
                                    .animate(core::AnimProperty::Transform)
                                    .hitTestMode(::core::dsl::HitTestMode::None)
                                    .build();
                            }
                        })
                        .build();
                    // 描述块（修 bug #7）：逐行渲染（换行测量与渲染同引擎）；
                    // 命中词 primarySoftBg 底条原位标记，clamp 态末行塞省略号
                    inner.stack(cid + ".desc")
                        .size(bodyW, descH)
                        .content([&] {
                            const std::vector<std::pair<std::size_t, std::size_t>>
                                hayRanges = findMatchRanges(
                                    core::str::toLowerAscii(descText), queryLower);
                            std::size_t consumed = 0;  // 原文中已消费字节（逐行推进）
                            for (std::size_t li = 0; li < showLines.size(); ++li) {
                                const std::string& line = showLines[li];
                                const float lineY =
                                    static_cast<float>(li) * lh;
                                const std::size_t lineStart = consumed;
                                const std::size_t lineEnd = consumed + line.size();
                                for (const auto& [ms, me] : hayRanges) {
                                    if (ms >= lineEnd || me <= lineStart) continue;
                                    const std::size_t s =
                                        std::max(ms, lineStart) - lineStart;
                                    const std::size_t e =
                                        std::min(me, lineEnd) - lineStart;
                                    const float px =
                                        core::TextPrimitive::measureTextWidth(
                                            line.substr(0, s), "", descFs, 400);
                                    const float pw =
                                        core::TextPrimitive::measureTextWidth(
                                            line.substr(s, e - s), "", descFs,
                                            400);
                                    inner.rect(cid + ".desc.hl." +
                                               std::to_string(li) + "." +
                                               std::to_string(ms))
                                        .position(px,
                                                  lineY + (lh - hlH) * 0.5f)
                                        .size(pw, hlH)
                                        .radius(2.0f)
                                        .color(t.colors.primarySoftBg)
                                        .build();
                                }
                                inner.text(cid + ".desc." + std::to_string(li))
                                    .position(0.0f, lineY)
                                    .size(bodyW, lh)
                                    .text(line.empty() ? std::string(" ") : line)
                                    .fontSize(descFs)
                                    .lineHeight(lh)
                                    .wrap(false)
                                    .color(t.colors.textSecondary)
                                    .hitTestMode(::core::dsl::HitTestMode::None)
                                    .build();
                                consumed = lineEnd;
                            }
                        })
                        .build();
                    if (expandable) {
                        // 「展开/收起」文字钮（描述块下方；点击与整卡同一开关）
                        inner.stack(cid + ".toggle")
                            .size(bodyW, toggleH)
                            .content([&] {
                                const std::string label =
                                    expanded ? std::string("收起")
                                             : std::string("展开");
                                const float toggleFs = core::theme::scaled(12.0f);
                                const float toggleW =
                                    core::TextPrimitive::measureTextWidth(
                                        label, "", toggleFs, 500) +
                                    core::theme::scaled(16.0f);
                                inner.rect(cid + ".toggle.bg")
                                    .size(toggleW, toggleH)
                                    .states(kTransparent, t.colors.hover,
                                            t.colors.hover)
                                    .radius(6.0f)
                                    .onClick([this, name = tool.name] {
                                        toggleToolCardExpanded(name);
                                    })
                                    .build();
                                inner.text(cid + ".toggle")
                                    .size(toggleW, toggleH)
                                    .text(label)
                                    .fontSize(toggleFs)
                                    .lineHeight(toggleH)
                                    .fontWeight(500)
                                    .horizontalAlign(eui::HorizontalAlign::Center)
                                    .verticalAlign(eui::VerticalAlign::Center)
                                    .color(t.colors.primaryText)
                                    .hitTestMode(::core::dsl::HitTestMode::None)
                                    .onClick([this, name = tool.name] {
                                        toggleToolCardExpanded(name);
                                    })
                                    .build();
                            })
                            .build();
                    }
                    if (hasParams) {
                        // 参数 chips（高 20 radius 6 次钮底档；命中词原位高亮）
                        inner.stack(cid + ".chips")
                            .size(bodyW, chipsH)
                            .content([&] {
                                for (const auto& place : chipPlaces) {
                                    const std::string& label =
                                        tool.params[place.idx];
                                    const std::string chid = cid + ".chip." +
                                                             std::to_string(
                                                                 place.idx);
                                    inner.rect(chid)
                                        .position(place.x, place.y)
                                        .size(place.w, chipH)
                                        .radius(6.0f)
                                        .color(toolNeutralFill(t))
                                        .build();
                                    for (const auto& [ms, me] :
                                         findMatchRanges(
                                             core::str::toLowerAscii(label),
                                             queryLower)) {
                                        const float px =
                                            core::TextPrimitive::measureTextWidth(
                                                label.substr(0, ms), "", chipFs,
                                                500);
                                        const float pw =
                                            core::TextPrimitive::measureTextWidth(
                                                label.substr(ms, me - ms), "",
                                                chipFs, 500);
                                        inner.rect(chid + ".hl." +
                                                   std::to_string(ms))
                                            .position(
                                                place.x + chipPadX + px,
                                                place.y + (chipH - hlH) * 0.5f)
                                            .size(pw, hlH)
                                            .radius(2.0f)
                                            .color(t.colors.primarySoftBg)
                                            .build();
                                    }
                                    inner.text(chid + ".text")
                                        .position(place.x, place.y)
                                        .size(place.w, chipH)
                                        .text(label)
                                        .fontSize(chipFs)
                                        .lineHeight(chipH)
                                        .fontWeight(500)
                                        .horizontalAlign(
                                            eui::HorizontalAlign::Center)
                                        .verticalAlign(eui::VerticalAlign::Center)
                                        .color(t.colors.textSecondary)
                                        .hitTestMode(
                                            ::core::dsl::HitTestMode::None)
                                        .build();
                                }
                            })
                            .build();
                    }
                })
                .build();
        })
        .build();
}

// 长文卡展开/收起（按工具名定位——搜索过滤后下标漂移，名字稳定）；bump 版本号
// 触发 scrollView 内容重测（contentKey 已编入）
void AgentModule::toggleToolCardExpanded(const std::string& name) {
    if (toolsExpandedNames_.count(name) > 0) {
        toolsExpandedNames_.erase(name);
    } else {
        toolsExpandedNames_.insert(name);
    }
    ++toolsListVersion_;
}

// ---- AI 草稿子弹窗（T7b）----
//
// 异步时序：主线程 startDraftGeneration 起 worker（requestTaskDraft → parseTaskDraft →
// 写槽位 → done.release → requestUiUpdate 要帧）；主线程 compose 每帧 reapDraftJob
//（done.acquire → join → 预填/报错/丢弃）。worker 全程不摸 UI 成员（输入/槽位值拷贝）；
// onStop join 先于 llm_.reset()；关闭子弹窗只 bump epoch 作废结果，请求不杀
//（DirectLlmClient 无取消口，已定案不动）。

void AgentModule::composeDraftDialog(
    ::eui::Ui& ui, const ::eui::Screen& screen, const ::core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& themeTokens) {
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Standard);
    const float h = kDraftDialogH;
    const float x0 = ui_kit::kDialogInsetX;
    const float innerW = w - ui_kit::kDialogBodyInsetX;
    const bool generating = draftPhase_ == DraftPhase::Generating;

    ui_kit::DialogFrame frame;
    frame.id = "agent.dialog.task.draft";
    frame.width = ui_kit::DialogWidth::Standard;
    frame.height = h;
    frame.open = draftDialogOpen_;
    frame.zIndex = 1100;   // 叠在任务弹窗（组件默认 1000）之上；遮罩同时挡住外层
    frame.onOpenChange = [this](bool v) {
        if (!v) closeDraftDialog();
    };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
        if (!draftDialogOpen_) return;
        ui_kit::drawDialogTitle(ui, "agent.dialog.task.draft.title", w,
                                "AI 生成草稿", ui_kit::kDialogTitleFontSize,
                                t.colors.textPrimary, {.y = 16.0f});

        // ✕ 关闭（与遮罩 / Esc / 「关闭」按钮同一路径）
        ui.text("agent.dialog.task.draft.close")
            .position(w - 56.0f, 22.0f)
            .size(core::theme::scaled(28.0f), core::theme::scaled(28.0f))
            .text("✕")
            .fontSize(core::theme::scaled(14.0f))
            .lineHeight(core::theme::scaled(28.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary)
            .onClick([this] { closeDraftDialog(); })
            .build();

        ui_kit::drawDialogText(
            ui, "agent.dialog.task.draft.caption", w,
            "用一句话描述这个任务，AI 生成表单草稿；结果只预填，保存仍由你确认",
            t.sizes.fontSizeSmall, t.colors.textSecondary,
            {.x = x0, .y = 64.0f, .height = 18.0f, .lineHeight = 18.0f});

        components::input(ui, "agent.dialog.task.draft.input")
            .position(x0, 92.0f)
            .size(innerW, kDraftInputH)
            .value(draftInput_)
            .placeholder("例如：每天早上 8 点看一下热榜，有大新闻就通知我")
            .fontSize(t.sizes.fontSizeBody)
            .multiline(true)
            .scrollbar(true)
            .theme(themeTokens)
            .onChange([this](const std::string& v) { draftInput_ = v; })
            .onCancel([this] { closeDraftDialog(); })   // 输入框内 Esc = 关子弹窗
            .build();

        // 底部按钮带（UAT6 重排）：生成=Primary 微信绿（核心确认）、关闭=Secondary
        //（次要），两钮同排整体居中、行顶 y=h-60（与 dialog_form 按钮带同口径）。
        const float btnY = h - 60.0f;
        const float btnX0 = (w - (kDraftBtnW * 2.0f + kDraftBtnGap)) * 0.5f;
        components::button(ui, "agent.dialog.task.draft.generate")
            .position(btnX0, btnY)
            .size(kDraftBtnW, kDraftBtnH)
            .text(generating ? "生成中…" : "生成草稿")
            .fontSize(t.sizes.fontSizeBody - 1.0f)
            .theme(themeTokens, true)
            .textColor(t.colors.onPrimary)
            .radius(t.sizes.radius)
            .disabled(generating)
            .onClick([this] { startDraftGeneration(); })
            .build();
        components::button(ui, "agent.dialog.task.draft.dismiss")
            .position(btnX0 + kDraftBtnW + kDraftBtnGap, btnY)
            .size(kDraftBtnW, kDraftBtnH)
            .text("关闭")
            .fontSize(t.sizes.fontSizeBody - 1.0f)
            .theme(themeTokens, false)
            .textColor(t.colors.textPrimary)
            .radius(t.sizes.radius)
            .border(1.0f, t.colors.borderDefault)
            .onClick([this] { closeDraftDialog(); })
            .build();

        // 结果行（Done → notice+warnings；Failed → error；同一槽位互斥渲染）。
        // 错误行走 wechat.warningRed（UAT6 微信化）；警告保留 amber t.colors.warning
        //（微信档无 amber 槽位，WeChat 化模块同款先例：claude_ui_detail 警示行）。
        ui_kit::drawDialogText(ui, "agent.dialog.task.draft.notice", w, draftNotice_,
                               t.sizes.fontSizeSmall, t.colors.textSecondary,
                               {.x = x0, .y = kDraftResultY, .height = 18.0f,
                                .lineHeight = 18.0f, .skipIfEmpty = true});
        ui_kit::drawDialogText(ui, "agent.dialog.task.draft.warnings", w,
                               draftWarnings_, t.sizes.fontSizeSmall,
                               t.colors.warning,
                               {.x = x0, .y = kDraftResultY + 18.0f, .height = 44.0f,
                                .lineHeight = 16.0f, .wrap = true,
                                .skipIfEmpty = true});
        ui_kit::drawDialogText(ui, "agent.dialog.task.draft.error", w, draftError_,
                               t.sizes.fontSizeSmall, t.colors.errorText,
                               {.x = x0, .y = kDraftResultY, .height = 18.0f,
                                .lineHeight = 18.0f, .skipIfEmpty = true});
    });
}

void AgentModule::openDraftDialog() {
    draftDialogOpen_ = true;
    std::lock_guard<std::mutex> lock(draftMu_);
    if (draftJob_ != nullptr &&
        !draftJob_->done.load(std::memory_order_acquire)) {
        draftPhase_ = DraftPhase::Generating;   // 请求在途：如实显示生成中
    } else if (draftPhase_ == DraftPhase::Generating) {
        draftPhase_ = DraftPhase::Idle;         // 状态陈旧（job 已被收走）
    }
    // Done/Failed 的提示行保留到下次生成前——重开还能看到上次结果说明
}

void AgentModule::closeDraftDialog() {
    if (!draftDialogOpen_) return;
    draftDialogOpen_ = false;
    // 关闭 = 作废未收割的结果：epoch bump 后 reap 走丢弃分支（请求本身不杀）。
    // Generating 阶段位保留：重开如实显示生成中，跑完在 reap 丢弃后回 Idle + 错误行点名
    ++draftEpoch_;
    if (draftPhase_ != DraftPhase::Generating) {
        draftPhase_ = DraftPhase::Idle;
    }
    draftNotice_.clear();
    draftWarnings_.clear();
    draftError_.clear();
}

// 任务删除确认弹窗（E1）：仿 claude 删除确认（Compact 420 / 标题 / 红字正文 / 按钮对）。
// 取消/遮罩/Esc 任一路径只关弹窗不动库；确认才走 performDeleteTask
void AgentModule::composeDeleteConfirmDialog(
    ::eui::Ui& ui, const ::eui::Screen& screen, const ::core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& themeTokens) {
    const float w = ui_kit::dialogWidthPx(ui_kit::DialogWidth::Compact);
    const float h = 190.0f;

    ui_kit::DialogFrame frame;
    frame.id = "agent.dialog.delete";
    frame.width = ui_kit::DialogWidth::Compact;
    frame.height = h;
    frame.open = deleteConfirmTaskId_.has_value();
    frame.zIndex = 1100;   // 叠在任务/详情弹窗（默认 1000）之上，同草稿子弹窗
    frame.onOpenChange = [this](bool v) {
        if (!v) deleteConfirmTaskId_.reset();
    };

    ui_kit::drawDialogFrame(ui, screen, frame, themeTokens, [&] {
        if (!deleteConfirmTaskId_) return;
        // 名字现查 tasks_：待删任务已不存在（他处删了）时如实显示读取失败
        std::string name = "（读取失败）";
        for (const auto& task : tasks_) {
            if (task.id == *deleteConfirmTaskId_) {
                name = task.name;
                break;
            }
        }
        ui_kit::drawDialogTitle(ui, "agent.dialog.delete.title", w, "确认删除？",
                                ui_kit::kDialogTitleFontSize, t.colors.textPrimary, {.y = 16.0f});
        ui_kit::drawDialogText(
            ui, "agent.dialog.delete.msg", w,
            "将删除任务「" + name + "」及其全部运行记录，不可恢复。",
            t.sizes.fontSizeSmall, t.colors.errorText,
            {.y = 58.0f, .height = 60.0f, .lineHeight = 20.0f, .wrap = true});

        ui_kit::DialogButtonSpec cancel;
        cancel.label = "取消";
        cancel.fontSize = 14.0f;
        cancel.style = ui_kit::DialogButtonStyle::SecondaryThemed;
        cancel.onClick = [this] { deleteConfirmTaskId_.reset(); };
        ui_kit::DialogButtonSpec confirm;
        confirm.label = "确认删除";
        confirm.fontSize = 14.0f;
        confirm.style = ui_kit::DialogButtonStyle::DangerThemed;
        confirm.onClick = [this] {
            if (deleteConfirmTaskId_) performDeleteTask(*deleteConfirmTaskId_);
        };
        ui_kit::drawDialogButtons(ui, "agent.dialog.delete", w, h, cancel, confirm,
                                  t, themeTokens);
    });
}

// 确认后的真删除（E1）：store 单事务级联清 runs/messages；关确认弹窗与可能开着的详情
// 弹窗（详情正显示被删任务时留着会渲染空数据），刷列表。失败置页级错误行不吞
void AgentModule::performDeleteTask(int64_t taskId) {
    deleteConfirmTaskId_.reset();
    if (store_ == nullptr) return;
    // E1 二道闸（确认时刻复查）：删除 chip 的禁用态与点击闭包的 busy 都是渲染帧快照，
    // 「弹窗开着 → 确认点击」之间 run 可能已起（scheduler/事件派发不挑时机）。判据与
    // 列表 runBusy 同口径：DB running 行 || 在跑 ActiveRun 任一即挡——scheduler 起的
    // run 不进 runs_，但同样经 store 落 running 行，照挡。scheduler 派发线程不触本闸
    // 读的任何状态（runs_ 走 runsMu_、latestRuns_ 仅主线程），无跨线程读面
    {
        const auto latest = latestRuns_.find(taskId);
        const bool runBusy =
            isTaskRunActive(taskId) ||
            (latest != latestRuns_.end() && latest->second.status == "running");
        if (runBusy) {
            pageError_ = "任务正在运行，请先停止后再删除";
            LOG_WARN("Agent: 删除被拒绝（任务 " + std::to_string(taskId) +
                     " 有在跑 run）");
            return;
        }
    }
    auto r = store_->deleteTask(taskId);
    if (r.isFail()) {
        pageError_ = "删除任务失败：" + r.errorMessage();
        LOG_WARN("Agent: 删除任务失败（task " + std::to_string(taskId) +
                 "）: " + r.errorMessage());
        return;
    }
    if (detailTaskId_ == taskId) closeTaskDetail();
    pageError_.clear();
    tasksDirty_ = true;
    LOG_INFO("Agent: 任务 " + std::to_string(taskId) + " 已删除（含全部运行记录）");
}

} // namespace module::agent
