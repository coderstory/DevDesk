# memcpp hook 协议契约事实钉版（对应可行性报告未验证项 18）

- 日期：2026-10-27
- 核验对象：可行性报告 §7 第 18 条（`docs/superpowers/research/2026-10-27-memcpp-feasibility.md:445`）——"`additionalContext`、10,000 字符上限、30 秒超时都是当前文档事实，未钉文档版本、无契约测试与回退到 MCP 的方案"。
- 方法：抓官方文档原文（hooks reference / hooks guide / settings）+ 下载官方 changelog 全文本地逐行检索钉版本。核验时 Claude Code 最新版 **2.1.311**。
- 术语先解释：**hook** = Claude Code 预留给外部程序的回调口子，到某个时机执行一段外部逻辑；**matcher** = 配置里的过滤条件，决定某个 hook 对哪些工具触发；**非阻塞错误** = 界面上留一条错误提示但流程继续；**exit 2** = 程序退出码 2，在 Claude Code 里有特殊含义（阻止动作）。

## 一、六条事实逐条核验

### 1. UserPromptSubmit 的 additionalContext 注入机制 —— 仍成立
- 输出格式：hook 以 JSON 返回 `{"hookSpecificOutput": {"hookEventName": "UserPromptSubmit", "additionalContext": "文本"}}`。字段放错层级（放在 JSON 顶层而非 hookSpecificOutput 里）会被**静默忽略**。
  出处（官方文档，2026-10-27 抓取）：https://code.claude.com/docs/en/hooks-guide 原文："if you place it at the top level of the JSON, Claude Code silently ignores it"
- 真的递到模型上下文：Claude Code 把这段文本包进 **system reminder**（系统提醒，一种不显示在聊天界面的内部便签），插到 hook 触发点，模型在下一次请求时读到。
  出处：https://code.claude.com/docs/en/hooks 原文："Claude Code wraps the string in a system reminder and inserts it into the conversation at the point where the hook fired. Claude reads the reminder on the next model request, but it doesn't appear as a chat message in the interface."
- 兜底写法：退出码 0 时，UserPromptSubmit 的普通 stdout 文本（不写 JSON）也会加入上下文；字符串里若含 `<system-reminder>` 字样会被转义成 `&lt;`，防止伪造内部便签（转义行为自 v2.1.292，changelog："Improved hook output handling: `<system-reminder>` tags written in a hook's output are escaped before they reach Claude"）。
- 同一事件多个 hook 都返回 → 全部送达："When several hooks return additionalContext for the same event, Claude receives all of the values."
- 版本锚点：字段自 v1.0.59 存在（changelog："Hooks: UserPromptSubmit now supports additionalContext in advanced JSON output"）。

### 2. 10,000 字符上限 —— 仍成立，超限行为钉版 v2.1.89
- 当前官方原文（https://code.claude.com/docs/en/hooks，2026-10-27）："A hook's `additionalContext`, `systemMessage`, and `initialUserMessage` strings, and its plain stdout, are capped at 10,000 characters"
- 超限不是丢弃：保存到会话目录的一个文件，改传"文件路径 + 前 2,000 字符预览"，模型可以自己去读全文。原文："Claude Code saves the output to a file in the session directory and replaces it with the file path and a preview of up to the first 2,000 characters."
- 这个数字**没有任何配置或环境变量可以调**：原文："this cap has no setting or environment variable to raise it"
- 版本锚点："超限落盘+预览"行为自 v2.1.89（changelog："Changed hook output over 10,000 characters to be saved to disk with a file path + a 2,000-character preview instead of being injected directly into context"）；10,000 这个数字本身更早。
- memcpp 相关：注入预算 ≤5 条 / ≤500 字，距上限约 20 倍余量；上限下调到 1k 以内才会真正影响设计。

