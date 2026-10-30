# 任务：需-7 笔记 MCP 最小三工具（notes_search / notes_read / notes_append→新文件路由）

mode: edit
从 dev 新建分支 wt-notes-mcp，worktree 内改码，构建/测试/提交由派单方收尾。

## 目标
Claude Code 经 MCP 能检索和读取 DevDesk 笔记，并把产出写成新文件进笔记库（会议纪要/结论可被后续会话引用）。

## 设计红线（深挖已裁决，照做不许自由发挥）
1. 复用现成缝，零检索逻辑：搜索= `MarkdownService::search`（markdown_service.cpp:870，FTS5 拉丁词 + LIKE 兜底 CJK 双路）；写入= `MarkdownService::write`（:636，原子写+索引同步）
2. 接线模式照抄 `memory_mcp_bridge.cpp` / `mcp_service_bridge.cpp` 既有样板
3. **写路径只走「新文件路由」**：MCP 只往独立目录建新文件（`来自Claude/YYYY-MM-DD-主题.md`），**禁改写/追加任何已有文件**——编辑器 400ms 自动保存持内存全文，会整个覆盖 MCP 写入（10-30 深挖实证的坑）；「追加到已有笔记」留二期（前置=编辑器 mtime 失配检测），本单不做
4. 新文件落既有 git 自动提交链即白拿历史回溯，不另做

## 非目标
不做整篇改写、不做已有文件追加、不做笔记 UI 改动、不碰 core

## 验收标准（回单自证）
- F-01 MCP 调 notes_search("中文关键词") 命中中文标题（LIKE 路径断言）
- F-02 notes_read 返回指定笔记全文
- F-03 notes_write 落新文件 → FTS 索引同步可搜到 → git log 有一条自动提交
- F-04 对「已存在文件」的写请求被拒并返回明确错误（防编辑器覆盖冲突的红线用例）
- F-05 定向用例全绿 + 先红后绿证据

## 回单
sha + 文件清单 + 说明三件套；报告极简风格。
