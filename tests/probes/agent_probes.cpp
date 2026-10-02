// tests/probes/agent_probes.cpp — 智能体页 probe（真窗口 + 真 Runtime 帧循环）
//
// 覆盖（与无头 test_agent_ui.cpp 互补——probe 负责真命中分发与帧内几何）：
//   ① 页面骨架（2026-10-02 批次 A1）：agent.page 铺满逻辑宽 / agent.title /
//      agent.page.newTask / agent.tasklist 是 scrollView 节点（有 .content 列）；
//   ② 任务行 + 行内「启动」：按钮 frame 完全落在任务行内；
//   ③ 点「启动」中心（真指针 press+release，下一帧 composeFrame 消费）→
//      fakeProxy 脚本化 end_turn → run 落库 completed → 状态列「成功」；
//      且详情弹窗自动打开、最终 assistant 文本块默认展开（回复立即可见）；
//   ④ 任务弹窗（编辑态）：点行内「编辑」→ agent.dialog.task 出现、**设计尺寸
//      760x400**、字段行底 ≤ 按钮行顶、按钮行不溢面板底、全字段节点在、
//      schedule 任务渲染 cron 输入；再真指针点开触发方式下拉 → 弹层最后一项
//      完整在屏内且不挤动下方行，收起后点取消关窗（复审整改：原「面板不超
//      视口」被 dialog 组件自身 clamp 兜住、零判别力，已换成上面这些可失败的
//      尺寸/预算断言）；
//   ⑤ 详情弹窗：**设计尺寸 760x660**、配置摘要 / run 列表（倒序 + 最近一次
//      高亮）/ 消息滚动区底 ≤ 面板底（同样是可失败的预算断言）；
//   ⑥ 任务列表滚动：种 30 条任务 → 内容高 > 视口高 → 出 .scroll 滚动条；
//   ⑦ 弹窗打开时列表首行仍可见（列表没被弹窗挤掉 / 没被遮罩层吞掉）；
//   ⑧ id 唯一性：走 orderedRoots 全树，断言全帧无重复 id（T10 id 撞车教训）；
//   ⑨ 可用工具区块：点 agent.tools.header → 卡片数 == 注册表工具数。
//
// 断言机制：probe 无几何断言 helper，全部手搓 check() + nearEq()
//（about_probe.cpp:60,94-99 口径）。收尾顺序照 about_probe.cpp:449-453。
#include <GLFW/glfw3.h>  // 必须先于 harness：让 GLFW 默认拉平台 GL 头（claude_probes 同款）
#include "probes/probe_harness.h"
#include "test_support.h"

#include "core/module_interface.h"
#include "module_agent/agent_module.h"
#include "module_agent/agent_store.h"
#include "platform/http_client.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

// fake 代理服务（test_agent_tools.cpp / test_llm_client.cpp 同款全接口桩）
class FakeProxy : public core::IClaudeProxyService {
public:
    bool isProxyRunning() const override { return true; }
    int proxyPort() const override { return 8787; }
    void setConfiguredProxyPort(int) override {}
    void stopProxy() override {}
    core::Result<int> startProxy(const std::string&, int) override {
        return core::Result<int>::fail(core::ErrorCode::Internal, "unused");
    }
    bool isClaudeCodeManaged() const override { return false; }
    // F2：接口改按值返回快照（M2 静态审计），桩同口径
    std::vector<std::string> lastSyncWarnings() const override { return {}; }
    std::string settingsFilePath() const override { return {}; }
    std::optional<std::string> resolveModelSlot(const std::string&) override {
        return std::string("claude-sonnet-5-5");
    }
};

bool nearEq(float a, float b) { return std::fabs(a - b) <= 0.5f; }

// inner 完全落在 outer 内（0.5 容差，与 nearEq 同口径）
bool frameContains(const core::dsl::Element& outer, const core::dsl::Element& inner) {
    return inner.frame.x >= outer.frame.x - 0.5f &&
           inner.frame.y >= outer.frame.y - 0.5f &&
           inner.frame.x + inner.frame.width <=
               outer.frame.x + outer.frame.width + 0.5f &&
           inner.frame.y + inner.frame.height <=
               outer.frame.y + outer.frame.height + 0.5f;
}

