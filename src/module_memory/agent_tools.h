// memory.stats / memory.health：记忆模块只读诊断工具（need 219 F-03/F-04）。
// 注册生命周期跟随 MemoryModule 启用（拍板 3）；proposals.db / usage_ledger.db
// 路径注入（生产 = engine::memoryDataDir()，测试 = 临时目录），不依赖引擎进程
// 存活、不依赖面板是否开过页。
#pragma once

#include <filesystem>
#include <memory>

#include "core/tool.h"

namespace module::memory {

// 三段聚合（提案分组计数 + 用量账面 + 引擎状态一行）
std::shared_ptr<core::ITool> makeMemoryStatsTool(
    const std::filesystem::path& proposalsDb,
    const std::filesystem::path& ledgerDb);

// 引擎快照 + 回环探活（probeEngineHealthy）
std::shared_ptr<core::ITool> makeMemoryHealthTool();

} // namespace module::memory
