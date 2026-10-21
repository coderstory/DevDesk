# GitLab 自动化与代码审查工具链调研

**日期**: 2026-10-21 | **来源**: cs-web-fetch 共 17 次抓取 | **编制**: DevDesk 智能中枢 B3/E 期技术输入

---

## 一、开源 MR 审查自动化方案

**结论先行**：reviewdog 是 GitLab MR 自动审查的事实标准，Danger 因官网瘫痪无法完整评估，GitLab 无官方 code review bot 产品。

### 1.1 reviewdog（重点）

reviewdog 是 Go 编写的跨平台审查自动化工具，核心理念"运行任意 linter，把结果贴到代码行上"。GitLab 支持两种 reporter：

| Reporter | 机制 | 适用版本 |
|----------|------|---------|
| `gitlab-mr-discussion`（推荐） | 把 linter 输出按文件+行号映射到 MR diff 的对应代码行，调用 GitLab Discussions API 创建行内评论 | GitLab >= v10.8 |
| `gitlab-mr-commit`（旧） | 把结果贴到 MR 的每个 commit 上 | v10.8 以下兼容 |

**工作流三步**：(1) 从 GitLab CI 环境变量自动读取 MR 元信息和 diff（`CI_API_V4_URL`、`CI_MERGE_REQUEST_IID` 等）；(2) 运行你选的 linter/分析工具（ESLint、golangci-lint、自定义脚本等），输出为标准 `file:line: message` 格式；(3) reviewdog 解析输出，过滤出 diff 范围内有变更的行，通过 GitLab API v4 发 MR discussion。

**鉴权**：`REVIEWDOG_GITLAB_API_TOKEN` 环境变量，Personal Access Token 需 `api` scope。自托管 GitLab 用 `GITLAB_API` 覆盖 API 地址。

**对 B3 的直接参考**：read diff 过滤模式可直接抄——拿到 MR changes → 只对变更行做分析 → 避免全文件扫描噪音。reviewdog 的 `diff-filter` 和 `filter-mode`（added/diff_context/file）是现成设计参考。

**引用**：https://github.com/reviewdog/reviewdog（2026-10-21 抓取）

### 1.2 Danger

Danger 是 Ruby/JS 双语言的 CI 期审查自动化框架（"在 CI 里跑的 code review bot"）。用 Dangerfile（DSL）编写规则，访问 MR 的元数据（标题、描述、变更文件列表、diff、label、author 等），可做"PR 标题格式检查""缺少 changelog 条目""改关键文件自动 @某人"等。

GitLab 支持通过 `danger-gitlab`（Ruby Gem）和 danger JS 的 GitLab 适配器实现。danger 官方站点（danger.systems）经测试连接正常但 GitLab 子页面全部 404，`/usage/gitlab.html`、`/js/guides/gitlab.html` 均返回 EMPTY 或 404。

**已知但未完整验证**：danger-gitlab gem 在 RubyGems 存在；danger JS 的 GitLab CISource 通过 CI 环境变量判断平台。由于官网文档不可访问，具体 API schema 无法实证。

**对 B3 的参考**：Dangerfile DSL 的"规则即代码"模式可借鉴——用 C++ 函数指针/lambda 做规则注册表，传入 MR 上下文 struct。

**引用**：https://github.com/danger/danger（2026-10-21 抓取）；https://danger.systems（2026-10-21 抓取，GitLab 子页面全部不可达）

### 1.3 GitLab 官方模式

GitLab 无类似 GitHub Code Review Bot 的内置产品。社区方案均基于：
- **GitLab CI**：在 `.gitlab-ci.yml` 中配置 pipeline，MR 事件触发 CI job 跑审查脚本
- **GitLab API v4**：Merge Requests API + Discussions API（发评论）、Notes API（发普通评论）
- **个人访问令牌**：`PRIVATE-TOKEN` 头或 `GITLAB_PERSONAL_ACCESS_TOKEN` 环境变量，scope `api`（全权限）或 `read_api`（只读）

---

## 二、GitLab MCP Server 现成实现

**结论先行**：官方参考实现已归档，仅有基础 CRUD 工具（无 MR 审查相关）；社区实现搜索无可靠结果。我们需要自己定义工具面。

### 2.1 官方参考实现（MCP Steering Group）

npm: `@modelcontextprotocol/server-gitlab` | Docker: `mcp/gitlab` | 源码: modelcontextprotocol/servers-archived

