// 智能体模块·工具块（AgentModule 成员函数定义）：「可用工具」注册表快照的惰性
// 缓存（ensureToolsCache）+ 系统通知授权的一次性请求时机
//（requestNotificationAuthorizationOnce，挂 pollRunEnd 的启动通道）。
// 拆分（04 号票）产物：类定义在 agent_module.h，跨 TU 共享助手在
// agent_module_internal.h；本文件只承载成员函数体，纯代码搬移，逻辑与签名零变化。

#include "agent_module_internal.h"

namespace module::agent {

// 「可用工具」注册表快照（惰性装载；展开时强制重装反映模块启停）
void AgentModule::ensureToolsCache(bool forceReload) {
    if (toolsCacheLoaded_ && !forceReload) return;
    toolsCache_ = listRegisteredTools();
    toolsCacheLoaded_ = true;
}

// 授权弹窗时机：Module::onStart 在窗口可见之前（Router::startAll 于装配期执行），
// 直接放 onStart 会让 macOS 授权框在无前台窗口时出现。pollRunEnd 是 app::compose 在
// splash 结束后每帧调用的现有启动通道，此时窗口已可见；因此授权请求挂在这里。
// 只请求一次：成员闸跨 onStop/onStart 保留（系统对已决定状态本就幂等）。
void AgentModule::requestNotificationAuthorizationOnce() {
    if (notificationAuthorizationRequested_) return;
    notificationAuthorizationRequested_ = true;
    platform::notify::requestAuthorization();
}

} // namespace module::agent
