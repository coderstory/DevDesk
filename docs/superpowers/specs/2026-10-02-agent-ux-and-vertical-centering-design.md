# 智能体 UX 重做 + 垂直居中根治 + 调用记录性能（2026-10-02）

> **来源**：用户 2026-10-02 真机反馈 7 条。**依据代码实锤**，非猜测（除 §4 调用记录性能外）。
> **交付方式**：三个并行 worktree，各自 TDD + 独立 opus review，串行合并后全量构建安装 + 静态审计。

## 0. 问题清单与根因（全部实锤，除 §4）

| # | 用户原话 | 根因（file:line） | 性质 |
|---|---|---|---|
| 1 | 新建智能体任务没有触发槽位之类的配置项 | `agent_module.cpp:519` `createTaskFromForm` 硬编码 `trigger="manual" / slot="default" / iter=10 / destructive=false`；页面只有 goal 输入框（:936-968）。全字段表单只存在于编辑流（:1109-1280，`editingTaskId_` 非空才渲染） | 设计缺口 |
| 2 | 创建任务「统计笔记数量」执行后啥都没输出 | run 14 `status=completed`、`iterations=2`、最终回复完整入库（真机 DB `~/DevDesk/devdesk.db`：`agent_messages` run14/seq3 含 thinking + 「当前应用统计如下：笔记数 3 篇…」text 块）。但详情消息只在 `if (expandedTaskId_)` 时加载与渲染（:685-711）——不展开任务行就永远看不到；且 1.7.7 把消息改成「点消息才展开全文」，展开后默认仍是折叠 | UI 缺陷（数据完整，呈现路径埋了） |
| 3 | 任务列表全少滚动条 | tasklist 用普通 `ui.column` + 预算高 `listH`（:911-916）裁剪，无 scrollView；任务一多即溢出或被裁 | UI 缺陷 |
| 4 | 查看详情改成弹窗展示 | 现状内联展开区 | 需求 |
| 5 | 智能体 UI/UX 很差，几乎不可用 | 上述 1-4 的合成症状：配置无处可设、结果看不见、列表不能滚、详情挤在列表下面 | 总症状 |
| 6 | profile 列表 item 圆圈内文本没垂直居中 | `claude_ui_list.cpp:234-243` `.avatar.text` 只设 `.lineHeight(avatarSize)` + `horizontalAlign(Center)`，**缺 `.verticalAlign(Center)`**。EUI-NEO runtime 只在 `verticalAlign=Center` 时把 ink 中心对齐到 frame 中心，否则 ink 从行框顶沿排——同一文件 :265 注释里已写明该原理（2026-09-29 修 ACTIVE 徽标时发现），头像这次漏了 | 用法 bug（复发） |
| 7 | md 编辑器主页 2 个标签区域的标签内文本没垂直居中 | `markdown_module.cpp:1136-1175`（标签筛选 chip）与 `:1324+`（编辑器顶栏 chip）文本均为 `.size(chipW,h).lineHeight(h)`，无 `verticalAlign` | 同一根因 |

**#6 与 #7 是同一个 EUI-NEO 用法 bug 的多处复发**，因此不做点修，做全仓根治（§2）。

## 1. 批次 A：智能体交互骨架

### 1.1 页面（`agent_module.cpp` compose）

页面只保留三块，全部弹窗化后：

```
智能体                                    [+ 新建任务]
可用工具（11）                          展开 ▼ / 收起 ▲
▼ 任务列表（scrollView，页内可滚）
┌──────────────────────────────────────────────┐
│ 统计笔记数量   手动 · default · 成功            │
│ 2 分钟前 · 2 轮 · 14 tok         启动 详情 编辑 禁用 │
├──────────────────────────────────────────────┤
│ ...（scrollView 内滚）                        │
└──────────────────────────────────────────────┘
```

- 移除页内新建表单（:936-968）与页内编辑表单（:1109-1280）
- 移除内联详情区（展开任务行显示 run 列表 + 消息，含 :1456-1531 区）
- 任务列表包 `components::scrollView`（沿用 :1017 / :1512 现有用法），行高固定、viewport 高 = 页面剩余高

### 1.2 弹窗 1：任务编辑（新建 / 编辑共用）

- 外框：`ui_kit::DialogFrame`（`src/ui_kit/dialog_form.h`，13 处站点共用），id `agent.dialog.task`
- 打开时机：点「+ 新建任务」→ `editingTaskId_` 为空、字段取默认值；点行内「编辑」→ `editingTaskId_` = 该任务、字段预填
- 字段（与现有编辑表单同源同 id 前缀，改为 `agent.dialog.task.*`）：
  1. 任务目标（多行输入，沿用 1.7.7 多行框 + placeholder）
  2. 触发方式（手动 / 定时 / 常驻）
  3. cron 表达式（仅触发方式 == 定时时渲染）
  4. 模型槽位（下拉，键集来自 `src/model_slots.h` 真源）
  5. 迭代上限（数字输入）
  6. 破坏性操作开关