### 3. hook 超时 —— 30 秒仍成立；超时=静默放行（不是报错打断）
- 默认值（https://code.claude.com/docs/en/hooks，2026-10-27）：`command`/`http`/`mcp_tool` 三类 hook 一般默认 600 秒；在 `UserPromptSubmit`、`PreModelSwitch`、`PostModelSwitch` 上降为 30 秒；`MessageDisplay` 上 10 秒；`prompt` 类 30 秒；`agent` 类 60 秒；`SessionEnd` 全部共享 1.5 秒预算。原文："Claude Code lowers the `command`, `http`, and `mcp_tool` default to 30 on `UserPromptSubmit`, `PreModelSwitch`, and `PostModelSwitch`, and to 10 on `MessageDisplay`."
- 为什么 UserPromptSubmit 特别短，官方给的理由："Because this hook runs before every prompt and blocks model processing until it completes, a stuck hook stalls the session."
- **超时后行为**（同步 hook）：进程被取消，输出（含 additionalContext）整体丢弃；用户的提问照常送给模型，只是没有注入内容；transcript（按 Ctrl-R 可见的运行记录）里留一条点名通知。原文："a `UserPromptSubmit` command, HTTP, or MCP tool hook that reaches its timeout is canceled and its output, including any `additionalContext`, is discarded. The prompt still reaches Claude without that context."（通知细节自 v2.1.280 改进："Improved the UserPromptSubmit hook timeout notice and the debug log to name which hook command timed out"）
- 想反过来"超时即阻断"要显式配 `onFailure: "block"`（v2.1.295 起）——memcpp **不要配**，默认放行正是想要的失败开放。
- async hook（后台异步 hook）：后台跑起来后不再受 timeout 约束。
- 版本锚点：每 hook 可配 timeout 自 v1.0.41；600 秒默认自 v2.1.3（"Changed tool hook execution timeout from 60 seconds to 10 minutes"）；**"UserPromptSubmit 降为 30 秒"这一条在 changelog 全文（9,578 行）里检索不到引入记录**，只能以当前文档为准——这正是"未钉版"风险的实例。

### 4. PreToolUse 的 matcher —— 只按工具名过滤，不能按文件路径过滤
- matcher 匹配对象是**工具名**：官方文档表格 "PreToolUse | tool name | `Bash`, `Edit\|Write`, `mcp__.*`"。
- 按文件过滤的两条正路（https://code.claude.com/docs/en/hooks，2026-10-27）：
  1. `if` 字段（v2.1.85 引入，changelog："Added conditional `if` field for hooks using permission rule syntax (e.g., `Bash(git *)`)"）：用权限规则语法同时匹配工具名和参数，如 `Edit(*.ts)` 只对 TypeScript 文件触发；Bash 场景解析不出具体命令时会照常触发（v2.1.245 起文档注记）。
  2. hook 内部自己解析 stdin 里的 `tool_input.file_path`：官方保证 Write/Edit/Read 三个文件工具的 file_path 一律绝对路径，Windows 反斜杠会被归一化。
- memcpp 必须知道的文档化盲点：用户用 **@ 引用**方式让 Claude 读文件时，**不触发 PreToolUse Read hook**——官方警告原文："@-referenced files don't trigger PreToolUse Read hooks"。按文件注入会漏掉这类文件，属于协议事实，不是故障。

### 5. hook 配置的版本/迁移口径 —— 官方未文档化（无版本字段、无整体兼容声明）
- settings.json **没有配置格式版本字段**。官方 JSON schema（https://json.schemastore.org/claude-code-settings.json，2026-10-27 抓取）顶层 142 个属性中，与版本相关的只有三个"运行闸门"：`minimumVersion`（防降级）、`requiredMinimumVersion` / `requiredMaximumVersion`（托管配置下版本不符直接拒绝启动）。schema 里另一个 "version" 字段是插件包的"Package version"，与配置格式无关。
- 兼容性口径 = 两条：①官方文档逐特性标注最低版本（如 onFailure 需 v2.1.295）；②changelog 逐条记录行为变化。**没有"旧配置永远兼容"的总声明**。
- 真实发生过的废弃迁移（证明字段会动）：PreToolUse 的顶层 `decision`/`reason` 字段已废弃（旧值 "approve"/"block" 仍被映射成新值 "allow"/"deny"）。官方原文："PreToolUse previously used top-level `decision` and `reason` fields, but these are deprecated for this event."
- 配置可放 6 处：用户级 ~/.claude/settings.json、项目级 .claude/settings.json、.claude/settings.local.json、托管策略、插件 hooks/hooks.json、技能 frontmatter；会话运行中改动由文件监视器热加载。
- 结论：memcpp 侧只能"钉 Claude Code 版本区间"，不存在格式版本协商机制。

### 6. hook 失败/非零退出对用户会话的可见影响 —— 不打断，除了 exit 2
（https://code.claude.com/docs/en/hooks 与 /hooks-guide，2026-10-27）
- 退出码 0：正常，stdout/JSON 按 §1 处理。
- **退出码 2：阻断**。UserPromptSubmit 上 exit 2 = 用户这条提问被拦下，stderr 显示给用户——**这是 hook 唯一能"打断用户"的路径**。memcpp hook 的任何内部错误都必须吞掉并 exit 0，绝不能让 2 冒出来。
- 其他非零退出码 / 启动失败 / 非法 JSON：**非阻塞错误**——界面留一条 "`<hook name> hook error`"（"Failed with non-blocking status code:"）提示，动作照常；UserPromptSubmit 场景下提问照常送达模型，只是没有注入内容。
- HTTP hook（`type:"http"`，直接 POST 到本地常驻服务）：非 2xx 返回或连接失败都是非阻塞错误，动作继续——memcpp 常驻进程挂掉时用户只丢预载、不被打断，与报告"失败开放"设计一致。
- Windows 平台差异（易变区，近期仍在修）：v2.1.309 修 "hooks that run in PowerShell not blocking when the hook's program exits with code 2"；v2.1.310 修 Windows 缺 Git Bash 时 PreToolUse hook 意外放行工具调用。
- 附：Stop hook 连续阻断 8 次会被强制放行（防死循环）；memcpp 不用 Stop hook，仅记录。

