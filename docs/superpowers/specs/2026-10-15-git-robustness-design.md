# Git 健壮性批设计 spec

> **日期**: 2026-10-15
> **分支**: `dev`
> **前置**: GitHub Sync 基础层（commit `8e82e0d`，handoff 见 `2026-10-15-github-sync-handoff.md`）
> **对应**: handoff §五 P1-4（git 未安装检测 / 网络重试 / Token 过期检测）+ P2-7（连接测试按钮）的提级落地

## 一、背景与目标

GitHub Sync 基础层今日交付：`core/git_sync_service`（popen 调系统 git）、`module_settings/github_sync_{config,card}`（配置模型 + Keychain + 设置卡）、`settings_module` 集成。当前 push/pull 回调为占位（显示「开发中」），无 git 检测、无连接测试、无重试、无失败可观测性。

本批补健壮性，**四项目标**：

1. **git 安装检测**：卡片呈现 git 状态，操作前 fail-fast。
2. **测试连接按钮**：一键验证「可访问 / 能 clone / 能 push」，测试分支用完即删、不留痕迹。
3. **git 网络操作重试机制**：瞬态失败自动重试，分类不瞎重试。
4. **失败确认弹窗 + 详情**：重试耗尽弹窗让用户定夺；详情按钮展示每次 git 命令与返回值（token 打码）。

## 二、非目标（明示不做）

- **gh 内置**：用户已否决（保持 Windows 单 exe / 零外部二进制发布形态）。
- **GitHub REST API 兜底**：被墙场景的原生 libcurl 同步兜底，留作后续项。
- **P0 运行时接线 + markdown/todo commit hook**：push/pull 真正接入业务写操作、自动同步调度——下一批。本批的重试框架届时直接复用。
- **命令注入加固**：popen 命令串对 repoUrl / branch 含双引号的逃逸风险已知，记录不修（除非审计定级要求）。
- **自动同步调度 UI**：本批只实现用户触发的测试 / 同步；未来后台自动同步的 escalation 策略通过注入预留。

## 三、架构与组件

```
设置卡「同步」tab (github_sync_card)
  ├─ git 状态行（版本 / 未安装）
  ├─ [立即同步] [拉取] [测试连接]   ← 新增第三个按钮
  └─ 状态行（逐步 ✓/✗ / 错误摘要 / 详情入口）
        │ SettingsCardActions.githubSyncTestConnection
        ▼
github_sync_test_controller（新增，module_settings 层）
  ├─ 后台线程编排测试四步
  ├─ TestState 快照 + 原子版本号（UI 轮询）
  └─ escalation：阻塞等 UI 答复（promise/future）
        │ 调用
        ▼
core/git_retry（新增，core 层纯逻辑、零 UI 依赖）
  ├─ RetryPolicy（次数 / 退避 / 可注入时钟）
  ├─ classifyGitFailure → Transient / Auth / Conflict / Fatal
  ├─ runWithRetry(op, policy, escalation)
  └─ OpAttemptLog（命令打码 / 退出码 / 输出 / 耗时 / 序号）
        │ 包装
        ▼
core/git_sync_service（既有，扩展）
  ├─ gitVersion()（新增）
  ├─ push() / pull() 命令构造修正（本批前提）
  └─ runGit / execCommand（popen，既有）
```

**层次纪律**：`core/git_retry` 是纯逻辑、零 UI 依赖（不 include EUI-NEO / 不调弹窗）；弹窗与测试编排放 `module_settings`。core 通过 `std::function<bool()>` escalation 回调把「要不要再来一轮」的决定权交回模块层，core 不反向依赖 UI。

**异步模型**：沿用 QuotaRefresher 模式——后台线程执行 popen（阻塞），写 `TestState` 快照 + 原子版本号；设置卡轮询版本号变化时刷新快照渲染。UI 线程永不阻塞、永不 join 操作线程。

## 四、组件设计

### 4.1 git 安装检测

- 服务层新增 `gitVersion()`：执行 `git --version`，从输出解析 `git version X.Y.Z` → 返回版本串；失败返回空。`isGitAvailable()`（既有，`git_sync_service.cpp:103`）保留作布尔快捷。
- 卡片新增 git 状态行：打开卡片 / 启用同步时查询，显示「git 2.43.0」或「未检测到 git，请安装后重试」。
- 所有同步 / 测试操作入口先检测：未安装 → 直接报错、不往下走（fail-fast）。

### 4.2 测试连接按钮

卡片第三个按钮（与「立即同步 / 拉取」同行或紧邻行，布局细节 writing-plans 阶段定；测试中禁用）。点击后台线程跑四步，状态行逐步显示 ✓/✗：

