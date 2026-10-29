// src/module_mcp/mcp_page_module.h
// MCP 菜单页：工具卡片网格 + 详情弹窗 + 「试一下」。视觉基准 = 桌面
// mcp-menu-demo.html（token 对齐；图标用 FA7 码点；关闭态走模块门控整页注销，
// 页内不置灰、不放开关——开关在设置页，settings_ui_cards.cpp 的门控承诺）。
//
// 数据源：McpToolRegistry::list() 快照（构造可注入，默认进程全局注册表）；
// 服务状态行只读消费 app::mcp 桥 status()（模块层禁 include mcp_service_bridge
// 之外的 app 装配面）。IMcpTool::execute 一律后台线程 + compose 轮询收割，
// 禁止 UI 线程直调（keep-alive shared_ptr 保证注销瞬间不悬垂，见 core/mcp_tool.h）。
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

#include "core/mcp_tool.h"
#include "core/module_interface.h"
#include "core/theme.h"

#include <components/theme.h>

namespace module::mcp {

class McpPageModule final : public ::core::Module {
public:
    McpPageModule();  // 数据源 = core::mcp::globalMcpTools()
    explicit McpPageModule(::core::mcp::McpToolRegistry& registry);
    ~McpPageModule() override;

    ::core::PageId pageId() const override;
    std::string displayName() const override;
    std::string shortName() const override;
    // fa-plug (U+F1E6) 的 UTF-8 字节；须与 app_registry 表行 iconGlyph 列同值。
    std::string iconGlyph() const override;

    void compose(eui::Ui& ui, const eui::Screen& screen) override;

    // Esc 关闭详情弹窗（与弹窗「关闭」按钮 + backdrop 点击三通道并存）
    bool onKeyEvent(const eui::KeyEvent& event) override;

    // 页面注销（模块开关关）时收割在途「试一下」线程：先停后注销的 join 收束点
    void onStop() override;

private:
    // 打开详情弹窗（shared_ptr keep-alive；同名工具重开保留在途运行态）
    void openDetail(const std::string& name);
    void closeDetail();
    bool detailOpen() const { return detailTool_ != nullptr; }

    // 「试一下」：execute("{}") 丢后台线程，compose 每帧收割（worker 置 done 后
    // requestUiUpdate 主动要帧，不等用户输入）
    void startTryRun();
    void harvestTryRun();
    bool tryRunning() const { return tryRunning_.load(std::memory_order_acquire); }

    // 卡片 / 详情弹窗 / 「记忆」模块行绘制（compose 专用；id 契约 = 测试定位面）
    void drawToolCard(eui::Ui& ui,
                      const std::shared_ptr<::core::mcp::IMcpTool>& tool,
                      const core::theme::Tokens& t,
                      const components::theme::ThemeColorTokens& comp,
                      bool serviceRunning);
    void drawDetailDialog(eui::Ui& ui, const eui::Screen& screen,
                          const core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& comp);
    void drawMemoryModuleRow(eui::Ui& ui, float contentW,
                             const core::theme::Tokens& t,
                             const components::theme::ThemeColorTokens& comp);

    ::core::mcp::McpToolRegistry* registry_;

    // 「记忆」模块行写盘失败反馈：setEnabled fail 时存原因、成功帧清空。
    // 开关本身无内部态（每帧按桥 status 重绘），失败回弹零额外逻辑。
    std::string memorySaveError_;

    // 详情态：keep-alive 指针 + 卡片 hover 集合（compose 每帧重建，成员只做存储）
    std::shared_ptr<::core::mcp::IMcpTool> detailTool_;
    std::set<std::string> hoveredCards_;

    // 在途运行：结果串走互斥锁（worker 写 / compose 读），完成位走原子
    std::thread tryThread_;
    std::atomic<bool> tryRunning_{false};
    std::atomic<bool> tryDone_{false};
    std::atomic<bool> tryError_{false};
    std::mutex tryMutex_;
    std::string tryResultText_;
    std::string tryToolName_;

    void joinTryThread();
};

} // namespace module::mcp