## 二、契约风险表

| # | 事实 | 当前值（2026-10-27 核验） | 官方出处 | 变更风险 |
|---|------|--------------------------|----------|----------|
| 1 | additionalContext 字段与投递 | `hookSpecificOutput.additionalContext`，system reminder 形式递模型 | https://code.claude.com/docs/en/hooks ；v1.0.59 引入 | **稳定**（字段名/嵌套长期未动；但有字段级修复史，如 v2.1.110 修 PreToolUse 注入被丢） |
| 2 | 10,000 字符上限 | 10k 硬上限；超限落盘 + 2,000 字预览；无任何开关可调 | https://code.claude.com/docs/en/hooks ；行为钉版 v2.1.89 | **稳定但硬编码**（想感知变化只能靠升级后契约测试） |
| 3 | 超时 | UserPromptSubmit 上 command/http/mcp_tool 默认 30s（一般 600s）；超时=取消+丢弃输出+放行+点名通知 | https://code.claude.com/docs/en/hooks ；600s 自 v2.1.3 | **易变**（官方按事件/类型调默认值是活跃改动区，v2.1.280/295/302 连续动周边；30s 本身无 changelog 引入记录） |
| 4 | matcher 能力 | matcher 只匹配工具名；文件过滤走 `if` 字段（v2.1.85+）或 hook 内解析 tool_input.file_path；@引用文件不触发 Read hook | https://code.claude.com/docs/en/hooks | **稳定**，但"按文件注入"的正解是 if + 内部过滤；@引用盲点是文档化事实，需产品侧接受 |
| 5 | 配置版本口径 | 无格式版本字段；兼容 = 逐特性最低版本注记 + changelog；托管侧可用 requiredMinimum/MaximumVersion 当启动闸门 | https://json.schemastore.org/claude-code-settings.json ；https://code.claude.com/docs/en/settings | **未文档化**（无整体向后兼容声明；唯一迁移先例 = PreToolUse decision/reason 废弃） |
| 6 | 失败可见性 | exit 2 = 阻断提问（唯一打断路径）；其他失败/超时 = 非阻塞通知 + 继续 | https://code.claude.com/docs/en/hooks-guide | **稳定**；Windows 失败模式近期仍有修复（v2.1.309/310），平台差异是易变区 |

## 三、回退方案建议


### MCP recall 兜底的切换条件（任一命中即降级）
1. **事实 1 失守**：additionalContext 字段更名/嵌套变更，或注入内容不再以模型可读形式送达（契约测试断言 1 失败）。
2. **事实 3 失守**：UserPromptSubmit 默认超时降到与注入预算同级（当前设计 95 分位 ≤500ms，30s 是 60 倍余量；若官方降到秒级以下则通道不可用），或超时行为从"放行"变为"阻断"且无法回配。
3. **事实 6 失守**：非零退出从"非阻塞通知"变为"打断会话"。
4. **事实 2 失守**：上限降到 1k 以内（触及 5 条 × 500 字预算），或"落盘+预览"改为截断丢弃。

### 降级路径
- **运行时自检**：每会话 SessionStart 跑一次探针 hook（回显一份合法注入 JSON），确认能送达才在本会话启用注入通道；探针失败 → 本会话只挂 MCP recall（模型按需主动查询），用户无感。
- **部署侧闸门**：托管 settings 的 `requiredMinimumVersion` / `requiredMaximumVersion` 可硬挡破坏契约的版本；个人安装无此硬闸，靠契约测试在升级后首跑时发现并告警。
- **底线不依赖 hook**：MCP recall 在 P0 就交付（报告 §6.1），hook 只是"预载优化"——任何 hook 事实变更都不影响"查得到"。