| 步 | 操作 | 命令形态 | 通过判据 |
|----|------|----------|----------|
| ① 访问验证 | ls-remote | `git ls-remote <authUrl>` | exit 0（同时验 token + 网络；401 → 认证错；无输出 = 仓库为空） |
| ② clone | 浅克隆到临时目录 | `git clone --depth 1 <authUrl> <temp>` | 临时目录出现 .git（仓库为空时跳过，标注「仓库为空」） |
| ③ push | 测试分支空提交推送 | 临时仓库内 `git commit --allow-empty` + `git push <authUrl> HEAD:devdesk-conn-test` | 远端出现 devdesk-conn-test |
| ④ 收尾 | 删测试分支 + 删临时目录 | `git push <authUrl> --delete devdesk-conn-test` + rm temp | 远端无残留、临时目录清除 |

**关键约束**：

- ② clone 到系统临时目录（`std::filesystem::temp_directory_path() / "devdesk-git-test-*"`），**不碰同步仓库本体 `~/DevDesk/`**。
- 测试分支**固定名** `devdesk-conn-test`（不用时间戳）：开测前先清残留（上次崩溃留下的远端分支 / 本地临时目录），末尾必删。固定名便于发现和清理孤儿。
- **仓库为空**（步① ls-remote 成功但无 refs 输出）：② clone 跳过并标注「仓库为空（首次同步将自动初始化）」；③ push 改用临时目录 `git init` + `git commit --allow-empty`（孤儿分支）+ push 测试分支，照样覆盖 push 权限。
- URL / Token 未填全：按钮禁用，悬停提示「请先填写仓库 URL 与 Token」。
- 收尾用 RAII 守卫：任何步骤失败 / 取消 / 异常都执行删远端分支 + 删临时目录。

### 4.3 重试机制（方案 A：core 纯逻辑执行器）

新增 `src/core/git_retry.{h,cpp}`：

```cpp
enum class GitFailKind { Transient, Auth, Conflict, Fatal };
struct RetryPolicy { int maxAttempts = 3; int backoffMs[3] = {2000, 4000, 8000}; };
struct OpAttemptLog {
    std::string displayCommand;  // token 打码后的展示串
    int exitCode = 0;
    std::string output;          // token 子串已打码
    int durationMs = 0;
    int attemptNo = 0;
};

GitFailKind classifyGitFailure(int exitCode, const std::string& output);
// Transient（重试）：Could not resolve host / Connection timed out / Failed to connect /
//                   SSL ... error / RPC failed / HTTP 5xx / connection reset
// Auth（立即停、提示换 token）：Authentication failed / 403 / could not read Username /
//                              Permission denied / 401
// Conflict（不重试、走冲突流程）：non-fast-forward / rejected / CONFLICT / merge conflict
// Fatal（不重试直报）：其余（列不全的模式保守归 Fatal）

// 仅网络类操作（ls-remote/clone/push/pull/fetch）走 runWithRetry；本地操作（add/commit/status）不走。
// escalation：3 次耗尽后调，返回 true=再来一轮 3 次，false=取消。
// onAttempt：每次尝试结束回调（写 OpAttemptLog）。
// 时间源可注入（单测不真睡）；退避 sleep 与 escalation 等待带 abort 标志、可中断。
Result<GitResult> runWithRetry(std::function<Result<GitResult>()> op,
                               const RetryPolicy&,
                               std::function<bool()> escalation,
                               std::function<void(const OpAttemptLog&)> onAttempt);
```

- **分类是启发式**：模式表未覆盖的保守归 Fatal（不重试、直报），避免对未知失败瞎重试。
- **退避 sleep 与 escalation 等待必须可中断**：带 abort 标志 + 可注入时钟；app 退出时唤醒，防挂死。

### 4.4 失败确认弹窗

- 触发：某次操作 3 次重试耗尽且 escalation 被调。
- 内容：标题「Git 操作失败」+ 一句人话错误摘要（按分类：Auth →「Token 无效或过期，请更新」；Transient →「网络不可达，已重试 3 次仍失败」；Conflict →「远端有更新 / 冲突，请先拉取」）+【重试】【取消】【详情】。
- 重试 = 重启一轮 3 次；取消 = 中止，状态行显示「已取消」。
- **跨线程**：后台线程在 escalation 回调内阻塞于 `std::promise/future` 等 UI 答复；UI 线程轮询发现 `TestState.awaitingConfirm=true` 后弹窗；用户点击 → 设置 future 值 → 后台线程续跑 / 退出。UI 线程不 join 操作线程，无死锁。
- **未来后台自动同步**：注入不同 escalation 策略（通知 + 状态、不弹窗）。本批只实现「弹窗」策略，接口预留。

