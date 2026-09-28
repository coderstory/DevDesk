// markdown_probes.cpp — DevDesk probes：markdown 模块 4 组断言
//（spec docs/superpowers/specs/2026-09-28-devdesk-probes-design.md §3-probe-full）
//
// 组 A（类 3b 双层卡，P2，本文件已实现）：
//   编辑态单栏 compose → 断言 "md.editor.bg" 不存在。
//   回归 770beb3：drawTextColumnChrome 删掉 ui.rect(columnId + ".bg")
//   （markdown_module.cpp:1033-1038）——旧列壳 bg 叠在 input 整面 surface 上
//   形成「双层近白卡 + 双边框 + 焦点阴影压边」。证伪门禁：把
//   drawTextColumnChrome 里的列壳 bg 加回来（id 必须是 columnId + ".bg"）
//   → find("md.editor.bg") 非空 → 红。
// 组 A 附（同一修复的实时列孪生）：实时态断言 "md.live.bg" 同样不存在
//   （drawTextColumnChrome 是编辑/实时两态共用实现，headless 测试
//   test_markdown.cpp:2726-2727 同款双向锁定）。
//
// 组 D（类 10 双栏同步滚动，P1，本文件已实现）：
//   回归守卫（W10，5b4b6cf）：markdown_module.cpp:1536-1538 每帧调用
//   ui.setScrollOffset("md.preview.scroll", computeSyncOffset(...))——
//   EUI-NEO syncOwnedScrollState 初始化后每帧声明值（.offset()）不再生效，
//   撤掉这两行 → 双栏态预览停滞在进栏瞬间锚点。
//   流程：双栏态 compose → 指针放到编辑器命中区 → 注入滚轮
//   queueScrollInput(window, 0, -5) → 2 帧后断言：
//     a. 编辑器 InputState.verticalScroll > 0（滚轮注入真实生效）；
//     b. md.preview.scroll onScrollOffsetChanged 被调用（帧末
//        consumePendingScrollOffsets 送达 setScrollOffset = W10 修复在位；
//        撤 L1537-1538 → 0 次调用 → 红）；
//     c. md.preview.scroll 元素 scrollOffset != 0（任务主判据「滚动生效」）。
//   实现：compose lambda 内每帧重挂回调计数器（元素树每帧重建、跨帧
//   Element* 失效——计数状态放持久 recorder，包装随旧树作废无需还原）。
//
// 组 P2/P3（本文件仅框 TODO，未实现）：见文件尾
//   §TODO-8a（类 8(a) 状态栏窄窗截断，P3）与 §TODO-2c（类 2(c) chip
//   hover 两截色，P3）。

#include "probes/probe_harness.h"
#include "test_support.h"
#include "app_registry.h"
#include "core/module_interface.h"
#include "core/database.h"
#include "module_markdown/md_ids.h"

