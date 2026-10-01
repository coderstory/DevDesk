// tests/probes/agent_probes.cpp — 智能体页 probe（真窗口 + 真 Runtime 帧循环）
//
// 覆盖（与无头 test_agent_ui.cpp 互补——probe 负责真命中分发与帧内几何）：
//   ① 页面骨架：agent.page 存在且 frame.width == 窗口逻辑宽（nearEq 0.5），
//      agent.title / agent.form.goal / agent.form.create 存在；
//   ② 种 1 条任务 → agent.task.<id>.run 存在，且 frame 完全落在
//      agent.task.<id> 行内（手搓包含判断，0.5 容差）；
//   ③ 点击 run 中心（core::queuePointerButton press+release 成对排队，下一帧
//      composeFrame 的 update 消费——claude_probes.cpp:169-182 同款真实命中
//      路径）→ fakeProxy 脚本化 end_turn → 有界轮询 run 落库 completed →
//      composeFrame 后状态列显示「成功」。
//   ④ M2 Task 11：schedule 任务（种 2 次 run，completed + limited/error）——
//      点任务行展开 → runlist 容器与两行存在且帧非零宽高、行文案含状态中文与
//      token 合计、错误行含 error 原文；再点 agent.task.<id>.edit →
//      agent.edit.form 出现且帧完全落在页面内容区内（frameContains），
//      schedule 任务渲染 agent.edit.cron。
//
// 断言机制：probe 无几何断言 helper，全部手搓 check() + nearEq()
// （about_probe.cpp:60,94-99 口径）。收尾顺序照 about_probe.cpp:449-453。
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