### 4.5 详情查看

- `OpAttemptLog`（放 core，§4.3 已列）记录本次操作**每一次尝试**：展示命令（token 打码成 `***`）、退出码、全部输出（token 子串存储前打码）、耗时、尝试序号。
- **打码时机**：命令生成那一刻**同时**产出「真实命令」与「展示命令」两条——真实命令只用于实际执行，永不进 UI / 日志；展示命令用于详情视图。`buildAuthUrl` 产出的 `https://<token>@...` 在生成展示串时 token → `***`；输出中含 token 子串的也替换。
- 现状保持：`runGit` 里 `LOG_INFO("git_sync: 执行命令")` 不落命令原文（既有，`git_sync_service.cpp:117`），保持。
- **入口**：弹窗内【详情】按钮打开可滚动详情视图；测试连接结束后，卡片状态区也提供【详情】入口查看本次测试全部尝试。同一数据结构、同一视图组件。
- **保留**：OpAttemptLog 列表保留最近一次操作（内存、不持久化，app 重启清空）。

### 4.6 顺带修正：push / pull 命令构造（本批交付物前提）

**Bug**：`git_sync_service.cpp` 的 `push()` / `pull()` 经 `gitAuthCmd` 把 `authUrl` 拼成命令**最后一个位置参数**：

```
git -c http.sslBackend=openssl -c credential.helper= -C "<repo>" push origin branch "<authUrl>"
```

`git push` 的位置参数语义是 `[<repository> [<refspec>...]]`——`origin` 是仓库、`branch` 是 refspec、`<authUrl>` 被当**第二个 refspec**，git 会尝试解析它为 refspec 而失败。命令疑似从未可用（UI 占位所以没暴露）。

**修正方向**（锁定，以独立审计结论为准）：`authUrl` 作 **repository 位置参数**，对齐 handoff §六 已人工验证过的命令形态：

- push：`git push <authUrl> +<branch>:<branch>`（handoff 验证命令：`git push https://... +HEAD:dev`）
- pull：`git pull <authUrl> <branch>`（或 `git fetch <authUrl> <branch>` + merge）

修正属本批交付物前提——测试连接的 ③ push 步直接调用 `push()`，不修则测试过不了。修复须「先红后绿」证据（见 §七）。

## 五、数据流与异步线程模型

```
用户点 [测试连接]
  → settings_module 调 test_controller.start(config)
  → 后台线程：
      gitVersion? ─ no → 写 TestState{failed:"未安装 git"} ─ 退出
                  └ yes → 步① ls-remote (runWithRetry)
                           ├─ 瞬态失败 → 退避重试 (≤3)
                           ├─ 3 次耗尽 → escalation → 阻塞 future
                           │              UI 轮询见 awaitingConfirm → 弹窗
                           │              用户【重试】→ future=true → 再来一轮
                           │              用户【取消】→ future=false → 中止
                           └─ 通过 → 步② clone ... 步③ push ... 步④ cleanup
      每步写 TestState{step, results[], awaitingConfirm?} + version++
  → UI 轮询 version 变 → 渲染状态行 / 弹窗 / 详情
  → 完成：TestState{done, summary, attempts[]} + version++
```

- 后台线程持有 abort 标志；app 退出 / 用户取消时置位，唤醒 sleep / escalation 等待。
- `TestState` 用 mutex 或 atomic 字段保护；版本号 atomic。
- UI 线程只读快照，绝不阻塞等操作线程。

## 六、错误处理与重试（分类行为表）

| 分类 | 触发模式（启发式） | 行为 |
|------|--------------------|------|
| Transient | resolve host / timed out / connect 失败 / SSL / RPC / 5xx / reset | 退避重试 ≤3 次；耗尽弹窗 |
| Auth | Authentication failed / 401 / 403 / could not read Username / Permission denied | 立即停、不重试；弹窗提示换 token |
| Conflict | non-fast-forward / rejected / CONFLICT | 不重试；走冲突流程（handoff P0-3，下一批） |
| Fatal | 其余 | 不重试；弹窗直报 |

## 七、验证策略

**单测**（`core/git_retry` 纯逻辑，可注入时间源、不真睡）：

- `classifyGitFailure`：每类至少 3 个模式用例 + 未覆盖模式归 Fatal。
- token 打码：命令串 + 输出含 token → 展示串 / 存储串均无明文 token。
- `RetryPolicy` / `runWithRetry`：注入 fake 时钟验退避序列、重试次数、escalation true/false 分支、abort 中断。
- `OpAttemptLog`：字段完整、序号递增。

