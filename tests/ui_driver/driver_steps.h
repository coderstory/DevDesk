#pragma once
// driver_steps.h — 步骤解释器（债-18 编排件）：九种步骤 → 动作 dispatch +
// wait 轮询循环 + click 命中回读执行。持有窗口 / Runtime / 帧循环，只做编排
// 不做纯逻辑（判定交 driver_checks.h，格式化交 driver_report.h / driver_dump.h）。
//
// 首版不做动作前自动等待（调研 §2.3 确定性取舍，非先例跟随）：失败显式落在
// wait 步骤上。命中回读语义 = 「点击后当帧该点实际命中」（Playwright deferred
// check 同构）：click 同帧 press+release，帧后查询的是当帧树；点击回调改了树
// （弹层刚开）时 readback 与点击瞬间可能有差——场景作者用 wait 或 force 化解
//（方案卡 R1）。
#include "probes/probe_harness.h"
#include "test_support.h"  // setShortcutModifier（Shortcut+ 平台判定，与仓内惯例同源）
#include "ui_driver/driver_checks.h"
#include "ui_driver/driver_dump.h"
#include "ui_driver/driver_keys.h"
#include "ui_driver/driver_report.h"
#include "ui_driver/driver_schema.h"

#include <chrono>
#include <ratio>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ui_drive {

struct DriverEnv {
    core::window::Handle window = nullptr;
    core::dsl::Runtime* runtime = nullptr;
    float windowW = 1200.0f;
    float windowH = 1000.0f;
    std::string outDir;              // 截图 / dump 落盘根
    bool keepGoing = false;          // --keep-going 或场景 failureStrategy
    int defaultTimeoutMs = 5000;
    // app 组装函数（main 提供：真 app::compose）。帧循环 = pollOnce + compose 成对。
    std::function<void(eui::Ui&, const eui::Screen&)> composeFn;
};

struct RunOutcome {
    std::vector<StepResult> results;
    bool reportWriteOk = true;  // 报告/dump/截图落盘失败 → 退出码 1（不静默）
    int exitCode = 0;
};

namespace detail {

using core::dsl::Element;
using core::dsl::Ui;

inline TreeView walkTreeView(Ui& ui) {
    TreeView tree;
    for (const auto& root : ui.roots()) {
        probe::detail::walkElements(*root, "", tree.parentOf, tree.byId, tree.leaves);
    }
    return tree;
}

inline std::string candidatesSuffixForIds(const std::string& id, const TreeView& tree) {
    std::vector<std::string> vocab;
    vocab.reserve(tree.byId.size());
    for (const auto& [key, element] : tree.byId) {
        vocab.push_back(key);
    }
    const std::vector<std::string> hits = nearCandidates(id, vocab);
    if (hits.empty()) return {};
    std::string out = "（相近候选:";
    for (std::size_t i = 0; i < hits.size(); ++i) {
        out += (i == 0 ? " " : "、") + hits[i];
    }
    out += "）";
    return out;
}

// 解析目标：返回元素指针（帧内有效）；miss = nullptr 并把诊断写进 err。
inline const Element* resolveTarget(Ui& ui, const std::string& id, const TreeView& tree,
                                    std::string* err) {
    if (Element* el = ui.find(id)) {
        return el;
    }
    *err = "id '" + id + "' 不在当前页元素树" +
           candidatesSuffixForIds(id, tree) +
           "——独立子窗 / OS 原生控件不在驱动射程";
    return nullptr;
}

inline core::LayoutRect centerOf(const Element& el) {
    return core::LayoutRect{el.frame.x + el.frame.width * 0.5f,
                            el.frame.y + el.frame.height * 0.5f, 0.0f, 0.0f};
}

} // namespace detail

class SceneRunner {
public:
    explicit SceneRunner(DriverEnv env) : env_(std::move(env)) {}

