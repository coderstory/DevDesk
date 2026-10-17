# 01 · `todo.create` 属性面过窄（建任务要二跳）

Status: `needs-triage`
实现: 已完成 b398e3c + 0280bde（2026-10-17）
建议优先级: 中
来源: handoff §3.C「A1」+ 2026-10-17 静态复核（**已纠正原路径**：原记 `src/module_agent/todo_tools.cpp:177`，实为 `src/module_todo/todo_tools.cpp:177`）

## 现象

智能体建待办时只能一次给 `title` + `columnId`；`description` / `priority` / `dueAt` 等属性必须**再发一次 `todo.update`**（二跳）。二跳失败就留下半成品任务（有标题、无属性），且用户看到的是「任务已创建」的成功语义。

## 证据

- `src/module_todo/todo_tools.cpp:177` — `todo.create` 的 input_schema 只有两个属性：
  `"properties":{"title":{...},"columnId":{...}},"required":["title"]`
- `src/module_todo/todo_service.h:69` — 服务层签名即 `createTask(int64_t columnId, const std::string& title)`，**根本没有**其余属性入参
- `src/module_todo/todo_tools.cpp:228` — `todo.update` 明确支持 `title` / `description` / `priority` / `dueAt`（且 `updateTask` 是**整字段 UPDATE**，未提供字段会被清空 → 工具须先取旧值叠加再回写，见同文件 `:3-4` 的注释）
- `src/module_todo/todo_tools.cpp:208` — create 实际调用 `svc_.createTask(columnId, title)`

## 影响（用户视角）

「让智能体记一条带截止时间和优先级的待办」需要两次工具调用：多一次往返 = 多一次失败面、多一次 LLM 决策，且两次之间若中断，库里留下无属性的任务而 UI 不会提示。

## 修复方向（三选一，待裁）

1. **扩 `createTask` 入参**：加可选 `description/priority/dueAt`，`todo.create` schema 同步放开 —— 最直接，但要动服务层签名与既有调用点。
2. **工具层内联二跳**：`todo.create` 收到可选属性时，内部先 create 再 update —— 不动服务层，但把两次写合进一个工具语义（失败要能回滚或明确报错）。
3. **只改文档**：在 schema description 里写明「要设属性请接一次 todo.update」—— 零代码，但把负担留给模型与用户。

> 注意：`updateTask` 的「未提供字段即清空」语义是选项 2 的主要风险点（见 `todo_tools.cpp:3-4`）。

## 验收判据（待定稿）

一条 `todo.create` 调用即可产出带 `priority` 与 `dueAt` 的任务；二跳路径不再需要；回归覆盖「属性全给 / 只给部分 / 都不给」三态。

## Comments

### 实现（b398e3c 实现 + 0280bde 测试补齐，2026-10-17）

选**修复方向 1（扩服务层 + 单条 INSERT）**，未选方向 2（工具层内联二跳）：

- 二跳自带失败面——第二跳失败要么吞掉（= 票面要消灭的半成品）要么补偿删除（先建后删，而删除本身也会失败）。单条 INSERT 让「属性与标题同生共死」成为**结构性不变量**，不靠失败处理兜底。
- 改动面：新增 `NewTaskAttributes`（`src/module_todo/todo_service.h:45` 附近）+ 带属性重载（同文件 `:82-85` 附近）；**旧两参入口签名与语义零改动**，仅委托到新重载（`src/module_todo/todo_service.cpp` 约 `:420`）。`updateTask` 一行未动。
- 未破坏「整字段 UPDATE」语义：create 路径根本不经过 `updateTask`；`todo.update` 的「先取旧值再叠加」仍在原位（`src/module_todo/todo_tools.cpp:274/293` 附近）。属性解析刻意**不与 update 共用实现**——update 要区分「未提供 = 保持」与「显式 null = 清空」，create 没有旧值可清，两者语义不同，硬共用会把 create 的 null 变成"清空"。
- 覆盖面（票面判据三态 + 负路径）：全给（`test_todo_tools.cpp:179`）、都不给（同用例对照组）、只给部分 / 显式 null（`0280bde`）、非法属性与幽灵列 → 报错且**一条不落库**（`test_todo_tools.cpp:233`）、服务层落库断言（`test_todo_service.cpp` 新增 1 条）。

### 未做 / 未覆盖（如实）

- **未覆盖**：非法 `dueAt` 之外的类型畸形（如 `priority` 传字符串）依赖既有 JSON 解析路径，未单独构造用例；「第二跳失败」这一类路径在本方案下**结构上不存在**（无二跳），故无需覆盖。
- **未做**：`tags` 入参（票面未要求）；`todo.update` 的任何重构（有意不做）。
- **待确认**：全量门禁由主会话在本批收尾时统一跑（本单实现者被打断，未自跑全量）。

