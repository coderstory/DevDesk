// src/module_settings/github_sync_exit_hook.h
// 进程退出钩子：main 的 atexit 经此调用设置模块的退出补推。
// 注册须晚于 app 装配层的 armExitHook（atexit LIFO → 本钩子先跑），
// 此时模块与数据库都还活着；推不完由 settings onStop 的 join 收束。
// 模块未启动 / 已 onStop / 同步未启用 / 不 dirty → no-op（实现侧守卫）。
#pragma once

namespace module::settings {

void runGithubSyncExitPush();

}  // namespace module::settings
