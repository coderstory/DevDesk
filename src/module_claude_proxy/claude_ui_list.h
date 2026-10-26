// src/module_claude_proxy/claude_ui_list.h
// 左栏单元：header / 引导条 / Profile 列表。每块可视区域 = 一个自由函数（状态显式
// 入参）；元素 id 按原值写死，无头用例（test_claude_ui）按这些 id 断言。
// 共享配置片段区块已删除（左栏只剩 Profile 列表与底部按钮）。
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

// 引导条渲染高。单一来源：drawClaudeGuideBar 按它画，compose 按它给 body 行预留。
inline constexpr float kGuideBarH = 18.0f;

// 顶部 header：标题 + 状态点/文字 + 代理开关（UAT6 副题横扫：claude.subtitle 已删）。
// id: claude.header / claude.h1 / claude.status.* / claude.btn.proxy
void drawClaudeHeader(eui::Ui& ui, float contentW, float headerH,
                      const core::theme::Tokens& t,
                      bool proxyRunning, const std::string& portStr,
                      const std::function<void()>& onToggleProxy);

// 引导条：代理未运行且本会话从未启动过时一次性提示接管语义；高度恒为 kGuideBarH。
// st 为引用：关闭按钮写回 guideDismissed_。
// id: claude.guide.wrap / claude.guide.icon / claude.guide.takeover / claude.guide.close
void drawClaudeGuideBar(eui::Ui& ui, float contentW,
                        const core::theme::Tokens& t,
                        ProfileListState& st);

// 左栏列表的回调（compose 每帧装配）
struct ProfileListActions {
    std::function<void()> onNewProfile;      // 「+ 新建 Profile」
    std::function<void()> onImportCcSwitch;  // 「从 CC 导入」（UAT5 文案缩短）
    // 右键菜单动作：菜单是这些动作的唯一 UI 入口
    std::function<void(const Profile&)> requestEdit;              // 编辑
    std::function<void(int64_t)> requestDuplicate;                // 建副本
    // 复制配置：入参带 Profile（序列化需要 baseUrl/authField/槽位/extra_env）；右键菜单是唯一入口
    std::function<void(const Profile&)> requestCopyConfig;
    std::function<void(int64_t, const std::string&)> requestDelete;  // 删除
    std::function<void(int64_t)> requestSetActive;                // 设为当前
};

// st 为引用：卡片点击/滚动回调写回模块状态，须指向存活到下一帧的对象。
void drawProfileListPanel(eui::Ui& ui, float leftW, float bodyH,
                          const core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& themeTokens,
                          ProfileListState& st,
                          const std::vector<Profile>& profiles,
                          const std::string& profilesError,
                          const ProfileListActions& actions);

// Profile 右键菜单：编辑 / 复制 / 删除 / 设为当前。常驻构建（关闭态由 opacity 收起）；
// 无右键目标时强制置关。锚点来自卡片 onContextMenu 换算（页面坐标）。
void drawProfileContextMenu(eui::Ui& ui, const eui::Screen& screen,
                            const components::theme::ThemeColorTokens& themeTokens,
                            ProfileListState& st,
                            const std::vector<Profile>& profiles,
                            const ProfileListActions& actions);

// 切换 Profile 二次确认弹窗（UAT6）：点非选中卡先弹确认，确定后才改 selectedId_
void drawSwitchConfirmDialog(eui::Ui& ui, const eui::Screen& screen,
                              const core::theme::Tokens& t,
                              const components::theme::ThemeColorTokens& themeTokens,
                              ProfileListState& st);

} // namespace module::claude