**单测**（`git_sync_service` 修正）：

- push / pull 命令构造：修前断言命令形态错误（红）→ 修后断言 authUrl 在 repository 位置（绿）。

**probe**（`tests/probes/`，`--screenshot`）：

- 测试连接按钮状态流转：禁用 → 测试中 → ✓/✗ → 完成。
- 失败弹窗几何 +【详情】视图可滚动。
- token 打码：详情视图截屏无明文 token。

**真机验收**：连真实 GitHub 私有仓库跑一次完整测试连接（含 push + 删分支），确认远端无残留、临时目录清除。

## 八、文件变更清单

| 文件 | 动作 | 说明 / 锚点 |
|------|------|-------------|
| `src/core/git_retry.h` | 新增 | RetryPolicy / classifyGitFailure / runWithRetry / OpAttemptLog 声明 |
| `src/core/git_retry.cpp` | 新增 | 上述实现，纯逻辑零 UI 依赖 |
| `src/core/git_sync_service.h` | 修改 | 新增 `gitVersion()` 声明；push / pull 签名不变 |
| `src/core/git_sync_service.cpp` | 修改 | push / pull 命令构造修正（§4.6）；`gitVersion()` 实现；命令生成处产出打码展示串 |
| `src/core/CMakeLists.txt` | 修改 | +git_retry.cpp |
| `src/module_settings/github_sync_test_controller.h` | 新增 | 后台线程编排四步 + TestState + escalation |
| `src/module_settings/github_sync_test_controller.cpp` | 新增 | 上述实现 |
| `src/module_settings/github_sync_card.cpp` | 修改 | +测试连接按钮 + git 状态行 + 测试结果状态行 + 详情入口 |
| `src/module_settings/github_sync_card.h` | 修改 | CardDrawArgs / 回调新增 testConnection |
| `src/module_settings/settings_ui_cards.h` | 修改 | SettingsCardsIn：+githubSyncGitVersion / +githubSyncTestState；SettingsCardActions：+githubSyncTestConnection |
| `src/module_settings/settings_module.cpp` | 修改 | 装配 test_controller + 回调 + 快照字段刷新 |
| `src/module_settings/settings_ui_logic.h` | 修改 | kGithubSyncCardHeight 调整（新增行） |
| `src/module_settings/CMakeLists.txt` | 修改 | +github_sync_test_controller.cpp |
| `tests/` | 新增 | git_retry 单测 + git_sync_service push/pull 构造单测 |
| `tests/probes/` | 新增 / 扩展 | 测试连接 + 弹窗 probe |

## 九、决策表

| 决策 | 选择 | 理由 |
|------|------|------|
| 重试机制放哪层 | core 纯逻辑执行器（方案 A） | 复用面广（本批测试 + 下批 P0 运行时 + md/todo hook）；core 不反向依赖 UI；与 QuotaRefresher 异步模式一致 |
| 是否内置 gh | 否 | 保持单 exe 零外部二进制形态；REST 兜底用既有 libcurl 原生做（后续）；gh 版本漂移不背 |
| 测试分支命名 | 固定 `devdesk-conn-test` | 便于发现 / 清理崩溃残留；末尾必删 |
| 空仓库 push 测试 | 临时目录 orphan 提交 | 不依赖远端有 refs，照样覆盖 push 权限 |
| 弹窗跨线程同步 | promise/future + UI 轮询 | UI 线程不阻塞、不 join 操作线程；复用版本号轮询模式 |
| token 打码时机 | 命令生成那一刻 | 真实命令永不进 UI / 日志；从源头杜绝泄漏 |
| push/pull 修正纳入本批 | 是 | 测试连接 ③ push 直接调用；不修则交付物不可达——属本批交付物前提 |
| 顺带修不扩到命令注入加固 | 不扩 | 范围纪律；记录已知风险，除非审计定级要求 |

## 十、待确认（writing-plans 阶段锁定，不影响设计）

- 设置卡现有刷新机制的具体接入点（`settings_module.cpp` 快照刷新路径 / 有无 onTimer）——writing-plans 读源码锁。
- app 现有 modal 弹窗锚点（今天交付的备份还原对话框）的具体 API——writing-plans 读源码锁。
- probe 几何断言数值（按钮位置 / 弹窗尺寸）——实施时按实际布局定。
- 测试连接按钮在卡片中的具体行布局（同行 / 紧邻新行）——实施时定。