    // 主循环：逐步骤执行（fail-fast 默认；keepGoing 收集全部失败）。
    RunOutcome run(const Scene& scene, const std::string& sceneName) {
        RunOutcome outcome;
        const int total = static_cast<int>(scene.steps.size());
        for (int i = 0; i < total; ++i) {
            StepResult result;
            result.index = i + 1;
            result.type = scene.steps[i].type;
            result.target = describeTarget(scene.steps[i]);
            const auto started = std::chrono::steady_clock::now();
            execute(scene.steps[i], &result);
            result.elapsedMs = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - started)
                                   .count();
            std::cout << formatStepLine(result, total) << std::endl;
            outcome.results.push_back(std::move(result));
            if (!outcome.results.back().passed && !env_.keepGoing) {
                break;  // fail-fast：场景是单链，继续跑只产生级联误导（调研 §4.3）
            }
        }
        outcome.reportWriteOk = reportWriteOk_;
        outcome.exitCode = aggregateExitCode(outcome.results, outcome.reportWriteOk);
        (void)sceneName;
        return outcome;
    }

private:
    DriverEnv env_;
    std::unique_ptr<probe::FrameCapture> capture_;  // 惰性建（渲染后端重量级，至多一个）
    bool reportWriteOk_ = true;

    void composeFrame() {
        probe::pollOnce(env_.window, *env_.runtime);
        env_.runtime->compose("app", env_.windowW, env_.windowH, env_.composeFn);
    }

    static std::string describeTarget(const Step& step) {
        if (step.type == "click") return step.id.empty() ? "point(" + std::to_string(static_cast<int>(step.pointX)) + "," + std::to_string(static_cast<int>(step.pointY)) + ") [坐标步骤]" : step.id;
        if (step.type == "type") return "'" + step.text + "'";
        if (step.type == "key") return step.keyName;
        if (step.type == "scroll") return "dx=" + std::to_string(static_cast<int>(step.dx)) + " dy=" + std::to_string(static_cast<int>(step.dy));
        if (step.type == "drag") return step.id + " → " + step.toId;
        if (step.type == "wait") return step.ms > 0 ? std::to_string(step.ms) + "ms" : step.until + " " + (step.id.empty() ? "'" + step.text + "'" : step.id);
        if (step.type == "assert") return step.check + " " + (step.id.empty() ? (step.check == "text" ? "'" + step.text + "'" : std::string()) : step.id);
        if (step.type == "screenshot" || step.type == "dump") return step.path;
        return {};
    }

    // 失败现场截图（证据链缺失不静默：落图失败置 reportWriteOk_=false → 退出 1）
    void captureFailureShot(StepResult* result, int stepIndex) {
        const std::string path = env_.outDir + "/fail-" + std::to_string(stepIndex) + ".png";
        if (captureFrame(path)) {
            result->screenshotPath = path;
        }
    }

    bool captureFrame(const std::string& path) {
        if (!capture_) {
            capture_ = std::make_unique<probe::FrameCapture>(env_.window);
        }
        if (!capture_->ok()) {
            std::cerr << "ui_driver: FrameCapture 建失败: " << capture_->error() << "\n";
            reportWriteOk_ = false;
            return false;
        }
        if (!capture_->capturePng(*env_.runtime, path)) {
            reportWriteOk_ = false;
            return false;
        }
        return true;
    }

    void execute(const Step& step, StepResult* result) {
        if (step.type == "click") execClick(step, result);
        else if (step.type == "type") execType(step, result);
        else if (step.type == "key") execKey(step, result);
        else if (step.type == "scroll") execScroll(step, result);
        else if (step.type == "drag") execDrag(step, result);
        else if (step.type == "wait") execWait(step, result);
        else if (step.type == "assert") execAssert(step, result);
        else if (step.type == "screenshot") execScreenshot(step, result);
        else if (step.type == "dump") execDump(step, result);
        else result->errorReason = "不支持的步骤类型";
        if (!result->passed && result->screenshotPath.empty()) {
            captureFailureShot(result, result->index);
        }
    }

    // ---- click（含命中回读 F-05）----
    void execClick(const Step& step, StepResult* result) {
        core::dsl::Ui* ui = currentUi();
        if (ui == nullptr) {
            result->errorReason = "compose 未执行（ui 为空）";
            return;
        }
        double cx = 0.0;
        double cy = 0.0;
        std::string targetResolvedId;
        if (!step.id.empty()) {
            const TreeView tree = detail::walkTreeView(*ui);
            std::string err;
            const core::dsl::Element* el = detail::resolveTarget(*ui, step.id, tree, &err);
            if (el == nullptr) {
                result->errorReason = err;
                return;
            }
            targetResolvedId = el->id;
            cx = el->frame.x + el->frame.width * 0.5;
            cy = el->frame.y + el->frame.height * 0.5;
        } else {
            cx = step.pointX;
            cy = step.pointY;
        }
        const core::PointerButton button =
            step.button == "right" ? core::PointerButton::Right : core::PointerButton::Left;
        probe::click(env_.window, *env_.runtime, cx, cy, button);

        if (step.force || !step.readback || targetResolvedId.empty()) {
            result->passed = true;  // force / 坐标点击：跳过命中断言（对齐 Playwright force）
            return;
        }
        // 命中回读：帧后查「点击点当帧实际命中的可交互元素」（hitTestInteractiveAt
        // 纯查询，不走 hover 缓存）
        const std::string hitId = env_.runtime->hitTestInteractiveAt(cx, cy, 1.0f);
        const TreeView tree = detail::walkTreeView(*ui);
        const HitVerdict verdict = classifyHit(tree, targetResolvedId, hitId);
        result->hitId = hitId;
        switch (verdict) {
            case HitVerdict::Target:
            case HitVerdict::Descendant:
                result->passed = true;
                return;
            case HitVerdict::Hijacked:
                result->errorReason = "点击点 (" + std::to_string(static_cast<int>(cx)) + "," +
                                      std::to_string(static_cast<int>(cy)) + ") 实际命中 '" +
                                      hitId + "'（截胡了目标 '" + targetResolvedId +
                                      "'）——等弹层收场后重试，或 force:true 跳过命中断言";
                return;
            case HitVerdict::Empty:
                result->errorReason = "点击点 (" + std::to_string(static_cast<int>(cx)) + "," +
                                      std::to_string(static_cast<int>(cy)) +
                                      ") 无任何可交互元素接住（目标 '" + targetResolvedId +
                                      "'）——多为 clip 外或 disabled 树";
                return;
        }
    }

    void execType(const Step& step, StepResult* result) {
        probe::queueText(env_.window, *env_.runtime, step.text);
        composeFrame();  // update 消费文本 + compose 反映
        result->passed = true;
    }

    void execKey(const Step& step, StepResult* result) {
        const KeySpec spec = lookupKeyName(step.keyName);
        if (!spec.known) {
            result->errorReason = "未知键名 '" + step.keyName + "'（加载期应已拦截）";
            return;
        }
        int mods = spec.modifiers;
        if (spec.shortcut) {
            // Shortcut+ → 平台判定（EUI-NEO KeyModifiers::shortcut() 当唯一判据，
            // test_support 惯例：测试侧零平台宏）
            core::KeyModifiers probe{};
            test_support::setShortcutModifier(probe, true);
            if (probe.control) mods |= GLFW_MOD_CONTROL;
            if (probe.super) mods |= GLFW_MOD_SUPER;
        }
        // Press / Release 隔帧成对（probe_harness queueKey 契约）
        probe::queueKey(env_.window, *env_.runtime, spec.key, core::KeyAction::Press, mods);
        composeFrame();
        probe::queueKey(env_.window, *env_.runtime, spec.key, core::KeyAction::Release, mods);
        composeFrame();
        result->passed = true;
    }

    void execScroll(const Step& step, StepResult* result) {
        double cx = env_.windowW * 0.5;
        double cy = env_.windowH * 0.5;
        if (!step.id.empty()) {
            core::dsl::Ui* ui = currentUi();
            const TreeView tree = detail::walkTreeView(*ui);
            std::string err;
            const core::dsl::Element* el = detail::resolveTarget(*ui, step.id, tree, &err);
            if (el == nullptr) {
                result->errorReason = err;
                return;
            }
            cx = el->frame.x + el->frame.width * 0.5;
            cy = el->frame.y + el->frame.height * 0.5;
        }
        // 先定位指针再滚轮（滚动目标 = 指针所在滚动容器）
        core::queuePointerMotion(env_.window, cx, cy, {}, {});
        probe::queueScroll(env_.window, *env_.runtime, step.dx, step.dy);
        composeFrame();
        result->passed = true;
    }

    void execDrag(const Step& step, StepResult* result) {
        core::dsl::Ui* ui = currentUi();
        const TreeView tree = detail::walkTreeView(*ui);
        std::string errFrom, errTo;
        const core::dsl::Element* from = detail::resolveTarget(*ui, step.id, tree, &errFrom);
        const core::dsl::Element* to =
            from != nullptr ? detail::resolveTarget(*ui, step.toId, tree, &errTo) : nullptr;
        if (from == nullptr || to == nullptr) {
            result->errorReason = from == nullptr ? errFrom : errTo;
            return;
        }
        probe::drag(env_.window, *env_.runtime,
                    from->frame.x + from->frame.width * 0.5,
                    from->frame.y + from->frame.height * 0.5,
                    to->frame.x + to->frame.width * 0.5,
                    to->frame.y + to->frame.height * 0.5,
                    step.dragSteps);
        composeFrame();
        result->passed = true;
    }

    // wait 条件评估（每帧一轮：pollOnce+recompose 后调 driver_checks 判定；
    // 条件满足立即过——不等待满额超时）
    void execWait(const Step& step, StepResult* result) {
        if (step.until.empty()) {
            // 固定模拟帧等待（动画兜底，非首选）：dt=1/60，与 probe 24 帧收尾惯例同源
            const int frames = static_cast<int>(std::ceil(step.ms / 16.67));
            for (int i = 0; i < frames; ++i) {
                composeFrame();
            }
            result->passed = true;
            return;
        }
        const int timeoutMs =
            step.timeoutMs > 0 ? step.timeoutMs : env_.defaultTimeoutMs;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs);
        while (true) {
            composeFrame();
            core::dsl::Ui* ui = currentUi();
            const TreeView tree = detail::walkTreeView(*ui);
            bool satisfied = false;
            std::string resolvedId;
            if (!step.id.empty()) {
                if (const auto it = tree.byId.find(resolvedTargetId(*ui, step.id));
                    it != tree.byId.end()) {
                    resolvedId = it->first;
                }
            }
            if (step.until == "exists") {
                satisfied = elementExists(tree, resolvedId);
            } else if (step.until == "missing") {
                satisfied = !elementExists(tree, resolvedId);
            } else if (step.until == "text") {
                TextOp op = TextOp::Contains;
                textOpFromName(step.op, &op);
                satisfied = textPresent(tree, resolvedId, op, step.text);
            }
            if (satisfied) {
                result->passed = true;
                return;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                result->errorReason = "等待 '" + step.until + "' 超时 " +
                                      std::to_string(timeoutMs) + "ms（目标 " +
                                      (step.id.empty() ? "'" + step.text + "'" : step.id) + "）";
                return;
            }
        }
    }

    void execAssert(const Step& step, StepResult* result) {
        core::dsl::Ui* ui = currentUi();
        const TreeView tree = detail::walkTreeView(*ui);
        std::string resolvedId;
        const core::dsl::Element* el = nullptr;
        if (!step.id.empty()) {
            el = ui->find(step.id);
            if (el != nullptr) resolvedId = el->id;
        }
        if (step.check == "exists") {
            if (elementExists(tree, resolvedId)) {
                result->passed = true;
            } else {
                result->errorReason = "断言失败：元素 '" + step.id + "' 不存在" +
                                      detail::candidatesSuffixForIds(step.id, tree);
            }
            return;
        }
        if (step.check == "missing") {
            if (!elementExists(tree, resolvedId)) {
                result->passed = true;
            } else {
                result->errorReason = "断言失败：元素 '" + step.id + "' 仍存在";
            }
            return;
        }
        if (step.check == "text") {
            TextOp op = TextOp::Contains;
            textOpFromName(step.op, &op);
            const std::string scope = step.id.empty() ? "全页" : "元素 '" + step.id + "'";
            if (textPresent(tree, resolvedId, op, step.text)) {
                result->passed = true;
            } else {
                result->errorReason = "断言失败：文本 '" + step.text + "' 未命中（op=" +
                                      (op == TextOp::Equals ? "equals" : "contains") +
                                      "，范围=" + scope + "）";
            }
            return;
        }
        if (step.check == "geometry") {
            if (el == nullptr) {
                result->errorReason = "断言失败：元素 '" + step.id + "' 不存在" +
                                      detail::candidatesSuffixForIds(step.id, tree);
                return;
            }
            if (step.op == "within-window") {
                if (rectWithinWindow(el->frame, env_.windowW, env_.windowH)) {
                    result->passed = true;
                } else {
                    result->errorReason =
                        "断言失败：元素 '" + step.id + "' 超出窗口（frame " +
                        std::to_string(static_cast<int>(el->frame.x)) + "," +
                        std::to_string(static_cast<int>(el->frame.y)) + " " +
                        std::to_string(static_cast<int>(el->frame.width)) + "x" +
                        std::to_string(static_cast<int>(el->frame.height)) + "，窗口 " +
                        std::to_string(static_cast<int>(env_.windowW)) + "x" +
                        std::to_string(static_cast<int>(env_.windowH)) + "）";
                }
                return;
            }
            // intersects
            const core::dsl::Element* other = ui->find(step.otherId);
            if (other == nullptr) {
                result->errorReason = "断言失败：'other' 元素 '" + step.otherId + "' 不存在" +
                                      detail::candidatesSuffixForIds(step.otherId, tree);
                return;
            }
            if (rectIntersectsRect(el->frame, other->frame)) {
                result->passed = true;
            } else {
                result->errorReason = "断言失败：元素 '" + step.id + "' 与 '" +
                                      step.otherId + "' 不相交";
            }
            return;
        }
        result->errorReason = "未知 assert.check '" + step.check + "'";
    }

    void execScreenshot(const Step& step, StepResult* result) {
        const std::string path = env_.outDir + "/" + step.path;
        if (captureFrame(path)) {
            result->screenshotPath = path;
            result->passed = true;
        } else {
            result->errorReason = "截图落盘失败: " + path;
        }
    }

    void execDump(const Step& step, StepResult* result) {
        core::dsl::Ui* ui = currentUi();
        const std::vector<DumpRow> rows = collectDumpRows(*ui);
        const std::string path = env_.outDir + "/" + step.path;
        if (writeDumpFile(rows, path)) {
            result->passed = true;
        } else {
            result->errorReason = "dump 落盘失败: " + path;
        }
    }

    // 当前帧 ui（main 在 compose 回调里经 attachUi 挂进来；指针随帧重建更新）
    core::dsl::Ui* currentUi() { return ui_; }

public:
    // main 在每次 compose 后把 ui 指针挂进来（指针随帧重建而更新）
    void attachUi(core::dsl::Ui* ui) { ui_ = ui; }

private:
    core::dsl::Ui* ui_ = nullptr;

    static std::string resolvedTargetId(core::dsl::Ui& ui, const std::string& id) {
        if (core::dsl::Element* el = ui.find(id)) return el->id;
        return {};
    }
};

} // namespace ui_drive
