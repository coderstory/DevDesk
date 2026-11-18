// mcp_card_343_probe.cpp — need343「文本全左侧」工具卡重排可视基线探针
//
// 设计稿 docs/design/2026-11-18-mcp-card-redesign.md：卡高 96、文本列 x=16
// 单基线（名称/状态行/描述）、右轨 40×40 图标 + hover 纯图标箭头、右侧零文本。
// 造 4 工具（长短名/长短描述混合）compose MCP 页网格落整页 PNG，供与 demo
//（docs/design/2026-11-18-mcp-card-redesign.html）逐卡目检比对。结构/几何
// 断言归 tests/test_modules/test_mcp_page.cpp（need343 两条结构用例），本
// 探针只产出视觉证据 + 卡面齐全门禁。
//
// 运行：mcp_card_343_probe --shot <png>（无 --shot 仅跑门禁）

#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（同 mcp_dialog_probe）

#include "core/mcp_tool.h"
#include "mcp_service_bridge.h"
#include "module_mcp/mcp_page_module.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

// app:: 层桩（probe 不建应用级 UI，compose 走 probe::composeModuleFrame）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_mcp_card_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

// 极简假工具（探针不触达 execute，同 mcp_dialog_probe 的 ProbeTool）
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

// app::mcp 桥只读桩：卡面呈现「可用」绿点主态（服务状态行同享）
app::mcp::LiveStatus runningStatus() {
    app::mcp::LiveStatus st;
    st.running = true;
    st.port = 8931;
    return st;
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    // 私有构图开关（parseProbeArgs 对未知参数忽略，这里自己收）
    std::string shot;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--shot" && i + 1 < argc) shot = argv[++i];
    }

    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("mcp_card_343_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }
    app::mcp::setHandlers(nullptr, nullptr, nullptr, &runningStatus);

    core::mcp::McpToolRegistry reg;
    // 长名+长描述（省略号截断态）/ 短名+短描述（常规态）混合，340–420 列宽
    // 自适应下 4 卡 = 3 列两行，第二行留半行看行卡排布
    (void)reg.registerTool(std::make_shared<ProbeTool>(
        "web_fetch_archive_site",
        "抓取网页内容并归档为 Markdown 快照，支持批量抓取与超时控制"));
    (void)reg.registerTool(std::make_shared<ProbeTool>(
        "memory_search", "查询本机长期记忆库"));
    (void)reg.registerTool(std::make_shared<ProbeTool>(
        "codegraph_explore_repository",
        "按符号与调用关系检索代码仓库，返回带行号的相关源码片段与调用路径"));
    (void)reg.registerTool(
        std::make_shared<ProbeTool>("notes_query", "检索本地笔记"));

    module::mcp::McpPageModule mod(reg);

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    // 断言基线跟 compose 同口径（probe_harness：按 framebuffer 反推）
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
    auto settle = [&]() {
        composeFrame();
        for (int i = 0; i < 24; ++i) {
            probe::pollOnce(window, runtime);
        }
    };

    settle();
    if (ui == nullptr ||
        ui->find("mcp.card.web_fetch_archive_site") == nullptr ||
        ui->find("mcp.card.notes_query") == nullptr) {
        std::cerr << "mcp_card_343_probe: 卡面缺失\n";
        passed = false;
    }

    if (!shot.empty() && passed) {
        probe::FrameCapture capture(window);
        if (!capture.ok()) {
            std::cerr << "mcp_card_343_probe: 帧捕获会话失败: "
                      << capture.error() << "\n";
            passed = false;
        } else {
            std::vector<unsigned char> rgba;
            int cw = 0, ch = 0;
            if (!capture.readFrame(runtime, rgba, cw, ch, 16)) {
                passed = false;
            } else if (!probe::writeRgbaToPng(shot, rgba.data(), cw, ch)) {
                passed = false;
            } else {
                std::cout << "probe: screenshot " << shot << " (" << cw << "x"
                          << ch << ")\n";
            }
        }
    }

    app::mcp::setHandlers(nullptr, nullptr, nullptr, nullptr);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    std::cout << (passed ? "probe: PASS" : "probe: FAIL") << "\n";
    return passed ? 0 : 4;
}
