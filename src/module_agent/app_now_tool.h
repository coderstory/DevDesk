// 当前时间工具 "app.now"：无参数，返回本地时区的时间快照 JSON
// （iso / date / time / 中文 weekday / epoch_ms）。纯读、无副作用、非破坏性，
// 不挂模块生命周期，由 AgentModule 持有注册（与 app.stats 同口径）。
#pragma once

#include <memory>

#include "core/tool.h"

namespace module::agent {

// 工厂：产出 "app.now" 只读工具，无参数
std::shared_ptr<core::ITool> makeAppNowTool();

} // namespace module::agent
