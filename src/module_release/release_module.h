// 上线模块：两个 Router 页面（任务 / 基础配置），共用一个模块开关 id。
// 工厂声明在 app_registry.h（分组父项 create_release_group_module 恒返回 nullptr）。
#pragma once

namespace module::release {

inline constexpr const char* kReleaseTasksPageKey = "release.tasks";
inline constexpr const char* kReleaseConfigPageKey = "release.config";

} // namespace module::release