#include <components/input.h>  // input_detail::InputModel::InputState（滚动断言读口）

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {

bool floatEq(float a, float b) { return std::fabs(a - b) < 0.5f; }

// markdown 探针专用环境：已 migrate 的临时库注册为进程级 appDatabase
//（模块 onStart 经 core::appDatabase() 拿连接，null 时跳过真实索引）+ 笔记
// 目录 / 视图状态文件重定向（同 test_support::MdHeadlessEnv 语义，这里内联
// 实现——不变更 test_support.h）。析构反转：注销 appDatabase → 还原环境变量
// → 删整棵临时目录。
struct ProbeMdEnv {
    std::unique_ptr<core::Database> db;
    std::filesystem::path dir;       // 笔记目录（DEVDESK_NOTES_DIR）
    std::filesystem::path viewState; // 视图状态文件（DEVDESK_MARKDOWN_VIEW_STATE_PATH）
    std::string savedNotes;
    std::string savedView;

    ProbeMdEnv() {
        dir = test_support::makeTempDir("mkprobe_", "notes");
        viewState = dir / "view_state.json";
        auto r = core::Database::open(dir / "devdesk.db");
        if (r.isFail()) {
            throw std::runtime_error("md probe: Database::open failed: " +
                                     r.errorMessage());
        }
        db = std::make_unique<core::Database>(std::move(r.value()));
        if (db->migrate().isFail()) {
            throw std::runtime_error("md probe: migrate failed");
        }
        ::core::registerAppDatabase(&db->db());  // 模块 onStart 依赖（同 MdHeadlessEnv）
        const char* cur = std::getenv("DEVDESK_NOTES_DIR");
        savedNotes = cur ? cur : "";
        cur = std::getenv("DEVDESK_MARKDOWN_VIEW_STATE_PATH");
        savedView = cur ? cur : "";
        test_support::setEnvVar("DEVDESK_NOTES_DIR", dir.string());
        test_support::setEnvVar("DEVDESK_MARKDOWN_VIEW_STATE_PATH",
                                viewState.string());
    }

    ~ProbeMdEnv() {
        ::core::registerAppDatabase(nullptr);
        test_support::setEnvVar("DEVDESK_NOTES_DIR", savedNotes);
        test_support::setEnvVar("DEVDESK_MARKDOWN_VIEW_STATE_PATH", savedView);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    ProbeMdEnv(const ProbeMdEnv&) = delete;
    ProbeMdEnv& operator=(const ProbeMdEnv&) = delete;
};

// 长文档：跨栏分块渲染 + 编辑器/预览双向滚动空间都足够（组 D 前提）。
// 12 组 × ~12 行 ≈ 146 行——双栏态中栏（~420px 宽）下编辑器文本高远大于
// 视口 → maxVerticalScroll > 0；预览 y 映射越过 20% 视口余量钳位区。
std::string makeLongNote() {
    std::string t = "# DevDesk probe note\n\n";
    for (int r = 0; r < 12; ++r) {
        const std::string tag = std::to_string(r);
        t += "para r" + tag + " A one\npara r" + tag + " A two\npara r" + tag +
             " A three\npara r" + tag + " A four\n\n";
        t += "para r" + tag + " B one\npara r" + tag + " B two\n\n";
        t += "- r" + tag + " list one\n- r" + tag + " list two\n\n";
        t += "```\nr" + tag + " f1\nr" + tag + " f2\nr" + tag + " f3\n```\n\n";
    }
    t += "final tail line\n";
    return t;
}

// 视图状态预置：mode=1（ViewMode::Split，枚举 0=Edit 1=Split 2=Preview 3=Live）
// + lastPath="note.md"——onStart 的 loadViewState（markdown_module.cpp:3287）
// 直接恢复「打开 note.md + 双栏态」，探针无需依赖树行索引/点击顺序。
constexpr const char* kSplitViewStateJson =
    "{\"lastPath\":\"note.md\",\"mode\":1,\"collapsedDirs\":[]}";

// md.preview.scroll onScrollOffsetChanged 计数器（spec §类 10 断言机制）：
// 元素树每帧重建，包装必须每帧在 compose lambda 内对当帧元素重挂；计数与
// 记录值放本结构体（生命周期跨帧），旧元素上的旧包装随旧树作废、无需还原。
struct ScrollRecorder {
    int calls = 0;        // setScrollOffset 实际送达实例的次数（≈ W10 修复在位）
    float lastOffset = -1.0f;  // 最近一次送达的偏移（「记录值 ≠ 前值」判据）
    std::function<void(float)> original;  // 当帧元素的原始回调（逐帧刷新）

    void wrap(core::dsl::Element* pv) {
        if (pv == nullptr) {
            return;
        }
        original = std::move(pv->onScrollOffsetChanged);
        pv->onScrollOffsetChanged = [this](float offset) {
            ++calls;
            lastOffset = offset;
            if (original) {
                original(offset);
            }
        };
    }

    void reset() {
        calls = 0;
        lastOffset = -1.0f;
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }
    auto cfg = test_support::tempConfigFile("pfx_", "cfg");
    test_support::ScopedConfigPath scopedCfg(cfg);

    // markdown 环境 + 内容预置（视图状态在 onStart 前落盘）
    ProbeMdEnv mdEnv;
    test_support::writeTextFile(mdEnv.dir / "note.md", makeLongNote());
    test_support::writeTextFile(mdEnv.viewState, kSplitViewStateJson);

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();  // rescan + loadViewState：打开 note.md 并恢复双栏态

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    const std::string pageId = "md";
    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    bool passed = true;

    core::dsl::Ui* ui = nullptr;
    ScrollRecorder previewRecorder;
    // 单帧标准单元：pollOnce（输入派发 + 交互）→ compose（recompose 树 +
    // 帧末 consumePendingScrollOffsets；spec §4.2 成对要求）+ 预览滚动回调
    // 计数器重挂（组 D 用，元素树每帧重建）。
    auto composeFrame = [&]() {
        probe::composeModuleFrame(runtime, window, pageId, w, h,
                                  [&](eui::Ui& u, const eui::Screen& screen) {
                                      ui = &u;
                                      mod->compose(u, screen);
                                      previewRecorder.wrap(
                                          u.find("md.preview.scroll"));
                                  });
    };
    // 真实命中分发：press + release 排队 → 下一帧 update 消费（settings 探针
    // 同款）。元素中心坐标即窗口逻辑坐标（probe 窗 dpiScale=1）。
    auto clickElement = [&](const core::dsl::Element& element) {
        const double cx = element.frame.x + element.frame.width * 0.5;
        const double cy = element.frame.y + element.frame.height * 0.5;
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Press, {});
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Release, {});
    };
    // 模式切换后过渡 0.15s（M2 淡出淡入，viewFadeIn_）+ 首帧建宽度：
    // 稳定帧数兜底（args.frames 至少 12）。
    auto settleFrames = [&](int n) {
        const int count = n > 12 ? n : 12;
        for (int i = 0; i < count; ++i) {
            composeFrame();
        }
    };
    auto clickMode = [&](int index, const char* name) -> bool {
        const core::dsl::Element* hit = ui != nullptr ? ui->find(
            std::string("md.mode.hit.") + std::to_string(index))
                                                      : nullptr;
        if (hit == nullptr) {
            std::cerr << "markdown_probes: md.mode.hit." << index << " ("
                      << name << ") not found\n";
            return false;
        }
        clickElement(*hit);
        composeFrame();
        settleFrames(1);
        return true;
    };

    // 初始帧（视图状态已恢复双栏态：打开 note.md，预览列在树）
    settleFrames(1);

    // ==================== 组 A：类 3(b) 双层卡（P2） ====================
    // 切编辑态（单栏）→ 断言旧列壳 bg 不存在
    if (!clickMode(0, "编辑")) {
        passed = false;
    }
    settleFrames(1);
    {
        const core::dsl::Element* editor =
            ui != nullptr ? ui->find("md.editor") : nullptr;
        const core::dsl::Element* editorHit =
            ui != nullptr ? ui->find(module::markdown::kEditorInputHitId)
                          : nullptr;
        const core::dsl::Element* preview =
            ui != nullptr ? ui->find("md.preview") : nullptr;
        if (editor == nullptr || editorHit == nullptr) {
            std::cerr << "markdown_probes: 编辑态单栏结构缺失 (md.editor="
                      << (editor ? "ok" : "null")
                      << ", input.hit=" << (editorHit ? "ok" : "null")
                      << ")——组 A 断言落空，非真空判定\n";
            passed = false;
        }
        if (preview != nullptr) {
            std::cerr << "markdown_probes: 编辑态仍存在 md.preview（非单栏）\n";
            passed = false;
        }
        // 回归目标（770beb3 修复）：drawTextColumnChrome 不再建
        // columnId + ".bg"，编辑列只剩 input 单层表面底
        if (ui != nullptr && ui->find("md.editor.bg") != nullptr) {
            std::cerr << "markdown_probes: 类3(b) FAIL——md.editor.bg 存在"
                         "（列壳双层卡回归）\n";
            passed = false;
        }
    }

    // 组 A 附：实时态同款列壳（同一 drawTextColumnChrome 实现）——
    // "md.live.bg" 同样必须不存在；md.live 列壳正常在位
    if (!clickMode(1, "实时")) {
        passed = false;
    }
    settleFrames(1);
    {
        const core::dsl::Element* live =
            ui != nullptr ? ui->find("md.live") : nullptr;
        if (live == nullptr) {
            std::cerr << "markdown_probes: 实时态列壳 md.live 缺失\n";
            passed = false;
        }
        if (ui != nullptr && ui->find("md.live.bg") != nullptr) {
            std::cerr << "markdown_probes: 类3(b) FAIL——md.live.bg 存在"
                         "（实时列双层卡回归）\n";
            passed = false;
        }
    }

    // ==================== 组 D：类 10 双栏同步滚动（P1） ====================
    // 切双栏态 → 编辑器滚轮注入 → 2 帧后断言预览滚动生效
    if (!clickMode(2, "双栏")) {
        passed = false;
    }
    settleFrames(1);
    {
        const core::dsl::Element* pv0 =
            ui != nullptr ? ui->find("md.preview.scroll") : nullptr;
        const core::dsl::Element* editorHit =
            ui != nullptr ? ui->find(module::markdown::kEditorInputHitId)
                          : nullptr;
        if (pv0 == nullptr || editorHit == nullptr) {
            std::cerr << "markdown_probes: 双栏态结构缺失"
                         " (md.preview.scroll=" << (pv0 ? "ok" : "null")
                      << ", input.hit=" << (editorHit ? "ok" : "null")
                      << ")——组 D 无法执行\n";
            passed = false;
        } else {
            // 指针放到编辑器命中区中心（滚轮事件按指针位置命中目标派发，
            // runtime_lifecycle.h:120 hitTestScrollable）
            const double ex = editorHit->frame.x + editorHit->frame.width * 0.5;
            const double ey =
                editorHit->frame.y + editorHit->frame.height * 0.5;
            core::queuePointerMotion(window, ex, ey, core::PointerButtons{},
                                     core::KeyModifiers{});
            // 三跳累积 -15：单跳 -5 换 ~165px，三跳保证越过预览视口 20% 顶部
            // 余量钳位（computeSyncOffset 减 pvH*0.2）后续偏移仍非零
            core::queueScrollInput(window, 0.0, -5.0);
            core::queueScrollInput(window, 0.0, -5.0);
            core::queueScrollInput(window, 0.0, -5.0);
            previewRecorder.reset();
            composeFrame();  // 帧 1：update 消费滚轮 → compose 推 setScrollOffset
                             // → 帧末送达实例并触发 onScrollOffsetChanged
            composeFrame();  // 帧 2：「2 帧后」判据

            // a. 编辑器自身滚动生效（滚轮注入到达 input 的 onScroll 处理器）
            const auto& st =
                ui->state<components::input_detail::InputModel::InputState>(
                    module::markdown::kEditorInputId);
            if (st.verticalScroll <= 0.0f) {
                std::cerr << "markdown_probes: 类10 FAIL——编辑器 verticalScroll="
                          << st.verticalScroll
                          << "（滚轮未到达编辑器）\n";
                passed = false;
            }
            // b. W10 修复在位：每帧 ui.setScrollOffset("md.preview.scroll", …)
            //    真正送达预览滚动实例（撤 markdown_module.cpp:1536-1538 →
            //    本计数恒 0 → 红，证伪门禁）
            if (previewRecorder.calls <= 0) {
                std::cerr << "markdown_probes: 类10 FAIL——setScrollOffset 未送达"
                             "预览（onScrollOffsetChanged 调用次="
                          << previewRecorder.calls << "）\n";
                passed = false;
            }
            // c. 任务主判据：2 帧后预览滚动偏移非零（元素帧声明值 = 模块
            //    本帧 computeSyncOffset 结果，非零即滚动生效）
            const core::dsl::Element* pvAfter =
                ui != nullptr ? ui->find("md.preview.scroll") : nullptr;
            if (pvAfter == nullptr || floatEq(pvAfter->scrollOffset, 0.0f)) {
                std::cerr << "markdown_probes: 类10 FAIL——预览 scrollOffset="
                          << (pvAfter != nullptr ? pvAfter->scrollOffset : -1.0f)
                          << "（期望非零；记录值=" << previewRecorder.lastOffset
                          << "）\n";
                passed = false;
            }
        }
    }

    mod->onStop();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!passed) std::cerr << "FAIL\n";
    return passed ? 0 : 1;
}

