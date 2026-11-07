// mcp_dialog_probe.cpp — bug180 MCP 执行详情弹窗两态对照探针
//
// 老板报单 2026-11-06 23:08：MCP 执行页弹窗空文本看不出问题，填充大量文本后
// 布局全乱。根因族 = 固定高度/绝对坐标 + 内容自增溢出：desc 描述盒
// （mcp_page_module.cpp y=88 h=48）wrap 无 clip，长描述墨迹越盒直铺，
// 压到 148 的「参数 schema」标题/展示框（同 clue 110 / bug145 schema.text
// 「折行溢出不越框」不变量族）。修法 = 按节点框 clip（b456c8d2 同族样板）。
//
// 两态真窗口渲染对照（证据 evidence/mcp-dialog-batch/{before,after}/）：
//   --shot-empty  <png>  短描述工具弹窗（空态基线）
//   --shot-filled <png>  6 行长描述工具弹窗（溢出态）
// 无开关 = 纯断言门禁：desc 节点 clip 置位 + 长描述不越盒（frame 契约）
// + 两卡命中面齐全。构图走 clickId 式直调 hit->onClick（clue 110⑤：真指针
// 路径不稳，勿用坐标点击）。
//
// 运行：mcp_dialog_probe [--shot-empty <png>] [--shot-filled <png>]

#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（同 agent_dialog_probe）

#include "core/mcp_tool.h"
#include "module_mcp/mcp_page_module.h"

#include <iostream>
#include <memory>
#include <string>

// app:: 层桩（probe 不建应用级 UI，compose 走 probe::composeModuleFrame）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_mcp_dialog_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

// 极简假工具（test_mcp_page.cpp FakeTool 的只读投影；探针不触达 execute）
class ProbeTool final : public core::mcp::IMcpTool {
public:
    ProbeTool(std::string name, std::string desc)
        : name_(std::move(name)), desc_(std::move(desc)) {}
    std::string name() const override { return name_; }
    std::string description() const override { return desc_; }
    std::string inputSchemaJson() const override {
        return R"({"type":"object","properties":{}})";
    }
    core::Result<core::mcp::ToolResult> execute(const std::string&) override {
        return core::Result<core::mcp::ToolResult>::ok(
            core::mcp::ToolResult{false, "ok"});
    }

private:
    std::string name_;
    std::string desc_;
};

// 6 行量级长描述：把墨迹推出 48 高（约 2 行）的固定盒（与 doctest 同文案）
std::string longDesc() {
    return "这是一个用于回归的超长工具描述：第一行内容足够长会折行；"
           "第二行继续铺满整行宽度迫使换行；第三行仍在增长，"
           "用来把墨迹推出 48 高的固定盒；第四行、第五行、第六行继续加码，"
           "确保溢出量远超一行，足以压到下方的参数 schema 展示区。";
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    // 私有构图开关（parseProbeArgs 对未知参数忽略，这里自己收）
    std::string shotEmpty, shotFilled;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--shot-empty" && i + 1 < argc) {
            shotEmpty = argv[++i];
        } else if (arg == "--shot-filled" && i + 1 < argc) {
            shotFilled = argv[++i];
        }
    }

    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("mcp_dialog_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    core::mcp::McpToolRegistry reg;
    auto shortTool = std::make_shared<ProbeTool>("probe_short_tool", "短描述工具");
    auto fatTool = std::make_shared<ProbeTool>("probe_fat_tool", longDesc());
    (void)reg.registerTool(shortTool);
    (void)reg.registerTool(fatTool);

    module::mcp::McpPageModule mod(reg);

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    // 断言基线跟 compose 同口径（probe_harness bug183/185：按 framebuffer 反推）
    const auto [logicalW, logicalH] = probe::windowLogicalSize(window);
    const float w = logicalW > 1.0f ? logicalW : static_cast<float>(args.windowW);
    const float h = logicalH > 1.0f ? logicalH : static_cast<float>(args.windowH);
    bool passed = true;

    eui::Ui* ui = nullptr;
    auto composeFrame = [&]() {
        probe::composeModuleFrame(
            runtime, window, "mcp", w, h,
            [&](eui::Ui& u, const eui::Screen& screen) {
                ui = &u;
                mod.compose(u, screen);
            });
    };

    // 弹窗过渡动画跑完再落图（probe_harness captureFramePng 同款时序）：先
    // compose 让条件元素进 DOM，再 24 帧纯 poll 跑完动画——**末尾不得再
    // recompose**：重建树会把 animate(Opacity) 过渡态清回起点，渲染抓到
    // opacity=0 的隐形弹窗（首拍全空页的教训；节点 opacity 字段只是目标值）。
    auto settle = [&]() {
        composeFrame();
        for (int i = 0; i < 24; ++i) {
            probe::pollOnce(window, runtime);
        }
    };

    auto openDialogOf = [&](const char* toolName) {
        composeFrame();
        const auto* hit = ui->find(std::string("mcp.card.") + toolName + ".hit");
        if (hit == nullptr || hit->onClick == nullptr) {
            std::cerr << "mcp_dialog_probe: 卡片命中面缺失: " << toolName << "\n";
            passed = false;
            return false;
        }
        hit->onClick();
        settle();
        const auto* backdrop = ui->find("mcp.dialog.detail.backdrop");
        if (backdrop == nullptr || backdrop->opacity != 1.0f) {
            std::cerr << "mcp_dialog_probe: 弹窗未开: " << toolName << "\n";
            passed = false;
            return false;
        }
        return true;
    };

    auto shotWith = [&](const std::string& path, int renderPasses) {
        if (path.empty() || ui == nullptr) return true;
        probe::FrameCapture capture(window);
        if (!capture.ok()) {
            std::cerr << "mcp_dialog_probe: 帧捕获会话失败: " << capture.error()
                      << "\n";
            passed = false;
            return false;
        }
        std::vector<unsigned char> rgba;
        int cw = 0, ch = 0;
        if (!capture.readFrame(runtime, rgba, cw, ch, renderPasses)) {
            passed = false;
            return false;
        }
        if (!probe::writeRgbaToPng(path, rgba.data(), cw, ch)) {
            passed = false;
            return false;
        }
        std::cout << "probe: screenshot " << path << " (" << cw << "x" << ch
                  << ")\n";
        return true;
    };

    // —— 门禁断言（无开关也跑）：溢出态 desc 必须按节点框 clip ——
    if (!openDialogOf("probe_fat_tool")) {
        core::window::destroyWindow(window);
        probe::terminateWindowBackend();
        return 3;
    }
    if (const auto* desc = ui->find("mcp.dialog.detail.desc");
        desc == nullptr || desc->text.empty()) {
        std::cerr << "mcp_dialog_probe: desc 节点缺失/空文本\n";
        passed = false;
    } else if (!desc->clip) {
        std::cerr << "mcp_dialog_probe: desc 未按节点框 clip（bug180 回归）\n";
        passed = false;
    }
    // --shot-filled：填充态落图
    shotWith(shotFilled, 16);

    // —— 空态基线：关弹窗 → 换短描述工具卡 → 落图 ——
    if (const auto* closeBtn = ui->find("mcp.dialog.detail.btn.close.bg");
        closeBtn != nullptr && closeBtn->onClick != nullptr) {
        closeBtn->onClick();
    }
    settle();
    if (openDialogOf("probe_short_tool")) {
        shotWith(shotEmpty, 16);
    }

    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    std::cout << (passed ? "probe: PASS" : "probe: FAIL") << "\n";
    return passed ? 0 : 4;
}