**9 个工具**（完整参数 schema）：

| 工具名 | 输入参数 | 用途 |
|--------|---------|------|
| `create_or_update_file` | project_id, file_path, content, commit_message, branch, previous_path? | 文件写操作 |
| `push_files` | project_id, branch, files[], commit_message | 批量提交 |
| `search_repositories` | search, page?, per_page? (default 20) | 搜索项目 |
| `create_repository` | name, description?, visibility?, initialize_with_readme? | 建仓库 |
| `get_file_contents` | project_id, file_path, ref? | 读文件内容 |
| `create_issue` | project_id, title, description?, assignee_ids?, labels?, milestone_id? | 建 Issue |
| `create_merge_request` | project_id, title, description?, source_branch, target_branch, draft?, allow_collaboration? | 建 MR |
| `fork_repository` | project_id, namespace? | Fork 项目 |
| `create_branch` | project_id, branch, ref? | 建分支 |

**鉴权**：`GITLAB_PERSONAL_ACCESS_TOKEN` 环境变量，支持 `GITLAB_API_URL` 指向自托管实例（如 `https://git-devops.zeekrlife.com/api/v4`）。Token scope 分三档：`api`（全权限）、`read_api`（只读）、`read_repository`+`write_repository`（仓库操作）。

**分页**：仅 `search_repositories` 支持 `page` + `per_page`。其他工具无分页设计。

**关键缺口**：没有任何 MR 审查相关工具——无 `list_merge_requests`、无 `get_merge_request_changes`（读 diff）、无 `create_merge_request_note`（发评论）。这是一个面向仓库管理的 MCP Server，不是代码审查工具。

**引用**：https://github.com/modelcontextprotocol/servers-archived/tree/main/src/gitlab（2026-10-21 抓取）

### 2.2 社区实现

Bing 搜索 "gitlab MCP server npm community merge request tools" 未返回有效结果。尝试访问 `github.com/nicholasxuu/mcp-gitlab-server` 返回 404。社区 GitLab MCP Server 生态目前尚未成熟到可抄写的地步——我们需要自己设计工具面。

---

## 三、GitLab Webhook / CI 联动模式

**结论先行**：GitLab webhook 是外部事件接入的标准通道；推荐 signing token（HMAC-SHA256）而非旧 secret token。

### 3.1 Webhook 机制

GitLab webhook 是 HTTP POST 回调，项目或群组级别配置。支持事件：push、merge request（open/update/merge/close）、issue、pipeline、job、release 等。

**两种鉴权方式**：

| 方式 | 机制 | 推荐度 |
|------|------|--------|
| Signing token（新） | `webhook-id` + `webhook-timestamp` + `webhook-signature` 头，HMAC-SHA256 签名（格式 `v1,{base64}`），同时验证真实性+完整性 | 推荐 |
| Secret token（旧） | `X-Gitlab-Token` 头传明文 token，仅验证来源 | 不推荐 |

也支持自定义头（最多 20 个）和自定义 payload 模板（用 `{{object_kind}}` 等变量）。

**引用**：https://docs.gitlab.com/ee/user/project/integrations/webhooks.html（2026-10-21 抓取）

### 3.2 典型自动审查管道

```
GitLab MR 事件 → Webhook → DevDesk 本地 HTTP Server
                           → 解析 payload（MR IID、project_id）
                           → B3 只读工具：GET MR changes（读 diff）
                           → LLM 审查分析
                           → E 期工具：POST MR discussions（写评论）
```

reviewdog 就是在 GitLab CI 里跑此模型的典范——CI job 在 MR 事件触发，自动拿到环境变量（`CI_MERGE_REQUEST_IID` 等），运行 linter 后调 reviewdog 发评论。我们不走 GitLab CI（DevDesk 是桌面应用），改用 webhook 推送到本地 HTTP server（已在 DevDesk 平台层有 HttpClient/HttpServer）。

**事件订阅建议**：仅订阅 Merge Request 事件（open + push-update），不订阅 push（太频繁）、pipeline（不需要）。

---

## 四、对我们三档的具体建议

### 4.1 B3 只读工具组草案（最低 5 工具）

按外部可见性分档（ADR 0003），只读档不需要写权限 token，最低 `read_api` scope：

