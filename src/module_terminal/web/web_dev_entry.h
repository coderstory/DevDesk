// 网页终端服务开发测试入口（need#490 W3，技术卡 §12.4 / 实现计划 §1.11）。
// 唯一的 #ifdef 落点：DEVDESK_WEB_AUTOSTART / DEVDESK_WEB_PORT / DEVDESK_WEB_TEST_HOOKS
// 只在本 TU 的 Debug 分支编译；release 构建这些符号不产生行为（readDevEntryConfig 返回默认）。
#pragma once

#include <string>

namespace module::terminal::web {

struct DevEntryConfig {
    bool autostart = false;
    int port = 0;       // 0 = 用服务默认端口
    bool testHooks = false;
};

// release：恒返回默认值（自动开服、固定端口、测试钩子全部不可用）
DevEntryConfig readDevEntryConfig();

// 测试用：release 下为 false
bool devEntryAvailable();

} // namespace module::terminal::web
