// 智能体模块·UI 块（AgentModule 成员函数定义）：任务弹窗渲染（composeTaskDialog）、
// AI 草稿子弹窗（composeDraftDialog + 开关）、删除确认弹窗与真删除、详情弹窗开关态
// 与消息展开态、消息行渲染（renderMessageRow）。
// 弹窗关闭态不建内容节点（存在性即开关状态，测试据此判开合）；弹窗字段标签一律
// .caption：dropdown 内部占用 <id>.label，同 id 会串位。
// 拆分（04 号票）产物：类定义在 agent_module.h，跨 TU 共享助手在
// agent_module_internal.h；本文件只承载成员函数体与 TU 私有助手，
// 纯代码搬移，逻辑与签名零变化。

#include "agent_module_internal.h"

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

// 消息行渲染：.head（徽章 + 时间 + 展开/收起 + 复制 chip，头高 24）、收起态 .preview、
// 展开态逐块 .blk.<j>；正文/预览/工具输出统一 fsS/20（spec §9.2），text 块 maxWidth
// 必须显式给（measureTextSize 只认 element.maxWidth）
void AgentModule::renderMessageRow(
    ::eui::Ui& inner, float cw, const std::string& prefix, size_t index,
    const AgentMessageRow& msg, int64_t refSec, const ::core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& tokens) {
    const float fsS = t.sizes.fontSizeSmall;
    // §9.2 消息行高下限 20：fontScale <1 时 lhS 派生值会更小，钳回 20 防拥挤
    const float msgLh = std::max(fsS * 1.4f, 20.0f);
    const std::string mid = prefix + ".msg." + std::to_string(index);
    const bool expanded = expandedMsgSeqs_.count(msg.seq) > 0;
    const float boxH = estimateMessageHeight(msg, expanded, cw, fsS, msgLh);
    const std::vector<MessageBlock> blocks =
        decodeMessageBlocks(msg.contentJson);
    const std::string badge = messageRoleBadge(msg);
    constexpr float kMsgBadgeW = 44.0f;   // 2 CJK @13 + 余量
    constexpr float kMsgTimeW = 110.0f;   // MM-DD HH:MM:SS（14 字符）@13 ≈ 96

    inner.column(mid)
        .size(cw, boxH)
        .gap(kMsgGap)
        .content([&] {
            inner.row(mid + ".head")
                .size(cw, kMsgHeaderH)
                .gap(8.0f)
                .alignItems(eui::Align::CENTER)
                .content([&] {
                    inner.text(mid + ".badge")
                        .size(kMsgBadgeW, 20.0f)
                        .text(badge)
                        .fontSize(fsS)
                        .lineHeight(20.0f)
                        .fontWeight(600)
                        .color(badge == "工具" ? tokens.primary
                                              : t.colors.textSecondary)
                        .build();
                    // 头行顺序 §10：badge → time → toggle → copy（copy 仅展开态）
                    inner.text(mid + ".time")
                        .size(kMsgTimeW, 20.0f)
                        .text(messageTimeText(msg.createdAt, refSec))
                        .fontSize(fsS)
                        .lineHeight(20.0f)
                        .wrap(false)
                        .maxWidth(kMsgTimeW)
                        .color(t.colors.textTertiary)
                        .build();
                    actionChip(inner, mid + ".toggle",
                               expanded ? "收起" : "展开", 48.0f,
                               /*primary=*/false, /*enabled=*/true, tokens,
                               [this, seq = msg.seq] {
                                   toggleMessageExpanded(seq);
                               }, fsS);
                    if (expanded) {
                        actionChip(inner, mid + ".copy", "复制", 48.0f,
                                   /*primary=*/false, /*enabled=*/true, tokens,
                                   [this, text = messagePlainText(blocks)] {
                                       // setClipboardText 返回 void 无法校验 → LOG 留痕
                                       core::window::setClipboardText(text);
                                       LOG_INFO("Agent: 已复制消息全文 " +
                                                std::to_string(text.size()) +
                                                " 字节到剪贴板");
                                       toast_.show("已复制",
                                                   "消息全文已复制到剪贴板");
                                   }, fsS);
                    }
                })
                .build();
            if (!expanded) {
                inner.text(mid + ".preview")
                    .size(cw, msgLh)
                    .text(messagePreview(msg))
                    .fontSize(fsS)
                    .lineHeight(msgLh)
.wrap(false)
                    .color(t.colors.textSecondary)
                    .onClick([this, seq = msg.seq] {
                        toggleMessageExpanded(seq);
                    })
                    .build();
                return;
            }
            if (blocks.empty()) {
                inner.text(mid + ".blk.empty")
                    .size(cw, msgLh)
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
                            .size(cw, estimateTextBlockHeight(b.text, cw, fsS,
                                                              msgLh))
                            .text(b.text)
                            .fontSize(fsS)
                            .lineHeight(msgLh)
                            .wrap(true)
                            .maxWidth(cw)
                            .color(b.type == MessageBlock::Type::Plain
                                       ? t.colors.textTertiary
                                       : t.colors.textPrimary)
                            .build();
                        break;
                    }
                    case MessageBlock::Type::ToolUse: {
                        inner.text(bid)
                            .size(cw, 20.0f)
                            .text("工具 · " + b.toolName + "  " +
                                  toolArgsSummary(b.text, 120))
                            .fontSize(fsS)
                            .lineHeight(20.0f)
                            .wrap(false)
                            .color(tokens.primary)
                            .build();
                        break;
                    }
                    case MessageBlock::Type::ToolResult: {
                        inner.text(bid)
                            .size(cw, msgLh)
                            .text(b.isError ? "工具输出（错误）" : "工具输出")
                            .fontSize(fsS)
                            .lineHeight(msgLh)
                            .color(b.isError ? t.colors.errorText
                                             : t.colors.textTertiary)
                            .build();
                        inner.text(bid + ".out")
                            .size(cw, estimateTextBlockHeight(b.text, cw, fsS,
                                                              msgLh))
                            .text(b.text)
                            .fontSize(fsS)
                            .lineHeight(msgLh)
                            .wrap(true)
                            .maxWidth(cw)
                            .color(b.isError ? t.colors.errorText
                                             : t.colors.textSecondary)
                            .build();
                        break;
                    }
                }
            }
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
                                    20.0f, t.colors.textPrimary, {.y = 8.0f});
            ui.rect("agent.dialog.task.titlebar.edge")
                .position(0.0f, kContentY - 1.0f)
                .size(w, 1.0f)
                .color(t.colors.borderDefault)
                .build();

            // ✕ 关闭（带内垂直居中）
            ui.text("agent.dialog.task.close")
                .position(w - 56.0f, 8.0f)
                .size(28.0f, 28.0f)
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
                constexpr float kVertNumD = 22.0f;   // 序号圆直径（r=11）
                constexpr const char* kStepSubs[] = {
                    "任务名称", "六类 + cron", "目标 / 系统提示词", "模型 / 限次 / 安全组"};
                for (int i = 0; i < kStepCount; ++i) {
                    std::string nodeId = "agent.dialog.task.step." + std::to_string(i);
                    const float navY = kContentY + static_cast<float>(i) * kVertNavItemH;
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
                        .size(kVertNavW - 44.0f - 14.0f, 18.0f)
                        .text(kStepLabels[i])
                        .fontSize(core::theme::scaled(14.0f))
                        .lineHeight(core::theme::scaled(18.0f))
                        .fontWeight(600)
                        .wrap(false)
                        .color(itemFg)
                        .build();
                    ui.text(nodeId + ".sub")
                        .position(44.0f, navY + 30.0f)
                        .size(kVertNavW - 44.0f - 14.0f, 14.0f)
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
                }

                // --- Step 1: 触发 ---
                if (currentStep_ == 1) {
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
                                        .radius(8.0f)
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
                                items.reserve(kCronTemplateCount + 1);
                                items.emplace_back("选择模板…");
                                for (int i = 0; i < kCronTemplateCount; ++i)
                                    items.emplace_back(kCronTemplates[i].label);
                                components::dropdown(ui, "agent.dialog.task.cron.template")
                                    .size(tplW, kDialogRowH)
                                    .theme(themeTokens)
                                    .items(std::move(items))
                                    .selected(0)
                                    .open(editCronTemplateOpen_)
                                    .onOpenChange([this](bool v) { editCronTemplateOpen_ = v; })
                                    .onChange([this](int idx) {
                                        if (idx >= 1 && idx <= kCronTemplateCount)
                                            setEditCronDraft(kCronTemplates[idx - 1].expr);
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
                                .radius(16.0f)
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
                                           {.x = x0, .y = hintY, .height = 12.0f,
                                            .lineHeight = 12.0f, .skipIfEmpty = true});
                    ui_kit::drawDialogText(ui, "agent.dialog.task.cron.preview", w,
                                           cronPreview_,
                                           t.sizes.fontSizeSmall,
                                           cronPreviewIsError_ ? t.colors.errorText
                                                               : t.colors.textTertiary,
                                           {.x = x0, .y = hintY, .height = 12.0f,
                                            .lineHeight = 12.0f, .skipIfEmpty = true});
                    ui_kit::drawDialogText(ui, "agent.dialog.task.once.preview", w,
                                           oncePreview_,
                                           t.sizes.fontSizeSmall,
                                           oncePreviewIsError_ ? t.colors.errorText
                                                               : t.colors.textTertiary,
                                           {.x = x0, .y = hintY, .height = 12.0f,
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
                            .radius(8.0f)
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
                    ui_kit::drawDialogText(
                        ui, "agent.dialog.task.meta.hint", w,
                        "迭代＝单次运行的最多轮数，跑满以「受限」结束、不再重试；"
                        "破坏性操作＝允许删除文件等高危工具，不勾则被拒",
                        t.sizes.fontSizeSmall, t.colors.textTertiary,
                        {.x = x0, .y = hintY, .height = 12.0f, .lineHeight = 12.0f});

                    // 安全组
                    const float safeY = hintY + 18.0f;
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
                                .size(20.0f, 32.0f)
                                .text("⚠")
                                .fontSize(core::theme::scaled(14.0f))
                                .lineHeight(core::theme::scaled(32.0f))
                                .verticalAlign(eui::VerticalAlign::Center)
                                .color(t.colors.errorText)
                                .build();
                        }
                        ui.text("agent.dialog.task.destructive.label")
                            .position(x0 + (editDestructive_ ? 24.0f : 0.0f), destructiveY)
                            .size(160.0f, 32.0f)
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
                        // 自动记录开关已移除（W4 C3）：行为固定为开启，字段五处清链
                    }
                }
            }

            // ====== 试运行结果行（W4 C1：移除去全局错误行，出错标节点不弹全局错误） ======
            // 锚内容区底缘内侧（底栏上方 18px），新竖向案内容区与底栏间无过渡带
            ui_kit::drawDialogText(
                ui, "agent.dialog.task.test.result", w, testRunResult_,
                t.sizes.fontSizeSmall, t.colors.success,
                {.x = x0, .y = h - kContentY - 18.0f, .height = 14.0f,
                 .lineHeight = 14.0f, .skipIfEmpty = true});

            // ====== 底部按钮区（W4 C1 重做：40px 高、8px 间距、居中文字；按钮顺序从右到左：下一步 Filled 最右 → 上一步 Outline → 测试 Outline → 取消 Ghost 最左） ======
            const float btnY = h - kContentY;   // 底栏带 (164, H-44) 高 44，按钮右对齐
            const float btnH = 40.0f;
            const float bw = 96.0f;
            const float testW = 112.0f;
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
                .radius(8.0f)
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
                    .radius(8.0f)
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
                    .radius(8.0f)
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
                .radius(8.0f)
                .border(0.0f, t.colors.borderDefault)
                .onClick([this] { closeTaskDialog(); })
                .build();
        })
        .build();
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
                                "AI 生成草稿", 20.0f,
                                t.colors.textPrimary, {.y = 16.0f});

        // ✕ 关闭（与遮罩 / Esc / 「关闭」按钮同一路径）
        ui.text("agent.dialog.task.draft.close")
            .position(w - 56.0f, 22.0f)
            .size(28.0f, 28.0f)
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
                                20.0f, t.colors.textPrimary, {.y = 16.0f});
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