// =====================================================================
// §TODO-8a（类 8(a) 状态栏窄窗截断，P3）——未实现，仅框架：
//   目标：markdown 状态栏左区（drawStatusBar，markdown_module.cpp:1336-1440，
//   元素 md.status.save / md.status.pos / md.status.bg）在窄窗最小尺寸
//   1200×720 下长文件名被 .clip() 硬裁不可读。
//   搭法（双栏态最小窗）：
//     a. 写一篇文件名 ≈ 40 CJK 字的笔记（如 "状态栏窄窗长文件名占位笔记
//        一二三四五六七八九十甲乙丙丁戊己庚辛壬癸.md"），openNote 后
//        md.status.save 文案 = 文件名 + " ✓ 已保存 …"；
//     b. 断言 md.status.save 文本内在宽 ≤ 可用宽（TextPrimitive::
//        measureTextSize，dsl.h:1729）或 wrap/clip 判别字段为修复落点真值
//        （修复前 clip 已存在 → 只信内在宽口径）；
//     c. 断言 md.status.save frame 不越 md.status.bg 右缘。
//   优先级 P3（弱——观感主体留人工），实现后删除本注释。
// §TODO-2c（类 2(c) chip hover 两截色，P3）——未实现，仅框架：
//   目标：drawTagBar（markdown_module.cpp:1287-1294 一带）标签 chip 主体
//   md.tags.chip.N.bg 无 hoverColor（两截色观感共享根因）。
//   搭法（带标签笔记，tagsForNote 非空 → chip 出树）：
//     a. md.tags.chip.0.bg 元素 hoverColor != nullptr（当前无 → 红）；
//     b. md.tags.chip.0.bg 的 hoverColor == md.tags.chip.0.del 的 hoverColor
//        （修复后统一；.del 现已挂 t.colors.hover）。
//   优先级 P3（弱，spec §3-probe-full 类 2(c)），实现后删除本注释。