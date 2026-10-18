# DevDesk 智能体能力建设 — 进度落盘（2026-10-18 20:30）

> 这份文件是**给人看的进度快照**，不是 gsd 交接文件（gsd 已卸载）。
> 目的：新会话接手时，5 分钟内知道「做到哪、剩什么、有什么坑」。
>
> 生成时刻状态：`DevDesk` 分支 `dev`，HEAD `033d7db`，领先 `origin/dev` 294 提交，**未 push**。
> EUI-NEO 分支 `main`，HEAD `93eb7b9`，工作树干净，**未 push**。

---

## 一、一句话现状

5 条需求的**底层能力已全部落地并提交**（14 个 commit），但**没有任何一条跑通过端到端**，因为新版 app 还没安装、token 刚写入未验证。另外有一个用户中途新增的「AI 自然语言生成任务」功能做了一半，卡在磁盘上。

---

## 二、已完成并提交（14 个 commit）

### 需求 1 · 逾期 todo 通知
| commit | 内容 |
|---|---|
| `48f2fc1` | `todo.list` 加 `overdue` 参数，直接复用 `TodoService::listOverdue()`（与 Todo 页逾期弹窗同口径）；`completed=true` 与之互斥返回工具错误 |

### 需求 2 · 每日流水 md
**零代码**。`md.create` 已会自动 `create_directories`，路径 `流水/YYYY-MM-DD.md` 即可。不需要写代码。

### 需求 3 · 今日热榜 top10
| commit | 内容 |
|---|---|
| `4d64ff0` | 微博热搜解析（过滤广告、按 `realpos` 重排、`#`/中文 percent-encode） |
| `e1b86ca` | 百度 PC / 今日头条 / 微博 `hot_band` 解析 + `HotSource` 分发 |
| `033d7db` | `hot.list` 工具本体（四源串行降级、单源 8s / 总 20s、缓存 300s、限频 60s、stale 1800s） |

### 需求 4 · 定时闹钟
| commit | 内容 |
|---|---|
| `e6f4c5f` | `trigger_config` 支持 `once_at`（1..4102444800，与 cron/events 互斥） |
| `4be2ee5` | Scheduler `once` 分支（60s 窗口、响过即停用、错过不补、`agent_runs` 防重启重响） |
| `144eeb0` | 触发方式加「闹钟」+ 表单（严格 `YYYY-MM-DD HH:MM` 解析、拒绝过去时间/夏令时不存在时刻、编辑回填） |

### 需求 5 · 激活 profile 连通检测
**零代码**。`claude.test_connection` 不传 id 就是当前激活 profile；无密钥时不发请求、不回显密钥、15s 超时。

### 通知基础设施（需求 1/4 依赖）
| commit | 内容 |
|---|---|
| `2fe9c03` | `platform::notify` 抽象 + macOS `UNUserNotificationCenter`（裸二进制 bundle guard、前台 banner） |
| `93eb7b9`（EUI-NEO） | Windows 托盘通知桥（SRWLOCK 队列 + `PostMessageW` 封送 UI 线程、UTF-16 安全截断） |
| `3150c67` | DevDesk Windows 通知后端接 EUI-NEO API |
| `0979707` | `scripts/sign-macos-app.sh` + 三条打包路径接线（修掉 `.app` 签名无效导致通知拿不到 bundle 身份） |
| `a30fb0c` | `app.notify` 工具 + AgentModule 注册 + 启动期请求授权 + 线程安全降级为应用内 toast |
| `8271d2a` | docs 收口：两份 spec 入库 + backlog 边界回写 + 修正 `MACOS-APP-BUNDLE.md` 的 `--deep` 旧口径 |

### 基础能力
| commit | 内容 |
|---|---|
| `225b43c` | 「每天」触发类型 + system prompt 注入当前本地时间（模型没时钟，日期由宿主给） |
| `cc87904` | （WIP）交接文件提交，可忽略 |

---

## 三、未提交的工作树（**重要：随时可能丢**）

```
 M src/module_agent/CMakeLists.txt      +task_draft.cpp
 M src/module_agent/agent_module.cpp    -123 行（时间辅助函数搬出匿名命名空间）
 M tests/CMakeLists.txt                 +test_agent_draft.cpp
?? src/module_agent/task_draft_parse.h   154 行
?? src/module_agent/task_draft.h          80 行
?? src/module_agent/task_draft.cpp       453 行
?? tests/test_modules/test_agent_draft.cpp  634 行
```

