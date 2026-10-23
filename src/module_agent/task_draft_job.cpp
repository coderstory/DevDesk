// AI 草稿生成的三段主线程方法（start / reap / apply）。
// 全部为 AgentModule 成员函数，实现在此文件以降低 agent_module.cpp 体积。
// 行为零变化——仅搬迁，不改逻辑/文案/样式/间距。

#include "agent_module.h"

#include <algorithm>
#include <ctime>
#include <system_error>
#include <vector>

#include "core/logging.h"
#include "core/platform/platform.h"
#include "core/time.h"
#include "module_agent/task_draft_parse.h"
#include "module_agent/trigger_config.h"

namespace module::agent {

// 点「生成」：主线程校验输入 → 起后台线程 requestTaskDraft + parseTaskDraft。
// 生成中重复点击被挡（按钮禁用 + 这里再挡一道）；结果由 reapDraftJob 回收。
void AgentModule::startDraftGeneration() {
    {
        // 槽位读与其他调用点同一把 draftMu_（锁口径统一：不留半锁半裸的成员）
        std::lock_guard<std::mutex> lock(draftMu_);
        if (draftJob_ != nullptr) return;   // 生成中重复点击（按钮已禁用，再挡一道）
    }
    const std::string text = trimCopy(draftInput_);
    if (text.empty()) {
        draftPhase_ = DraftPhase::Failed;
        draftNotice_.clear();
        draftWarnings_.clear();
        draftError_ = "请先用一句话描述任务";
        return;
    }
    ensureLlm();
    if (llm_ == nullptr) {
        draftPhase_ = DraftPhase::Failed;
        draftNotice_.clear();
        draftWarnings_.clear();
        draftError_ = "LLM 配置不可用：请在设置页配置 Claude 后重试";
        return;
    }
    // 防御：先收上一个已完成未收割的 job（正常路径 compose 每帧已收），仍占用则退
    reapDraftJob();
    {
        std::lock_guard<std::mutex> lock(draftMu_);
        if (draftJob_ != nullptr) {
            draftPhase_ = DraftPhase::Failed;
            draftError_ = "上一次请求仍在进行，请稍候";
            return;
        }
    }
    ++draftEpoch_;
    auto job = std::make_unique<DraftJob>();
    job->input = text;
    job->modelSlot = editSlot_;
    job->epoch = draftEpoch_;
    DraftJob* raw = job.get();
    {
        std::lock_guard<std::mutex> lock(draftMu_);
        draftJob_ = std::move(job);
    }
    draftPhase_ = DraftPhase::Generating;
    draftNotice_.clear();
    draftWarnings_.clear();
    draftError_.clear();
    try {
        raw->thread = std::thread([this, raw] {
            const std::time_t now = deps_.schedulerNow ? deps_.schedulerNow()
                                                       : core::time::nowUnix();
            auto resp = requestTaskDraft(*llm_, raw->input, now, raw->modelSlot);
            if (resp.isFail()) {
                raw->errorText = resp.errorMessage();
            } else {
                auto parsed =
                    parseTaskDraft(resp.value(), registeredToolNames());
                if (parsed.isOk()) {
                    raw->parsed = std::move(parsed.value());
                    raw->ok = true;
                } else {
                    // 连 JSON 都没有：给不出可编辑骨架，按失败呈报（parse 契约）
                    raw->errorText = parsed.errorMessage();
                }
            }
            raw->done.store(true, std::memory_order_release);
            // 事件驱动的帧循环不主动重绘，完成即要帧（startTask 同款先例）
            ::core::platform::requestUiUpdate();
        });
    } catch (const std::system_error& e) {
        // 线程创建失败：槽位归还（线程未启动，无需 join），阶段回失败
        std::lock_guard<std::mutex> lock(draftMu_);
        draftJob_.reset();
        draftPhase_ = DraftPhase::Failed;
        draftError_ = "生成线程创建失败";
        LOG_ERROR(std::string("Agent: AI 草稿线程创建失败: ") + e.what());
    }
}

// 主线程收割已完成的生成线程：join → 预填表单 / 写错误行 / 丢弃过期结果。
// compose 每帧调用（worker 置 done 后 requestUiUpdate 主动要帧，不等用户输入）。
void AgentModule::reapDraftJob() {
    std::unique_ptr<DraftJob> job;
    {
        std::lock_guard<std::mutex> lock(draftMu_);
        if (draftJob_ == nullptr ||
            !draftJob_->done.load(std::memory_order_acquire)) {
            return;
        }
        job = std::move(draftJob_);
    }
    if (job->thread.joinable()) job->thread.join();
    if (!job->ok) {
        draftPhase_ = DraftPhase::Failed;
        draftError_ = job->errorText;
        return;
    }
    if (job->epoch != draftEpoch_) {
        // 弹窗曾关闭 / 已重开生成：结果按约丢弃，不改表单一个字节
        draftPhase_ = DraftPhase::Idle;
        draftError_ = "上次生成已作废（子弹窗曾关闭），请重新生成";
        return;
    }
    applyDraftPrefill(job->parsed);
    draftPhase_ = DraftPhase::Done;
}

// 把校验过的草稿填进表单：只填模型给出的可信字段（goalFilled / triggerFilled）；
// 触发不可信时一字不动。结果永不落库——保存仍是用户动作。
void AgentModule::applyDraftPrefill(const TaskDraftResult& res) {
    std::vector<std::string> filled;
    if (res.goalFilled && !res.draft.goal.empty()) {
        editGoal_ = res.draft.goal;
        filled.emplace_back("任务目标");
    }
    // W4 C3：contextNote 预填已移除（执行记忆并入 systemPrompt）
    // 触发只有「类型 + 载荷都过校验」才采信；不可信时一字不动（含用户已选的类型）
    if (res.triggerFilled) {
        const int idx = triggerIndexOf(res.draft.triggerType);
        editTriggerIdx_ = idx;
        filled.emplace_back(std::string("触发方式（") + kTriggerLabels[idx] + "）");
        const auto cfg = decodeTriggerConfig(res.draft.triggerConfig);
        if (idx == kTriggerScheduleIdx && cfg.isOk()) {
            setEditCronDraft(cfg.value().cron);
            if (cfg.value().maxRuns) editMaxRuns_ = *cfg.value().maxRuns;
        } else if (idx == kTriggerEventIdx && cfg.isOk()) {
            editEvents_ = {};
            for (int i = 0; i < kEventCount; ++i) {
                if (std::find(cfg.value().events.begin(), cfg.value().events.end(),
                              std::string(kEventNames[i])) !=
                    cfg.value().events.end()) {
                    editEvents_[i] = true;
                }
            }
            if (cfg.value().maxRuns) editMaxRuns_ = *cfg.value().maxRuns;
        } else if (idx == kTriggerOnceIdx && cfg.isOk() && cfg.value().onceAt) {
            setEditOnceDraft(formatOnceAtText(*cfg.value().onceAt));
        }
    }
    draftNotice_ = filled.empty()
                       ? std::string("模型没有给出可预填的字段，请手填或换个说法重试")
                       : "已预填表单：" + joinCnList(filled) + "；请检查后自己点「保存」";
    // 降级警告 + 模型自报的待补项 → 子弹窗警告行（不 block，用户可见哪些没吃进去）
    std::vector<std::string> warn = res.draft.warnings;
    for (const auto& ask : res.needsUserInput) warn.push_back(ask);
    draftWarnings_ = joinCnList(warn);
}

} // namespace module::agent