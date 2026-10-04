// src/module_claude_proxy/claude_ui_list.h
// 左栏单元：顶部 header / 引导条 / Profile 列表（背景 + overlay）。
// 统一手法：每块可视区域 = 一个自由函数（状态显式入参、不捕获 this）；元素 id
// 由本单元按原值写死，无头用例（test_claude_ui）按这些 id 断言。
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

// 引导条渲染高。单一来源：drawClaudeGuideBar 按它画，compose 按它给 body 行
// 预留（此前两处各写一份致子项总高越界 4px）；消费方要用 kGuideBarH + gap。
inline constexpr float kGuideBarH = 18.0f;

// 顶部 header：标题 + 状态点/文字 + 副标题 + 右侧代理开关。
// id: claude.header / claude.h1 / claude.status.* / claude.subtitle /
//     claude.btn.proxy
void drawClaudeHeader(eui::Ui& ui, float contentW, float headerH,
                      const core::theme::Tokens& t,
                      bool proxyRunning, const std::string& portStr,
                      const std::function<void()>& onToggleProxy);

// 引导条：代理未运行且本会话从未启动过时一次性提示接管语义；高度恒为 kGuideBarH。
// id: claude.guide.wrap / claude.guide.icon / claude.guide.takeover
void drawClaudeGuideBar(eui::Ui& ui, float contentW,
                        const core::theme::Tokens& t);

// 左栏列表的回调（compose 每帧装配）
struct ProfileListActions {
    std::function<void()> onNewProfile;      // 「+ 新建 Profile」
    std::function<void()> onImportCurrent;   // 「导入当前配置」
    std::function<void()> onImportCcSwitch;  // 「从 cc-switch 导入」（2026-10-03）
    // 右键菜单动作：与既有按钮回调同源的最小委托（claude_module listActions
    // 装配，不新写业务逻辑）。batch-C C-3 起，菜单是这些动作的唯一 UI 入口
    std::function<void(const Profile&)> requestEdit;              // 编辑
    std::function<void(int64_t)> requestDuplicate;                // 建副本
    // 复制配置：入参带 Profile（序列化需要 baseUrl/authField/槽位/extra_env）；右键菜单是唯一入口
    std::function<void(const Profile&)> requestCopyConfig;
    std::function<void(int64_t, const std::string&)> requestDelete;  // 删除
    std::function<void(int64_t)> requestSetActive;                // 设为当前
    // 批 2（设计 §2.10）：共享配置片段动作——区块「新建」/ 卡片点击编辑
    std::function<void()> onNewShared;
    std::function<void(const SharedConfig&)> requestEditShared;
};

// 左栏：Profile 列表（底板 + overlay）。st 为引用：卡片点击/滚动回调写回模块
// 状态，须指向存活到下一帧的对象。sharedConfigs/Error：共享配置片段快照
//（与 profile 列表同滚动容器）
void drawProfileListPanel(eui::Ui& ui, float leftW, float bodyH,
                          const core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& themeTokens,
                          ProfileListState& st,
                          const std::vector<Profile>& profiles,
                          const std::string& profilesError,
                          const std::vector<SharedConfig>& sharedConfigs,
                          const std::string& sharedConfigsError,
                          const ProfileListActions& actions);

// 共享配置区块：标题行 + 片段卡片（name + 「高级属性 JSON 预设」副文案）+
// 「+ 新建片段」按钮。区块钉面板底，高度由调用方按 sharedBlockH(bodyH) 算出并
// 传入（区块恒高，增删片段/出错恢复整列不跳）；片段装不下时块内滚，「+ 新建
// 片段」固定尾部恒可见可点。块内元素一律相对块顶从 0 起算（绝对位置由外层
// claude.shared.block stack 的 .position 决定）。
// st：块内滚动偏移的读写目标（集签名变化时由本函数归零）。
// id: claude.shared.title / claude.shared.scroll / claude.shared.card.N.{item,bg,name,keys}
//     / claude.shared.error / claude.shared.empty / claude.shared.new
void drawSharedConfigBlock(eui::Ui& ui, float width, float blockH,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& themeTokens,
                           ProfileListState& st,
                           const std::vector<SharedConfig>& sharedConfigs,
                           const std::string& sharedConfigsError,
                           const ProfileListActions& actions);

// 左栏 profile 列表右键菜单：编辑 / 复制 / 删除 / 设为当前。常驻构建（关闭态由
// 组件内 opacity 收起）；无右键目标时强制置关（目标与开合标志同生共灭）。锚点
// 来自卡片 onContextMenu 换算的 ctxMenuX_/ctxMenuY_（页面坐标），滚动位置天然正确。
void drawProfileContextMenu(eui::Ui& ui, const eui::Screen& screen,
                            const components::theme::ThemeColorTokens& themeTokens,
                            ProfileListState& st,
                            const std::vector<Profile>& profiles,
                            const ProfileListActions& actions);

} // namespace module::claude
