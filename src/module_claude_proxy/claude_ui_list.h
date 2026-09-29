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

// 引导条渲染高（§6.4）。**单一来源**：drawClaudeGuideBar 按它画，claude_module
// 的 compose 按它给 body 行预留 —— 此前两处各写一份（绘制 18 / 预留 30），
// 预留值悄悄偏离了被预留对象，根 column 的子项总高超出可用高 4px（底部 padding
// 被压缩，macOS 布局审计 finding: claude_module.cpp:86）。预留必须 = 本值 +
// 根 column 的 spacingLarge（两行之间有间距），故消费方要用 kGuideBarH + gap。
inline constexpr float kGuideBarH = 18.0f;

// 顶部 header：标题 + 状态点/文字 + 副标题 + 右侧代理开关。
// id: claude.header / claude.h1 / claude.status.* / claude.subtitle /
//     claude.btn.proxy
void drawClaudeHeader(eui::Ui& ui, float contentW, float headerH,
                      const core::theme::Tokens& t,
                      bool proxyRunning, const std::string& portStr,
                      const std::function<void()>& onToggleProxy);

// 引导条（§6.4）：代理未运行且本会话从未启动过时一次性提示接管语义。
// 高度恒为 kGuideBarH（占位口径与绘制口径同源，见上）。
// id: claude.guide.wrap / claude.guide.icon / claude.guide.takeover
void drawClaudeGuideBar(eui::Ui& ui, float contentW,
                        const core::theme::Tokens& t);

// 左栏列表的回调（compose 每帧装配）
struct ProfileListActions {
    std::function<void()> onNewProfile;      // 「+ 新建 Profile」
    std::function<void()> onImportCurrent;   // 「导入当前配置」
    // 右键菜单动作（UI 现代化 M1-5）：与既有按钮回调同源的最小委托
    //（claude_module listActions 装配：编辑=openEditForm / 建副本=doDuplicate /
    // 删除=确认弹窗 / 设为当前=doSetActive——不新写业务逻辑）
    std::function<void(const Profile&)> requestEdit;              // 编辑
    std::function<void(int64_t)> requestDuplicate;                // 建副本
    // 复制配置（2026-09-30 batch-fix C，§5⑤）：入参带 Profile（序列化需要
    // baseUrl/authField/槽位/extra_env，不是 id 能表达的）；与详情第 7 键
    // 同源 doCopyConfig
    std::function<void(const Profile&)> requestCopyConfig;
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
//（name + N 个键）+「+ 新建片段」按钮。
// 2026-09-28（组 B P4）：区块**钉面板底**（底沿 = 底部按钮行上沿 − 12），
// 高度由调用方按 sharedBlockH(bodyH, 片段数, 是否有错) 算出并传入（调用方需要
// 它才能排列表视口 —— 高度只有一个来源）；片段装不下时在区块内部滚，卡片全部
// 可达、「+ 新建片段」是固定尾部恒可见可点。
// 坐标系（审计 F-08 改准）：块内元素一律**相对块顶**从 0 起算 —— 块的绝对位置
// 由外层 `claude.shared.block` stack 的 .position 决定，本函数不持偏移。
// st：块内滚动偏移的读写目标（集签名变化时由本函数归零，见 .cpp）。
// id: claude.shared.title / claude.shared.scroll / claude.shared.card.N.{item,bg,name,keys}
//     / claude.shared.error / claude.shared.empty / claude.shared.new
void drawSharedConfigBlock(eui::Ui& ui, float width, float blockH,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& themeTokens,
                           ProfileListState& st,
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