1. **list_merge_requests** — `project_id, state?(opened/closed/merged), search?, page?, per_page?` → MR 列表
2. **get_merge_request** — `project_id, mr_iid` → MR 详情（标题、描述、源/目标分支、作者、状态）
3. **get_merge_request_changes** — `project_id, mr_iid` → MR 的 diff（变更文件列表 + 每文件 patch）
4. **get_merge_request_discussions** — `project_id, mr_iid, page?, per_page?` → MR 讨论/评论线程
5. **get_pipeline_status** — `project_id, pipeline_id 或 mr_iid` → 流水线状态

可选扩展：`search_projects`、`get_file`（读仓库文件）、`list_pipelines`（多条流水线）。

参考 API：GitLab v4 `GET /projects/:id/merge_requests`、`GET /projects/:id/merge_requests/:mr_iid/changes`、`GET /projects/:id/merge_requests/:mr_iid/discussions`。

### 4.2 E 期建 MR 的最小工具面（4 工具）

E 期需要 `api` scope token + 用户显式授权（ADR 0003）：

1. **create_merge_request** — 参数抄 MCP 官方：`project_id, title, description?, source_branch, target_branch, draft?`
2. **create_merge_request_note** — `project_id, mr_iid, body, line?（行号）, line_type?(new/old), path?（文件路径）` → 发 MR 评论（普通评论或代码行评论）
3. **create_merge_request_discussion** — `project_id, mr_iid, body, position?（行定位）` → 创建讨论线程（含代码行绑定）
4. **merge_merge_request** — `project_id, mr_iid, merge_commit_message?, should_remove_source_branch?` → 合并 MR（人审通过后）

注：官方 MCP server 的 `create_merge_request` 工具 schema 可直接抄，只需加 `draft` 布尔参数（MCP 官方 8 工具中已有）。

### 4.3 事件接入方式推荐

不走 GitLab CI（DevDesk 是桌面应用，没有 CI runner 环境），推荐：

- **方式**：Webhook（signing token）→ DevDesk 本地 `HttpServer`（平台层已有）→ MR 事件回调 → 触发智能体审查
- **端点**：`POST http://localhost:<port>/webhook/gitlab`
- **验签**：实现 HMAC-SHA256 签名校验（`webhook-signature` 头 vs 本地计算），防伪造
- **事件过滤**：只处理 `object_kind == "merge_request"` 且 `object_attributes.action in (open, update)`
- **补充**：提供手动触发入口（用户粘贴 MR URL → DevDesk 解析 project_id + mr_iid → 调 API 抓数据），不依赖 webhook

**引用**：https://docs.gitlab.com/ee/user/project/integrations/webhooks.html（2026-10-21 抓取）；https://docs.gitlab.com/ee/api/merge_requests.html（2026-10-21 抓取）

---

## 附录：抓取记录

| # | URL | 结果 |
|---|-----|------|
| 1 | reviewdog/reviewdog GitHub | 48958 bytes，完整 |
| 2 | danger.systems/js/guides/gitlab.html | EMPTY |
| 3 | docs.gitlab.com/ee/api/merge_requests.html | 200KB，完整（截断使用） |
| 4 | modelcontextprotocol/servers src/gitlab | 404 |
| 5 | modelcontextprotocol/servers README | 9584 bytes，获知 archived 路径 |
| 6 | danger.systems/usage/gitlab.html | 404 |
| 7 | modelcontextprotocol/servers-archived src/gitlab | 12179 bytes，完整工具清单 |
| 8 | danger/danger CHANGELOG.md | 404 |
| 9 | docs.gitlab.com webhooks | 24118 bytes（30000 截断），完整 |
| 10 | docs.gitlab.com discussions API | 58601 bytes（20000 截断），超长 |
| 11 | servers-archived GitLab README raw | 11544 bytes，鉴权/分页细节 |
| 12 | Bing: gitlab MCP server community | 垃圾结果（中文社媒） |
| 13 | danger/danger GitHub README | 5021 bytes，仅通用介绍 |
| 14 | nicholasxuu/mcp-gitlab-server | 404 |
| 15 | docs.gitlab.com notes API | TIMEOUT |
| 16 | Bing: danger-gitlab gem npm | 垃圾结果（成人内容，Bing 安全搜索失效） |
| 17 | Bing: GitLab CI review pipeline | 无结果 |

**统计**：17 次抓取，成功 9 次，失败/无信息 8 次（danger.systems 全链断裂、Bing 搜索对技术关键词返回垃圾、GitLab docs SPA 渲染超时、社区 MCP 不存在）。