// 元素完全落在 w×h 视口内（0.5 容差）
bool insideViewport(const core::dsl::Element& e, float w, float h) {
    return e.frame.x >= -0.5f && e.frame.y >= -0.5f &&
           e.frame.x + e.frame.width <= w + 0.5f &&
           e.frame.y + e.frame.height <= h + 0.5f;
}

std::string frameStr(const core::dsl::Element* e) {
    return "(" + std::to_string(e->frame.x) + ", " + std::to_string(e->frame.y) +
           ", " + std::to_string(e->frame.width) + "x" +
           std::to_string(e->frame.height) + ")";
}

// 点击元素中心（真实指针 press+release 成对排队，下一帧消费）
void clickCenter(core::window::Handle win, const core::dsl::Element* e) {
    const float cx = e->frame.x + e->frame.width * 0.5f;
    const float cy = e->frame.y + e->frame.height * 0.5f;
    core::queuePointerButton(win, cx, cy, core::PointerButton::Left,
                             core::PointerAction::Press, {});
    core::queuePointerButton(win, cx, cy, core::PointerButton::Left,
                             core::PointerAction::Release, {});
}

// 全树收集 id（⑧ id 唯一性扫描）：Element::children 公开，递归走
void collectIds(const core::dsl::Element& e,
                std::map<std::string, int>& counts) {
    ++counts[e.id];
    for (const auto& child : e.children) {
        if (child) collectIds(*child, counts);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("agent_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    // 数据底座 + 配置隔离（home_probes.cpp:126-136 / recipe §3 同款）
    auto cfg = test_support::tempConfigFile("agentprobe_", "cfg");
    test_support::ScopedConfigPath scopedCfg(cfg);
    test_support::TestEnv env("agentprobe_", "agent", true);

    // fake 依赖 + 脚本化 HTTP（end_turn 一轮即完成，零真实网络）
    FakeProxy proxy;
    module::agent::AgentModule::Deps deps;
    deps.db = &env.handle();
    deps.proxy = &proxy;
    deps.http = [](const platform::http::Request&) {
        platform::http::Response res;
        res.statusCode = 200;
        res.body = R"({"id":"m1","stop_reason":"end_turn","content":[)"
                   R"({"type":"text","text":"最终答复"}],)"
                   R"("usage":{"input_tokens":1,"output_tokens":1}})";
        return core::Result<platform::http::Response>::ok(res);
    };
    auto mod = std::make_unique<module::agent::AgentModule>(deps);
    mod->onStart();

    module::agent::AgentStore store(env.handle());
    // 首条任务 = 列表首行（②⑦ 的锚点）+ 真跑一次 run（③）
    auto created = store.createTask("探针任务", "直接回复完成，不调用任何工具",
                                    "manual", "default", 10, false);
    if (created.isFail()) {
        std::cerr << "agent_probes: seed task failed: "
                  << created.errorMessage() << "\n";
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        probe::terminateWindowBackend();
        return 1;
    }
    const int64_t firstTaskId = created.value();
    const std::string tid = std::to_string(firstTaskId);

    // ④ 用：schedule 任务 + 两次 run（一次 completed，一次 limited 带 error/token）
    // ——两次 run 都在 onStart 之后种（否则被 sweepStaleRuns 清成 failed）
    bool schedSeeded = false;
    std::string sid;
    {
        auto sched = store.createTask("定时任务", "直接回复完成，不调用任何工具",
                                      "schedule", "sonnet", 20, false,
                                      R"({"cron":"0 9 * * 1-5"})");
        if (sched.isOk()) {
            sid = std::to_string(sched.value());
            auto sr1 = store.startRun(sched.value());
            auto sr2 = store.startRun(sched.value());
            if (sr1.isOk() && sr2.isOk()) {
                store.finishRun(sr1.value(), "limited", 7, int64_t(190000),
                                int64_t(15000),
                                std::string("token 累计超上限 200K"));
                store.finishRun(sr2.value(), "completed", 1, std::nullopt,
                                std::nullopt, std::nullopt);
                schedSeeded = true;
            }
        }
    }
    if (!schedSeeded) {
        std::cerr << "agent_probes: schedule 任务/run 种子失败\n";
    }

    // ⑥ 用：补到 30 条任务 → 列表内容高远超视口 → 必出滚动条
    for (int i = 0; i < 28; ++i) {
        store.createTask("填充任务 " + std::to_string(i), "g", "manual", "default",
                         10, false);
    }

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    const std::string pageId = "agent";
    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    core::dsl::Ui* ui = nullptr;
    auto composeFrame = [&]() {
        probe::composeModuleFrame(runtime, window, pageId, w, h,
                                  [&](eui::Ui& u, const eui::Screen& screen) {
                                      ui = &u;
                                      mod->compose(u, screen);
                                  });
    };

    bool passed = true;
    auto check = [&](bool cond, const std::string& what) {
        if (!cond) {
            std::cerr << "agent_probes: FAIL " << what << "\n";
            passed = false;
        }
    };

    composeFrame();

    // ---- ① 页面骨架：存在性 + agent.page 铺满逻辑宽 + 列表是 scrollView ----
    const auto* page = ui != nullptr ? ui->find("agent.page") : nullptr;
    if (page == nullptr) {
        std::cerr << "agent_probes: agent.page 不存在\n";
        passed = false;
    } else {
        check(nearEq(page->frame.width, w),
              "agent.page 宽 " + frameStr(page) + " 期望宽 " + std::to_string(w));
    }
    check(ui != nullptr && ui->find("agent.title") != nullptr,
          "agent.title 不存在");
    check(ui != nullptr && ui->find("agent.page.newTask") != nullptr,
          "agent.page.newTask（+ 新建任务）不存在");
    // 旧页内新建表单已删
    check(ui != nullptr && ui->find("agent.form") == nullptr,
          "旧页内新建表单 agent.form 仍在");
    check(ui != nullptr && ui->find("agent.tasklist.content") != nullptr,
          "agent.tasklist 不是 scrollView（缺 .content 内容列）");

    // ---- ② 首行任务 + 「启动」按钮：frame 完全落在行内 ----
    const auto* row = ui != nullptr ? ui->find("agent.task." + tid) : nullptr;
    if (row == nullptr) {
        std::cerr << "agent_probes: agent.task." << tid << " 不存在\n";
        passed = false;
    } else {
        const auto* runBtn = ui->find("agent.task." + tid + ".run");
        if (runBtn == nullptr) {
            std::cerr << "agent_probes: agent.task." << tid << ".run 不存在\n";
            passed = false;
        } else {
            check(frameContains(*row, *runBtn),
                  "run 未完全落在任务行内 run" + frameStr(runBtn) + " row" +
                      frameStr(row));
        }
    }

    // ---- ③ 点「启动」中心：真实命中分发 → run 落库 + 详情弹窗自动打开 ----
    const auto* runBtn = ui != nullptr ? ui->find("agent.task." + tid + ".run")
                                       : nullptr;
    if (runBtn == nullptr) {
        std::cerr << "agent_probes: 点击目标 agent.task." << tid
                  << ".run 不存在\n";
        passed = false;
    } else {
        clickCenter(window, runBtn);   // press + release 同帧排队，下一帧消费
        composeFrame();

        bool completed = false;
        for (int i = 0; i < 500; ++i) {
            auto runs = store.listRuns(firstTaskId, 1);
            if (runs.isOk() && !runs.value().empty() &&
                runs.value().front().status == "completed") {
                completed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(completed, "点击 run 后 5s 内未落库 completed（命中分发没走通？）");

        composeFrame();
        const auto* status = ui->find("agent.task." + tid + ".status");
        check(status != nullptr, "agent.task." + tid + ".status 不存在");
        if (status != nullptr) {
            check(status->text.find("成功") != std::string::npos,
                  "状态列实测「" + status->text + "」期望含「成功」");
        }
        // 启动即开详情弹窗，且最终 assistant 文本块默认展开（回复立即可见）
        const auto* detail = ui->find("agent.dialog.detail");
        check(detail != nullptr, "点启动后详情弹窗未自动打开");
        const auto* finalBlk = ui->find("agent.dialog.detail.msg.1.blk.0");
        check(finalBlk != nullptr,
              "详情弹窗缺默认展开的最终 assistant 文本块 msg.1.blk.0");
        if (finalBlk != nullptr) {
            check(finalBlk->text.find("最终答复") != std::string::npos,
                  "最终回复内容未渲染，实测: " + finalBlk->text);
        }
        // 弹窗几何（评审口径：被 dialog 组件 clamp 兜住的「不超出视口」零判别力
        // → 钉我们自己的设计尺寸与内部高度预算）
        const auto* dpanel = ui->find("agent.dialog.detail.panel");
        check(dpanel != nullptr && nearEq(dpanel->frame.width, 760.0f),
              "详情弹窗宽应为设计值 760，实测 " +
                  (dpanel ? frameStr(dpanel) : std::string("（缺 panel）")));
        check(dpanel != nullptr && nearEq(dpanel->frame.height, 660.0f),
              "详情弹窗高应为设计值 660，实测 " +
                  (dpanel ? frameStr(dpanel) : std::string("（缺 panel）")));
        check(dpanel != nullptr && insideViewport(*dpanel, w, h),
              "详情弹窗未完整落在视口内（尺寸断言之外的兜底）");
        if (dpanel != nullptr) {
            const auto* scrollNow = ui->find("agent.dialog.detail.msgscroll");
            check(scrollNow != nullptr &&
                      scrollNow->frame.y + scrollNow->frame.height <=
                          dpanel->frame.y + dpanel->frame.height - 19.5f,
                  "消息滚动区溢出面板底（底边距应收 20）");
            const auto* rl = ui->find("agent.dialog.detail.runlist");
            const auto* mt = ui->find("agent.dialog.detail.msgs.title");
            check(rl != nullptr && mt != nullptr &&
                      rl->frame.y + rl->frame.height <= mt->frame.y + 0.5f,
                  "run 列表压到消息标题");
        }
        // ⑦ 弹窗打开时列表首行仍可见（列表在弹窗层之下照常布局）
        const auto* rowUnderDialog = ui->find("agent.task." + tid);
        check(rowUnderDialog != nullptr && rowUnderDialog->frame.width > 0.0f &&
                  rowUnderDialog->frame.height > 0.0f,
              "弹窗打开后列表首行不可见/零尺寸");
        if (rowUnderDialog != nullptr && page != nullptr) {
            const auto* pageNow = ui->find("agent.page");
            if (pageNow != nullptr) {
                check(frameContains(*pageNow, *rowUnderDialog),
                      "弹窗打开后列表首行跑到页面外 " + frameStr(rowUnderDialog));
            }
        }

        // ---- ⑤ 详情弹窗三段：配置摘要 / run 列表 / 消息滚动区 ----
        const auto* info = ui->find("agent.dialog.detail.info");
        check(info != nullptr && info->text.find("manual") != std::string::npos,
              "详情弹窗配置摘要缺失或触发行不对");
        const auto* msgscroll = ui->find("agent.dialog.detail.msgscroll");
        check(msgscroll != nullptr && msgscroll->frame.width > 0.0f &&
                  msgscroll->frame.height > 0.0f,
              "详情弹窗消息滚动区缺失/零尺寸");

        // 关详情弹窗（真指针点 ✕），下面④要操作页面上的「编辑」
        const auto* dclose = ui->find("agent.dialog.detail.close");
        if (dclose == nullptr) {
            check(false, "详情弹窗缺 ✕ 关闭面 agent.dialog.detail.close");
        } else {
            clickCenter(window, dclose);
            composeFrame();
            check(ui->find("agent.dialog.detail.msgscroll") == nullptr,
                  "点 ✕ 后详情弹窗未关闭");
        }
    }

    // ---- ④ 任务弹窗（编辑态）：真实指针点行内「编辑」----
    if (!schedSeeded) {
        passed = false;
    } else {
        const auto* editBtn = ui != nullptr ? ui->find("agent.task." + sid + ".edit")
                                            : nullptr;
        if (editBtn == nullptr) {
            std::cerr << "agent_probes: schedule 任务行缺 edit 按钮\n";
            passed = false;
        } else {
            clickCenter(window, editBtn);   // 元素指针跨帧失效 → 重新 find
            composeFrame();
            const auto* panel = ui->find("agent.dialog.task.panel");
            const auto* goal = ui->find("agent.dialog.task.goal");
            if (panel == nullptr || goal == nullptr) {
                std::cerr << "agent_probes: 点 edit 后任务弹窗未打开\n";
                passed = false;
            } else {
                // 设计尺寸 + 内部预算（不是被组件 clamp 兜住的「不超视口」）
                check(nearEq(panel->frame.width, 760.0f),
                      "任务弹窗宽应为设计值 760，实测 " + frameStr(panel));
                check(nearEq(panel->frame.height, 400.0f),
                      "任务弹窗高应为设计值 400，实测 " + frameStr(panel));
                check(insideViewport(*panel, w, h),
                      "任务弹窗未完整落在视口内 " + frameStr(panel));
                check(goal->frame.height >= 90.0f,
                      "任务弹窗 goal 输入应为多行高 ≥90，实测 " + frameStr(goal));
                check(ui->find("agent.dialog.task.trigger") != nullptr,
                      "任务弹窗缺触发方式下拉");
                check(ui->find("agent.dialog.task.slot") != nullptr,
                      "任务弹窗缺模型槽位下拉");
                check(ui->find("agent.dialog.task.iter") != nullptr,
                      "任务弹窗缺迭代上限输入");
                check(ui->find("agent.dialog.task.destructive") != nullptr,
                      "任务弹窗缺破坏性开关");
                check(ui->find("agent.dialog.task.cancel") != nullptr &&
                          ui->find("agent.dialog.task.save") != nullptr,
                      "任务弹窗缺取消/保存按钮");
                check(ui->find("agent.dialog.task.cron") != nullptr,
                      "schedule 任务的弹窗未渲染 cron 输入");
                // 行底 ≤ 按钮行顶（行高改动 / 弹窗高改小即红）
                const auto* saveBtn = ui->find("agent.dialog.task.save");
                const auto* metaWrap =
                    ui->find("agent.dialog.task.destructive.wrap");
                check(saveBtn != nullptr && metaWrap != nullptr &&
                          metaWrap->frame.y + metaWrap->frame.height <=
                              saveBtn->frame.y + 0.5f,
                      "任务弹窗字段行压到按钮行");
                check(saveBtn != nullptr &&
                          saveBtn->frame.y + saveBtn->frame.height <=
                              panel->frame.y + panel->frame.height - 19.5f,
                      "任务弹窗按钮行溢出面板底（底边距应收 20）");

                // 下拉展开态（评审点 5）：真指针点字段 → 弹层最后一项完整在屏内，
                // 且覆盖层不挤动下方行；再点一次收起
                const auto* triggerField =
                    ui->find("agent.dialog.task.trigger.field");
                const float metaYBefore =
                    metaWrap != nullptr ? metaWrap->frame.y : 0.0f;
                if (triggerField == nullptr) {
                    check(false, "触发方式下拉字段命中面缺失");
                } else {
                    clickCenter(window, triggerField);
                    composeFrame();
                    const auto* lastItem =
                        ui->find("agent.dialog.task.trigger.item.2");
                    check(lastItem != nullptr && lastItem->frame.width > 0.0f &&
                              lastItem->frame.height > 0.0f,
                          "下拉展开后最后一项缺失/零尺寸");
                    if (lastItem != nullptr) {
                        check(insideViewport(*lastItem, w, h),
                              "下拉最后一项跑出屏外 " + frameStr(lastItem));
                    }
                    const auto* metaAfter =
                        ui->find("agent.dialog.task.destructive.wrap");
                    check(metaAfter != nullptr &&
                              nearEq(metaAfter->frame.y, metaYBefore),
                          "下拉展开挤动了下方行（展开前 " +
                              std::to_string(metaYBefore) + " 展开后 " +
                              (metaAfter ? std::to_string(metaAfter->frame.y)
                                         : std::string("（缺行）")) + "）");
                    const auto* fieldNow =
                        ui->find("agent.dialog.task.trigger.field");
                    if (fieldNow != nullptr) {
                        clickCenter(window, fieldNow);   // 收起弹层
                        composeFrame();
                    }
                }

                // 关弹窗（真指针点取消），恢复页面态
                const auto* cancel = ui->find("agent.dialog.task.cancel");
                if (cancel != nullptr) {
                    clickCenter(window, cancel);
                    composeFrame();
                    check(ui->find("agent.dialog.task.goal") == nullptr,
                          "点取消后任务弹窗未关闭");
                }
            }
        }
    }

    // ---- ⑥ 任务列表滚动：30 条任务 → 内容高 > 视口高 → 出滚动条 ----
    {
        const auto* list = ui->find("agent.tasklist");
        const auto* content = ui->find("agent.tasklist.content");
        const auto* bar = ui->find("agent.tasklist.scroll");
        if (list == nullptr || content == nullptr) {
            check(false, "任务列表 scrollView 节点缺失");
        } else {
            check(content->frame.height > list->frame.height + 1.0f,
                  "列表内容高 " + std::to_string(content->frame.height) +
                      " 未超过视口高 " + std::to_string(list->frame.height) +
                      "（种的任务不够多？）");
            check(bar != nullptr && bar->frame.height > 0.0f,
                  "内容超高时未出现滚动条 agent.tasklist.scroll");
        }
    }

    // ---- ⑧ 全帧 id 唯一性（当前帧 = 页面 + 已关弹窗态）----
    {
        std::map<std::string, int> counts;
        for (const auto& root : ui->orderedRoots()) {
            if (root) collectIds(*root, counts);
        }
        size_t duplicated = 0;
        std::string firstDup;
        for (const auto& [key, n] : counts) {
            if (n > 1) {
                ++duplicated;
                if (firstDup.empty()) firstDup = key;
            }
        }
        check(duplicated == 0, "⑧ 全帧发现重复 id " + std::to_string(duplicated) +
                                   " 个，首个: " + firstDup);
    }

    // ---- ⑨ 可用工具区块：收起默认 → 点头部展开 → 卡片数 == 注册表数 ----
    {
        const size_t registered = core::ToolRegistry::instance().listTools().size();
        check(registered >= 1, "⑨ 注册表为空（app.stats 应已注册）");
        const auto* toolsHeader = ui->find("agent.tools.header");
        if (toolsHeader == nullptr) {
            check(false, "⑨ agent.tools.header 缺失");
        } else {
            const auto* toolsLabel = ui->find("agent.tools.header.label");
            check(toolsLabel != nullptr &&
                      toolsLabel->text.find("可用工具") != std::string::npos &&
                      toolsLabel->text.find(std::to_string(registered)) !=
                          std::string::npos,
                  "⑨ 头部缺「可用工具（N）」计数，实测: " +
                      (toolsLabel ? toolsLabel->text : std::string("（无）")));
            check(ui->find("agent.tools.card.0") == nullptr,
                  "⑨ 收起态不应有工具卡");
            clickCenter(window, toolsHeader);
            composeFrame();
            size_t cards = 0;
            for (size_t k = 0; k < registered + 4; ++k) {
                if (ui->find("agent.tools.card." + std::to_string(k)) != nullptr) {
                    ++cards;
                }
            }
            check(cards == registered,
                  "⑨ 工具卡数 " + std::to_string(cards) + " != 注册表数 " +
                      std::to_string(registered));
        }
    }

    mod->onStop();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!passed) {
        std::cerr << "FAIL\n";
    } else {
        std::cout << "agent_probes: PASS（" << args.windowW << "x"
                  << args.windowH << "）\n";
    }
    return passed ? 0 : 1;
}