std::string frameStr(const core::dsl::Element* e) {
    return "(" + std::to_string(e->frame.x) + ", " + std::to_string(e->frame.y) +
           ", " + std::to_string(e->frame.width) + "x" +
           std::to_string(e->frame.height) + ")";
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
                   R"({"type":"text","text":"done"}],)"
                   R"("usage":{"input_tokens":1,"output_tokens":1}})";
        return core::Result<platform::http::Response>::ok(res);
    };
    auto mod = std::make_unique<module::agent::AgentModule>(deps);
    mod->onStart();

    // 种 1 条任务（fresh 库 → 首个 id；id 字符串进元素 id 契约）
    module::agent::AgentStore store(env.handle());
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
    const std::string tid = std::to_string(created.value());

    // ④ 用：schedule 任务 + 两次 run（一次 completed，一次 limited 带 error/token）
    // ——两次 run 都在 onStart 之后种（否则被 sweepStaleRuns 清成 failed）
    auto sched = store.createTask("定时任务", "直接回复完成，不调用任何工具",
                                  "schedule", "default", 10, false,
                                  R"({"cron":"0 9 * * 1-5"})");
    bool schedSeeded = sched.isOk();
    std::string sid;
    if (schedSeeded) {
        sid = std::to_string(sched.value());
        auto sr1 = store.startRun(sched.value());
        auto sr2 = store.startRun(sched.value());
        if (sr1.isOk() && sr2.isOk()) {
            store.finishRun(sr1.value(), "limited", 7, int64_t(190000),
                            int64_t(15000), std::string("token 累计超上限 200K"));
            store.finishRun(sr2.value(), "completed", 1, std::nullopt,
                            std::nullopt, std::nullopt);
        } else {
            schedSeeded = false;
        }
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

    // ---- ① 页面骨架：存在性 + agent.page 铺满逻辑宽 ----
    const auto* page = ui != nullptr ? ui->find("agent.page") : nullptr;
    if (page == nullptr) {
        std::cerr << "agent_probes: agent.page 不存在\n";
        passed = false;
    } else {
        check(nearEq(page->frame.width, w),
              "agent.page 宽 " + frameStr(page) + " 期望宽 " +
                  std::to_string(w));
    }
    check(ui != nullptr && ui->find("agent.title") != nullptr,
          "agent.title 不存在");
    check(ui != nullptr && ui->find("agent.form.goal") != nullptr,
          "agent.form.goal 不存在");
    check(ui != nullptr && ui->find("agent.form.create") != nullptr,
          "agent.form.create 不存在");

    // ---- ② 任务行 + run 按钮：存在 + frame 完全落在行内 ----
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

    // ---- ③ 点击 run 中心：真实命中分发 → run 落库 ----
    const auto* runBtn = ui != nullptr ? ui->find("agent.task." + tid + ".run")
                                       : nullptr;
    if (runBtn == nullptr) {
        std::cerr << "agent_probes: 点击目标 agent.task." << tid
                  << ".run 不存在\n";
        passed = false;
    } else {
        // press + release 同帧排队，下一 composeFrame 内 update 消费
        //（claude_probes.cpp:171-176 同款；指针坐标 = 窗口坐标，probe 窗口
        // dpiScale=1 与逻辑坐标同值）
        const float cx = runBtn->frame.x + runBtn->frame.width * 0.5f;
        const float cy = runBtn->frame.y + runBtn->frame.height * 0.5f;
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Press, {});
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Release, {});
        composeFrame();

        // 有界轮询（10ms 步进 ≤5s）等 run 线程落库 completed
        bool completed = false;
        for (int i = 0; i < 500; ++i) {
            auto runs = store.listRuns(created.value(), 1);
            if (runs.isOk() && !runs.value().empty() &&
                runs.value().front().status == "completed") {
                completed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(completed, "点击 run 后 5s 内未落库 completed（命中分发没走通？）");

        // recompose 后状态列含「成功」（completed → 成功 映射）
        composeFrame();
        const auto* status = ui->find("agent.task." + tid + ".status");
        check(status != nullptr, "agent.task." + tid + ".status 不存在");
        if (status != nullptr) {
            check(status->text.find("成功") != std::string::npos,
                  "状态列实测「" + status->text + "」期望含「成功」");
        }
    }

    // ---- ④ 编辑入口 + run 列表（Task 11）：真实指针点击路径 ----
    if (!schedSeeded) {
        std::cerr << "agent_probes: schedule 任务/run 种子失败\n";
        passed = false;
    } else {
        const auto* schedRow = ui != nullptr ? ui->find("agent.task." + sid) : nullptr;
        const auto* schedTitle =
            ui != nullptr ? ui->find("agent.task." + sid + ".title") : nullptr;
        const auto* editBtn =
            ui != nullptr ? ui->find("agent.task." + sid + ".edit") : nullptr;
        if (schedRow == nullptr || schedTitle == nullptr || editBtn == nullptr) {
            std::cerr << "agent_probes: schedule 任务行/标题/edit 按钮缺失\n";
            passed = false;
        } else {
            check(frameContains(*schedRow, *editBtn),
                  "edit 未完全落在任务行内 edit" + frameStr(editBtn) + " row" +
                      frameStr(schedRow));
            // 点标题中心展开（标题元素不带 onClick，命中落到任务行本体）
            const float tx = schedTitle->frame.x + schedTitle->frame.width * 0.5f;
            const float ty = schedTitle->frame.y + schedTitle->frame.height * 0.5f;
            core::queuePointerButton(window, tx, ty, core::PointerButton::Left,
                                     core::PointerAction::Press, {});
            core::queuePointerButton(window, tx, ty, core::PointerButton::Left,
                                     core::PointerAction::Release, {});
            composeFrame();
            check(ui != nullptr && ui->find("agent.task." + sid + ".detail") != nullptr,
                  "点任务行未展开详情（agent.task." + sid + ".detail 缺失）");
            const auto* runlist =
                ui != nullptr ? ui->find("agent.task." + sid + ".runlist") : nullptr;
            if (runlist == nullptr) {
                std::cerr << "agent_probes: run 列表容器缺失\n";
                passed = false;
            } else {
                const auto* row0 = ui->find("agent.task." + sid + ".runlist.0");
                const auto* row1 = ui->find("agent.task." + sid + ".runlist.1");
                if (row0 == nullptr || row1 == nullptr) {
                    std::cerr << "agent_probes: run 列表行缺失\n";
                    passed = false;
                } else {
                    check(row0->frame.width > 0.0f && row0->frame.height > 0.0f,
                          "runlist.0 零尺寸 " + frameStr(row0));
                    check(row1->frame.width > 0.0f && row1->frame.height > 0.0f,
                          "runlist.1 零尺寸 " + frameStr(row1));
                    check(row0->text.find("成功") != std::string::npos,
                          "runlist.0 非最近一次 run，实测「" + row0->text + "」");
                    check(row1->text.find("受限") != std::string::npos &&
                              row1->text.find("205000") != std::string::npos,
                          "runlist.1 缺状态/token 合计，实测「" + row1->text + "」");
                    const auto* errRow =
                        ui->find("agent.task." + sid + ".runlist.1.err");
                    check(errRow != nullptr &&
                              errRow->text.find("200K") != std::string::npos,
                          "runlist.1.err 缺 error 原文");
                }
            }
            // 点 edit 中心 → 编辑表单出现且完全落在页面内容区内。
            // 注意：composeFrame 会重建元素树，跨帧持有的 Element* 立即失效
            //（上面那次展开点击后必须重新 find，否则用的是已析构元素的 frame）
            const auto* editBtnNow = ui->find("agent.task." + sid + ".edit");
            if (editBtnNow == nullptr) {
                std::cerr << "agent_probes: 重新查找 edit 按钮失败\n";
                passed = false;
            } else {
                const float ex = editBtnNow->frame.x + editBtnNow->frame.width * 0.5f;
                const float ey = editBtnNow->frame.y + editBtnNow->frame.height * 0.5f;
                core::queuePointerButton(window, ex, ey, core::PointerButton::Left,
                                         core::PointerAction::Press, {});
                core::queuePointerButton(window, ex, ey, core::PointerButton::Left,
                                         core::PointerAction::Release, {});
                composeFrame();
                const auto* form =
                    ui != nullptr ? ui->find("agent.edit.form") : nullptr;
                const auto* pageNow =
                    ui != nullptr ? ui->find("agent.page") : nullptr;
                if (form == nullptr || pageNow == nullptr) {
                    std::cerr << "agent_probes: 点 edit 后 agent.edit.form 缺失\n";
                    passed = false;
                } else {
                    check(frameContains(*pageNow, *form),
                          "编辑表单未落在页面内容区内 form" + frameStr(form) + " page" +
                              frameStr(pageNow));
                    check(ui->find("agent.edit.cron") != nullptr,
                          "schedule 任务的编辑表单未渲染 cron 输入");
                    const auto* save = ui->find("agent.edit.save");
                    check(save != nullptr && save->frame.width > 0.0f,
                          "编辑表单缺保存按钮");
                }
            }
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