- 保存：编辑态走 `updateTask`，新建态走 `createTask`（参数序以 `agent_store.h:42-59` 现签名为准）；cron 合法性沿用现有保存期校验，非法 → 弹窗内错误文案、**不落库**、弹窗不关
- 取消 / 关闭（✕ / 点遮罩 / Esc）→ `editingTaskId_.reset()` + 弹窗关闭，不落库

### 1.3 弹窗 2：任务详情

- 外框：`ui_kit::DialogFrame`，id `agent.dialog.detail`，内容分三段：
  1. 任务配置摘要（目标 / 触发 / cron 原文 / 槽位 / 迭代上限 / 破坏性标记）——与编辑表单同源读 `getTask`
  2. run 列表（时间 / 状态 / 轮数 / token，倒序，最近一次高亮）
  3. 消息流：`components::scrollView` 内滚，**最终 assistant text 默认展开**；thinking / tool_use / tool_result 折叠（点开）
- 点行内「启动」→ 启动 run 后**立即打开本弹窗**并随页面既有刷新周期实时更新（run 进行中显示状态，完成即出最终回复）
- 关弹窗不回写任务列表；重开回到最近一次 run

### 1.4 状态与生命周期

- `editingTaskId_` 语义不变（空 = 新建），新增 `detailTaskId_` + `detailRunId_` 取代 `expandedTaskId_`
- 保留 1.7.7 已有能力：消息块级渲染、`measureTextSize` 实测展开高度、tool_use / tool_result 展示、可用工具清单
- `msgScrollVersion_` / `expandedMsgSeqs_` 等既有状态继续服务弹窗内消息区

## 2. 批次 B1：垂直居中根治

**规则**：`ui.text` 落在「固定高度的小方块」（chip / badge / pill / avatar / 圆圈 / 计数泡）内且意图居中时，必须显式 `.verticalAlign(eui::VerticalAlign::Center)`。仅设 `.lineHeight(h)` 不构成垂直居中——runtime 仍从框顶排 ink。

**执行**：
1. 全仓扫（`src/module_*/**.cpp`）文本节点与其容器尺寸关系，找「文本盒高 == 容器高」且无 `verticalAlign` 的站点
2. 命中即补 + 就近加一行注释指向本规则（防止第三次复发）
3. **排除 `claude_ui_logs.cpp`**（由 B2 并行改动，避免同文件双写）；若扫到该文件的候选，写进报告交控制器后补
4. 用例：每个改动点至少一条几何/参数断言（中心偏移 ≈ 0），禁止只改不测

**已知必改**：`claude_ui_list.cpp:234`（头像首字）、`markdown_module.cpp:1160` 附近（筛选 chip）、`markdown_module.cpp:1324` 附近（顶栏 chip）。

## 3. 批次 B1 的测试要求

- 断言打在**渲染参数**上（`verticalAlign == Center` 的节点存在且数量符合预期），或几何断言（文本盒中心与容器中心差 ≤ 1px）
- 回归等价物：回退一处改动 → 该断言变红

## 4. 批次 B2：调用记录弹窗加载慢

**现状（已读码确认）**：`claude_ui_logs.cpp` 已是异步查询 + 在途守卫（`refreshCallsCache`：在途 future 存在则不重派），store 侧有 `LIMIT/OFFSET` 分页（`call_log_store.cpp:139-150`）。**无派发风暴**，故慢因在别处。

**两个待实测候选**：
- H1 **按需重绘**：后台结果就绪后无人请求重绘 → 观感上「查完了但不显示」，直到用户动鼠标
- H2 **单 sqlite 连接 mutex 争用**：全进程唯一连接 + recursive_mutex（2026-10-02 刚修过并发纪律），UI 线程其它 DB 操作与后台查询互斥

**执行**：先加计时（打开 → 首帧 → 查询派发 → 结果上屏各段耗时，无头用例/探针），实测出哪段耗时后再对症修；**不猜着改**。若两候选都被证伪，回到报告继续定位，不得交回「已优化」的空结论。

## 5. 验证与交付（六阶段纪律）

- 每批：worktree 隔离实现 → 定向用例（`--filter agent` / `claude` / `markdown`）→ 涉及可见 UI 的改动必须有 probe 或无头几何断言 → 独立 opus review（实现者 ≠ 审计者）→ 按 findings 修 → 静态审计六维度
- 合并：**串行**合入 dev；合并后主树全量构建 + 安装到 `/Applications/DevDesk.app`（用户口径「编译」= 构建 + 安装）
- 交付后由用户真机点验

## 6. 不做（YAGNI）

- 不做智能体与 Claude 页的跨模块联动重构
- 不做调用记录的分页 UI 重设计（只修速度）
- 不做居中问题的 EUI-NEO 上游改造（Text 组件默认居中会破坏全局排版，属另一个议题；本批只在 DevDesk 侧按规则补齐）
