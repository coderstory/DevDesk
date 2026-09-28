// src/module_claude_proxy/claude_ui_list.h
// R4（P-B4）左栏单元：顶部 header / 引导条 / Profile 列表（背景 + overlay：
// 标题、错误/空态、滚动卡片、底部两键）。
//
// 统一手法（spec §5.2）：每块可视区域 = 一个自由函数，签名 =（ui, 几何,
// tokens, 只读输入 / 引用状态, 回调）；状态**显式入参**、不捕获 this；元素 id
// 由调用方（本单元）按原值写死 —— 无头用例（test_claude_ui）按这些 id 断言。
//
// 零行为变化：节点 id、文案、字号、颜色、几何、控制流逐字保留（P-B4 只做
// 「搬出 compose + 把 this 成员换成显式入参」）。
#pragma once

#include "claude_service.h"
#include "claude_ui_state.h"

#include <components/theme.h>
#include <core/theme.h>
#include <eui/dsl.h>

#include <functional>
#include <string>
#include <vector>

namespace module::claude {

// 顶部 header：标题 + 状态点/文字 + 副标题 + 右侧代理开关。
// id: claude.header / claude.h1 / claude.status.* / claude.subtitle /
//     claude.btn.proxy
void drawClaudeHeader(eui::Ui& ui, float contentW, float headerH,
                      const core::theme::Tokens& t,
                      bool proxyRunning, const std::string& portStr,
                      const std::function<void()>& onToggleProxy);

// 引导条（§6.4）：代理未运行且本会话从未启动过时一次性提示接管语义。
// id: claude.guide.wrap / claude.guide.icon / claude.guide.takeover
void drawClaudeGuideBar(eui::Ui& ui, float contentW,
                        const core::theme::Tokens& t);

// 左栏列表的回调（compose 每帧装配）
struct ProfileListActions {
    std::function<void()> onNewProfile;      // 「+ 新建 Profile」
    std::function<void()> onImportCurrent;   // 「导入当前配置」
    // 右键菜单动作（UI 现代化 M1-5）：与既有按钮回调同源的最小委托
    //（claude_module listActions 装配：编辑=openEditForm / 复制=doDuplicate /
    // 删除=确认弹窗 / 设为当前=doSetActive——不新写业务逻辑）
    std::function<void(const Profile&)> requestEdit;              // 编辑
    std::function<void(int64_t)> requestDuplicate;                // 复制
    std::function<void(int64_t, const std::string&)> requestDelete;  // 删除
    std::function<void(int64_t)> requestSetActive;                // 设为当前
    // 批 2（设计 §2.10）：共享配置片段动作——区块「新建」/ 卡片点击编辑
    std::function<void()> onNewShared;
    std::function<void(const SharedConfig&)> requestEditShared;
};

// 左栏：Profile 列表（含底板 + overlay 两块，调用点在 claude.body row 内）。
// st 为引用：卡片点击 / 滚动回调写回模块状态，故须指向存活到下一帧的对象
//（生产 = ClaudeUiState::list 成员）。
// sharedConfigs / sharedConfigsError：共享配置片段快照（批 2，「共享配置」
// 区块数据源；与 profile 列表同滚动容器）
void drawProfileListPanel(eui::Ui& ui, float leftW, float bodyH,
                          const core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& themeTokens,
                          ProfileListState& st,
                          const std::vector<Profile>& profiles,
                          const std::string& profilesError,
                          const std::vector<SharedConfig>& sharedConfigs,
                          const std::string& sharedConfigsError,
                          const ProfileListActions& actions);

// 共享配置区块（批 2，设计 §2.10「片段列表入口」）：标题行 + 片段卡片
//（name + N 个键）+「+ 新建片段」按钮。与 profile 列表同滚动容器（scrollView
// content 尾部整块；profiles 空/错时垫在空态/错误态下方）。y = 区块起始
//（scroll 内 = 相对 0）；返回区块总高（滚动容器按此撑高）。
// id: claude.shared.title / claude.shared.card.N.{bg,name,keys} / claude.shared.new
float drawSharedConfigBlock(eui::Ui& ui, float y, float width,
                            const core::theme::Tokens& t,
                            const std::vector<SharedConfig>& sharedConfigs,
                            const std::string& sharedConfigsError,
                            const ProfileListActions& actions);

// 左栏 profile 列表右键菜单（UI 现代化 M1-5）：编辑 / 复制 / 删除 / 设为当前。
// 常驻构建（关闭态由组件内 opacity 收起）；无右键目标时强制置关
//（4523e09 的「隐形菜单卡开」同款守卫——目标与开合标志同生共灭）。
// 锚点来自卡片 onContextMenu 时经 core::menuAnchorForPointer 换算的
// ctxMenuX_/ctxMenuY_（页面坐标），滚动位置天然正确（指针即视觉位置）。
void drawProfileContextMenu(eui::Ui& ui, const eui::Screen& screen,
                            const components::theme::ThemeColorTokens& themeTokens,
                            ProfileListState& st,
                            const std::vector<Profile>& profiles,
                            const ProfileListActions& actions);

} // namespace module::claude