共 **1321 行未提交**，属「T7a：AI 自然语言生成任务草稿 — 纯逻辑层」。

**状态：完全没构建过、没跑过测试。** 写它的子代理在网络故障前被 `git stash` 收走后直接死掉，我弹回 stash 救回来的。

⚠️ **风险**：未提交 + 未验证。建议立刻先 `git add -A && git commit` 存一档（哪怕是 wip），再谈构建修复。

---

## 四、未处理的问题

### P0 · 阻塞主线
1. **T7a 半成品没验证** —— 1321 行代码从未编译过。可能编译不过、可能逻辑跑偏，必须先构建。
2. **网关限流** —— 连续 3 个子代理被掐死（响应头超时 → DNS 解析失败 → `Forbidden: Detect Abuse Traffic`）。派子代理前要确认恢复了；**并且必须禁止子代理 `git stash`/`checkout`/`reset` 工作树**（第 3 个就是这么把代码弄丢的）。
3. **新版 app 没安装** —— 运行中的还是旧版本，所有新能力都没被用户实际用过。

### P1 · 需要你参与
4. **token 未验证** —— 你说已经写进 profile，但没验证过。端到端必须先跑一次 `claude.test_connection`。
5. **Windows 11 真机 UAT 从零开始** —— 只有 MinGW 交叉编译通过，真实横幅、中文/emoji 边界、Focus Assist 抑制、托盘未初始化降级全都没验过。双平台是硬约束。
6. **macOS 通知真机 UAT** —— 授权弹窗、点击激活、ad-hoc 重装后权限持久性都没验。

### P2 · 设计债
7. **通知 spec 与代码已脱节** —— spec 里写「未提交」的地方现在都提交了；阶段编号 T1a–T1f 与实际编号 T1c2/T1d/T1e2 对不上；签名示例比实际脚本多一个 identifier 断言。
8. **`listOverdue` 不排除归档任务** —— 「已归档 + 已逾期 + 未完成」仍算逾期。这是既有语义，本次严格沿用没改。
9. **daily 跨午夜边界** —— 昨天 23:59 起的 run 跨过午夜，今天会尝试派发一次被单闸拒绝，每分钟刷一条 WARN 直到旧 run 结束。低概率但会刷日志。

---

## 五、未开发的内容

### 队列（7 项，按依赖排序）

| # | 任务 | 规模 | 依赖 |
|---|---|---|---|
| 1 | **T7a 收尾**：构建 + 修正 + 补 CHANGELOG + 提交 | 中 | 无（代码已在树里） |
| 2 | **T3c2**：`hot.list` 注册进 AgentModule + 生命周期/卡片测试 | 小 | 无 |
| 3 | **T7b**：弹窗标题行「AI 生成草稿」chip + 子弹窗输入 + 异步生成 + 预填 | 中 | #1 |
| 4 | **UI 重设计 P1**：详情页布局壳 | 中 | #3 |
| 5 | **UI 重设计 P2/P3**：时间线、run 详情、配置折叠 | 大 | #4 |
| 6 | **T6 收口**：装新版 + 5 条任务落库 + 端到端验证 | 需你参与 | 全部 |
| 7 | **Windows 11 真机 UAT** | 需你的 Win 机器 | #6 |

### 需求侧还欠的（都不是代码）

- **5 条任务一条都没建**。用户原文：
  1. 每天 17 点判断逾期 todo，存在则通知
  2. 每天在【流水】新建 `【年-月-日】.md`，内容一行 `# 今日待办`
  3. 手动执行，联网查今日最热话题，输出 top10
  4. 定时闹钟，时间到了发通知
  5. 手动检测激活 profile 连通
- 需要等新版装好后逐条创建 + 真实跑一遍。

### 明确不做（已定案）
- 闹钟**不补跑**（app 没开/睡眠错过就不响，下次打开也不补）。
- 通知**不推 OS 级定时**（不做 launchd / 任务计划程序），进程退出后不保证投递。
- 热榜**不合并多源**（热度量纲不同），整源切换 + `source` 标注。
- 不改 `listOverdue` 的归档语义。

