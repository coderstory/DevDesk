// panel/recall_eval_data.h
// 冻结评测集（只读资产，源 = tools/memory-lab/experiments/{observations,queries}.json，
// 2026-10-27 冻结版；本文件由该两份 JSON 机械生成，勿手改正文）。
// doc 合成式与 eval_retrieval.py:138-150 逐字同源：title。narrative facts concepts。
#pragma once

#include <array>
#include <cstddef>

namespace module::memory::panel {

struct FrozenQuery {
    const char* id;
    const char* text;
    std::size_t goldCount;
    const int* goldIds;
};

inline constexpr std::size_t kFrozenDocCount = 27;
inline constexpr std::size_t kFrozenQueryCount = 15;

// 27 条冻结记忆 doc（索引 = observations.json 的 id 字段 = gold_ids 的取值域）
inline constexpr std::array<const char*, kFrozenDocCount> kFrozenDocs{{
    "调用 ask-matt 技能。用户中断请求，随后调用 /mattpocock-skills:ask-matt，参数为“接管下任务，然后使用子任务驱动开发”。。[Request interrupted by user] <command-name>/mattpocock-skills:ask-matt</command-name> <command-args>接管下任务，然后使用子任务驱动开发</command-args>。mattpocock-skills:ask-matt 子任务驱动开发",
    "使用子任务开发接管P1。使用 superpowers:subagent-driven-development 接管 P1。。使用 superpowers:subagent-driven-development 接管 P1。subagent-driven-development P1",
    "主会话只编排。主会话只编排，实现交给子任务。。主会话只编排 实现交给子任务。主会话 子任务",
    "使用独立worktree开发MCP。用户要求使用独立 worktree 开发独立的 MCP 模块，将本 app 的能力开放出来。。使用独立worktree开发 开发一个独立的MCP模块 将本app的能力开放出来。worktree MCP模块",
    "MCP默认关闭可设置开启。MCP 模块默认关闭，可以在设置里开启模块；用户可在设置-通用里设置 MCP 端口。。MCP模块默认关闭 可以在设置里开启模块 在设置-通用里设置mcp的端口。MCP 设置",
    "MCP支持Win和macOS。目前需要开发出 MCP 服务的骨架，支持 Windows 和 macOS。。开发MCP服务的骨架 支持win和macos。MCP服务骨架 跨平台",
    "MCP能力抽象接口。MCP 的能力需要抽象出接口，具体的能力通过代码模块插入。。MCP能力抽象出接口 具体能力通过代码模块插入。接口抽象 模块化",
    "先规划后开发代码设计优先。开发流程要求先规划后开发，代码设计高于一切。。先规划后开发 代码设计高于一切。开发流程 代码设计",
    "HelloWorldMCP示例能力。可以先写一些 hello world 的 MCP 能力，比如访问网页，返回网页的 md 版本。。写hello world的MCP能力 访问网页 返回网页的md版本。MCP能力 网页转Markdown",
    "独立worktree开发MCP模块。使用独立 worktree 开发一个独立的 MCP 模块，将本 app 的能力开放出来。。使用独立worktree开发 开发独立MCP模块 将app能力开放出来。worktree MCP 能力开放",
    "MCP端口在设置-通用配置。用户可以在设置-通用里设置 MCP 服务的端口。。端口在设置-通用中设置。MCP 端口 设置-通用",
    "MCP能力抽象接口可插拔。MCP 的能力需要抽象出接口，具体的能力通过代码模块插入。。能力抽象出接口 具体能力通过代码模块插入。接口抽象 模块化 可插拔",
    "先规划后开发设计优先。先规划后开发，代码设计高于一切。。先规划后开发 代码设计高于一切。规划 代码设计",
    "MCP需支持Win和macOS。MCP 服务需要支持 Windows 和 macOS。。支持win 支持macos。跨平台",
    "先做hello world网页转md。先写一些 hello world 的 MCP 能力，例如访问网页并返回网页的 md 版本。。先写hello world MCP能力 示例为访问网页返回md版本。MCP hello world 网页转Markdown",
    "需开发MCP服务骨架。目前需要开发出 MCP 服务的骨架。。开发MCP服务骨架。MCP 骨架",
    "使用独立worktree开发。用户要求使用独立worktree进行开发。。使用独立worktree开发。worktree 开发方式",
    "开发独立MCP模块。开发一个独立的MCP模块，将本app的能力开放出来。。开发一个独立的MCP模块 将本app的能力开放出来。MCP 模块化 能力开放",
    "设置-通用配置MCP端口。用户可以在设置-通用里设置MCP的端口。。用户可以在设置-通用里设置mcp的端口。MCP 端口 设置-通用",
    "开发MCP服务骨架。目前需要开发出MCP服务的骨架，支持win和macos。。目前需要开发出mcp服务的骨架 支持win和macos。MCP服务 Windows macOS",
    "先写hello world MCP能力。先写一些hello world的MCP能力，比如访问网页，返回网页的md版本。。先写一些hello world的mcp能 比如访问网页 返回网页的md版本。MCP能力 网页访问 Markdown",
    "先规划后开发。先规划后开发，代码设计高于一切。。先规划后开发 代码设计高于一切。规划 代码设计 开发原则",
    "DevDesk 新增独立 MCP 模块。DevDesk 新增独立 MCP 模块，默认关闭，设置-通用可配端口，能力抽象接口 + hello world 工具（网页转 markdown），支持 Win/macOS 双平台，在独立 worktree 开发。。默认关闭 设置-通用可配端口 能力抽象接口 + hello world 工具（网页转 markdown） Win/macOS 双平台 独立 worktree 开发。MCP DevDesk",
    "新增独立MCP模块。DevDesk 计划新增独立的 MCP 模块，该模块默认关闭，端口可在设置-通用中配置。。默认关闭 设置-通用可配端口。MCP模块 DevDesk 配置",
    "MCP能力接口与工具。该模块包含能力抽象接口，并提供一个 hello world 工具，用于将网页转换为 markdown。。能力抽象接口 hello world 工具（网页转 markdown）。MCP 网页转markdown 抽象接口",
    "支持Win/macOS双平台。该 MCP 模块需要支持 Windows 和 macOS 双平台。。Win/macOS 双平台。跨平台",
    "独立worktree与先规划。MCP 模块的开发流程要求使用独立 worktree 开发，并遵循先规划后开发的原则。。独立 worktree 开发 先规划后开发。worktree 开发流程 规划",
}};

inline constexpr int kGoldQ01[] = { 0, };
inline constexpr int kGoldQ02[] = { 1, };
inline constexpr int kGoldQ03[] = { 2, };
inline constexpr int kGoldQ04[] = { 3, 9, 16, 26, };
inline constexpr int kGoldQ05[] = { 4, 10, 18, 22, 23, };
inline constexpr int kGoldQ06[] = { 5, 13, 19, 25, };
inline constexpr int kGoldQ07[] = { 6, 11, 24, };
inline constexpr int kGoldQ08[] = { 7, 12, 21, 26, };
inline constexpr int kGoldQ09[] = { 8, 14, 20, 24, };
inline constexpr int kGoldQ10[] = { 3, 9, 16, 26, };
inline constexpr int kGoldQ11[] = { 4, 10, 18, 22, 23, };
inline constexpr int kGoldQ12[] = { 6, 11, 24, };
inline constexpr int kGoldQ13[] = { 7, 12, 21, 26, };
inline constexpr int kGoldQ14[] = { 5, 13, 15, 19, };
inline constexpr int kGoldQ15[] = { 8, 14, 20, 24, };

// 15 条冻结查询（gold = 等价组，命中任一即该查询命中）
inline constexpr std::array<FrozenQuery, kFrozenQueryCount> kFrozenQueries{{
    {"q01", "帮我接管这个任务，然后用拆分子任务的方式推进开发", 1, kGoldQ01},
    {"q02", "P1 阶段能不能交给子任务来接着推进？", 1, kGoldQ02},
    {"q03", "主会话不直接写代码，只负责编排调度可以吗？", 1, kGoldQ03},
    {"q04", "在单独的 worktree 里开发一个 MCP 模块，把应用的能力暴露出去", 4, kGoldQ04},
    {"q05", "MCP 能不能默认不启用，需要时再去设置里打开？", 5, kGoldQ05},
    {"q06", "先把 MCP 服务的骨架搭起来，Windows 和 macOS 都要能用", 4, kGoldQ06},
    {"q07", "MCP 的各项能力要不要先定义成接口，再按模块插进来？", 3, kGoldQ07},
    {"q08", "开发时能不能先把方案规划清楚再动手写代码？", 4, kGoldQ08},
    {"q09", "先做几个简单的 MCP 能力练手，比如抓网页转成 Markdown", 4, kGoldQ09},
    {"q10", "用独立的 worktree 开发一个 MCP 模块，把应用功能开放出来", 4, kGoldQ10},
    {"q11", "MCP 服务的端口应该在哪里配置？", 5, kGoldQ11},
    {"q12", "MCP 的能力是不是应该抽象成接口，方便以后插入新模块？", 3, kGoldQ12},
    {"q13", "能不能坚持先做规划再开发，把代码设计放在第一位？", 4, kGoldQ13},
    {"q14", "这个 MCP 服务需要同时兼容 Windows 和 macOS 吗？", 4, kGoldQ14},
    {"q15", "先写个 hello world 级别的 MCP 能力，访问网页并返回 Markdown 版", 4, kGoldQ15},
}};

}  // namespace module::memory::panel
