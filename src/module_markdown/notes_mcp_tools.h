// src/module_markdown/notes_mcp_tools.h
// notes_search / notes_read / notes_write MCP 工具（core::mcp 接缝的
// module_markdown 实现，module_memory/mcp/search_tool 同款接线）。检索/读取直走
// MarkdownService 既有缝（search 的 FTS5 拉丁词 + LIKE CJK 兜底双路 / read 全文），
// 零新增检索逻辑；注册/注销挂 MarkdownModule onStart/onStop（core/mcp_tool.h 头注
// 明的标准模式，markdown_tools 同款生命周期）。
//
// 写路径红线（需-7 深挖裁决）：只走「新文件路由」——固定落
// 来自Claude/日期-主题.md，目标已存在一律拒绝（编辑器 400ms 自动保存持内存全文，
// 会整个覆盖外部写入）；「追加/改写已有笔记」留二期（前置 = 编辑器 mtime 失配
// 检测）。新文件经 NoteGitAutoCommit::onFileSaved 收编进既有 git 自动提交链，
// 白拿历史回溯，不另做。
//
// 线程契约：execute 在 MCP HTTP worker 线程跑（IMcpTool 契约）——不触 UI；
// SqliteDb 自带 recursive_mutex（watcher 线程与本缝同款并发现状），服务调用
// 连接级串行安全。
//
// 模块隔离：只 include core 与 module_markdown 自有头。
#pragma once

#include <memory>
#include <string>

#include "core/mcp_tool.h"
#include "markdown_service.h"
#include "md_note_git.h"

namespace module::markdown::mcp {

inline constexpr char kNotesSearchToolName[] = "notes_search";
inline constexpr char kNotesReadToolName[] = "notes_read";
inline constexpr char kNotesWriteToolName[] = "notes_write";

// 新文件路由目录（MCP 写唯一落点；主题只作文件名，不接受路径）
inline constexpr char kNotesMcpInboxDir[] = "来自Claude";

// 注册三工具进 registry（gitAutoCommit 以 shared 持有：模块 onStop 释放后，
// 在途 execute 的写后提交链不悬垂；可空 = 不触发自动提交链，测试注入用）。
// 重名 fail 原样上抛（注册表既有语义），调用方 LOG_ERROR 即可。
core::Result<void> registerNotesMcpTools(core::mcp::McpToolRegistry& registry,
                                         MarkdownService& service,
                                         std::shared_ptr<NoteGitAutoCommit> gitAutoCommit);

// 注销三工具（名字不存在 = no-op，可安全重复调用）
void unregisterNotesMcpTools(core::mcp::McpToolRegistry& registry);

// 本地日期 YYYY-MM-DD（core::time::localTime，无平台分支）
std::string notesMcpTodayYmd();

// 日期 + 主题 → 新文件 POSIX 相对路径（纯函数，测试直测路由规则）
std::string notesMcpRelPath(const std::string& ymd, const std::string& topic);

}  // namespace module::markdown::mcp