---

## 六、关键设计决策（改代码前必读）

| 决策 | 理由 |
|---|---|
| 日期由宿主注入 system prompt | 模型没时钟，绝不让模型猜绝对时间 |
| 通知分层：显式 `app.notify` 走系统通知，run-end 仍走应用内 toast | 不推翻 M3 裁决，显式 notify 是新触发源 |
| macOS 不加 hardened runtime / `--deep` 签名 | Homebrew OpenSSL 在 bundle 外，会被 library validation 拒绝 |
| 裸二进制必须 bundle guard | 直接碰 `UNUserNotificationCenter` 会抛 `NSInternalInconsistencyException` |
| AI 草稿：模型只做意图理解，宿主做时间/cron/工具名校验 | 确定性判断不交给模型 |
| AI 草稿：单项校验失败只降级 + 警告，绝不整体失败 | 让用户白填一遍更糟 |
| AI 草稿：生成结果永不自动保存 | 防止模型理解偏差直接建出错任务 |
| T7b 用标题行 chip + 子弹窗，不加高弹窗 | 弹窗 500 高有 3 处常量断言 + 2 处 `+224` 断言 |
| T7 不给 `DirectLlmClient` 加 `SendOptions` | 4096 token / 120s 够用，避免动既有断言 |

---

## 七、环境与纪律

**构建**
- 一律 `bash scripts/build-test-run.sh`（在 `DevDesk/` 下），**禁止手敲 cmake**。
- `build/` 独占：**同一时间只允许一个跑构建/测试的子任务**。只读调研可并行。
- 基线：全量 **1523 / 1517 / 0**（6 skipped，平台已知跳过）。
- 已知非回归红（别当 bug 修）：macOS FileWatcher 轮询兜底 ×2、keychain 空账号语义 ×1。

**协作**
- 主会话只做拆单、派单、验收，**不执行构建/测试/审计**。
- 子任务并发上限 **3**。
- 派单要**拆细**，不要一次性塞给一个子代理。
- 子代理**新增硬约束**：禁止 `git stash` / `git checkout` / `git reset` 操作工作树——直接在现有文件上继续做，保证网络挂掉时代码留在磁盘上。

**安全**
- 不 push 到任何远端。
- EUI-NEO 只提交本地，永不碰 `origin`。
- **绝不读取或回显 profile 密钥**，验证连通性只用 `claude.test_connection` 工具。

**代码**
- 业务代码禁止 `#ifdef _WIN32` / `__APPLE__`，平台差异拆文件 + CMake 选源。
- `module_*` 之间不可互相依赖。
- 不抛 C++ 异常，错误走 `Result<T>`。
- 子代理**追加一条**：失败时优先读日志定位，别造根因；先复现再改。

---

## 八、参考文档

| 文件 | 内容 |
|---|---|
| `docs/superpowers/specs/2026-10-18-agent-notification-design.md` | 通知层设计（**注意已与代码脱节，见 P2-7**） |
| `docs/superpowers/specs/2026-10-18-agent-detail-dialog-redesign.md` | 详情页重设计 739 行（UI 阶段必读） |
| `docs/superpowers/plans/2026-09-22-backlog.md` | M3 通知裁决 + 本次边界补充 |
| `docs/MACOS-APP-BUNDLE.md` | macOS 签名口径（已改，别改回 `--deep`） |
| `CHANGELOG.md` 的 `[未发布]` 段 | 已落地能力的准确清单 |
| `.planning/.continue-here.md` | 旧 gsd 交接文件，**已过期**，别照着做 |

---

## 九、建议的下一步

1. **先存档**未提交的 1321 行（`git add -A && git commit -m "wip(T7a): ..."`），止损。
2. **确认网关恢复**后再派 T7a 收尾，拆成两个更小的任务：
   - 7a-1：只做「能编译 + 时间函数搬移等价」（小、快、易验证）
   - 7a-2：补功能 + 补测试（大）
3. T7a 绿了之后，按队列 T3c2 → T7b → UI P1/P2/P3。
4. 全部代码落地后，再走 T6：装新版 → 验 token → 建 5 条任务 → 端到端。
5. Windows 11 真机 UAT 单独排时间。