### P1 契约测试应钉的断言清单（只列断言名与判据）
1. `additional_context_delivered`：UserPromptSubmit 返回合法 JSON → 判据：模型上下文出现该字符串，以 system reminder 形式、非聊天消息。
2. `output_cap_failsafe`：注入 10,501 字符 → 判据：送达内容 = 会话目录文件路径 + ≤2,000 字符预览；注入 9,999 字符 → 判据：全文送达。
3. `timeout_fail_open`：hook 睡 31 秒（不配 timeout，吃默认 30s）→ 判据：提问照常完成、无注入内容、transcript 出现点名超时通知。
4. `failure_never_blocks`：分别以 exit 1、exit 3、进程崩溃、HTTP 连接拒绝四种方式失败 → 判据：会话全部继续、仅非阻塞通知；负向断言：memcpp hook 二进制任何内部异常路径都不产生退出码 2。
5. `matcher_tool_name_only`：matcher 配 `Edit|Write|Read` → 判据：三类工具触发、其他工具不触发；文件过滤由 hook 内解析 `tool_input.file_path` 完成，且断言其为绝对路径。
6. `at_reference_blind_spot`：@引用文件加载 → 判据：PreToolUse Read hook 不触发（记录为已知盲区，不算故障）。
7. `config_hot_reload`：会话中修改 settings 的 hooks 块 → 判据：不重启即生效。
8. `multi_hook_all_delivered`：同事件两个 hook 都返回 additionalContext → 判据：两条都送达。
9. `version_gate`：claude --version 低于钉定下限 → 判据：注入通道自禁、只留 MCP recall。

## 四、与可行性报告相关表述交叉核对

对照 `docs/superpowers/research/2026-10-27-memcpp-feasibility.md`（行号为 2026-10-27 版）：

| 报告原文（file:行） | 核验结论 |
|---|---|
| :61 "UserPromptSubmit、SessionStart、PostToolUse、PreToolUse 都可返回 hookSpecificOutput.additionalContext" | **仍成立**（补充：Stop/SubagentStop 自 v2.1.163 也支持，报告未提但无害） |
| :136 "单条注入字符串上限 10,000 字符（超限不是丢弃：写入 session 文件并给 2,000 字预览）" | **仍成立**，行为可钉版 v2.1.89 |
| :136 "UserPromptSubmit 的 command/http/mcp_tool hook 默认超时从 600 秒降到 30 秒" | **仍成立**（600s 默认自 v2.1.3；30s 特例无 changelog 引入记录，以当前文档为准） |
| :136 "同步 hook 超时会取消并丢弃 additionalContext" | **仍成立** |
| :136 "async hook 不受 timeout 约束" | **仍成立** |
| :136 "其输出本来就不用于注入"（同句后半） | **已变化**：现官方文档明确 "After the background process exits, Claude Code delivers the `additionalContext` and `systemMessage` fields from the hook's JSON response to Claude on the next conversation turn"——async hook 的注入内容会在后台退出后的**下一轮**递给模型，不是"不投递"。:230 同句同判。对 memcpp 的含义：async 注入是"迟一轮"，预载场景不可用，"注入必须同步"的结论反而更硬（:327 仍成立） |
| :137 "官方支持 type:"http" hook" | **仍成立**（settings 另有 allowedHttpHookUrls / httpHookAllowedEnvVars 配套开关） |
| :229 证据表"10,000 字符……文档核实" | **仍成立**，本次补钉 v2.1.89 |
| :445 §7.18 "未钉文档版本、无契约测试与回退到 MCP 的方案" | 本文档关闭"钉版"与"回退方案"两半；契约测试断言清单已给出（第三节），落地待 P1 |

## 引用清单（均为一手来源，检索/抓取日期 2026-10-27）

1. Hooks reference（官方文档）：https://code.claude.com/docs/en/hooks —— 事实 1/2/3/4/6 当前口径的全部引文
2. Hooks guide（官方文档）：https://code.claude.com/docs/en/hooks-guide —— JSON 格式、顶层字段静默忽略、失败行为、配置结构
3. Settings（官方文档）：https://code.claude.com/docs/en/settings —— settings 位置、ConfigChange hook、无版本字段
4. Settings JSON Schema：https://json.schemastore.org/claude-code-settings.json —— minimumVersion / requiredMinimumVersion / requiredMaximumVersion / disableAllHooks / allowedHttpHookUrls / httpHookAllowedEnvVars
5. Claude Code CHANGELOG（官方仓库 main 分支，顶部最新版 2.1.311）：https://raw.githubusercontent.com/anthropics/claude-code/main/CHANGELOG.md —— 版本锚点 v1.0.41、v1.0.59、v2.1.3、v2.1.9、v2.1.85、v2.1.89、v2.1.110、v2.1.163、v2.1.245（文档注记）、v2.1.280、v2.1.292、v2.1.295、v2.1.302、v2.1.309、v2.1.310
