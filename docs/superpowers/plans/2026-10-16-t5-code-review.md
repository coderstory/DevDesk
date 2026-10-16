# T5 双轴 code-review（Standards + Spec）— 记录与处置

- 日期：2026-10-16 ｜ 对象：dev 上 T1–T4 全批 = `de456cb...HEAD`（72 提交）
- 两轴：
  - **Standards**：`DevDesk/CLAUDE.md`「注释纪律」（硬规则）+ 本仓既有约定（静默失败面 / 纯件契约）
  - **Spec**：`docs/superpowers/specs/2026-10-15-github-sync-api-design.md`（§4 关键接口 / §5 数据流 / §6 错误处理与可见性 + 决策表 20 条）
- 本票：`worktree-agent-t5-cleanup`（基于 dev `2db7e38`），逐项独立 commit；本文件留痕
- 本票写动作：本文件 + 下列修复。**未动 spec / backlog**（retry 口径回写由主会话 `1352875` 完成），未跑 GUI / 真机

## Standards 轴

### S1（硬违规）注释纪律：出处与证据引用 + 演变叙事

- 扫描方式：`git diff de456cb...HEAD -- '*.cpp' '*.h'` 全量（含 `tests/`）逐行判「帮 AI 少犯错 vs 浪费注意力」。
- 命中：约 29 处**出处与证据**（`决策 #N` / `spec F#` / `审计 finding N` / `T3 审计` / `A#-C# 复审` / `（复审裁决）`）+ 约 5 处**演变叙事**（「原先只进日志」「提示行改成」「修前本用例红」「T3 时代的旧记录」「原先只能靠 PUT」）；同类的裸 spec 编号（`F5`–`F9`）、验证体系自解说（`test_sql_dump 的 xx 钉死`）一并计入。
- 处置：**已修** `fa80dfa` —— 103 处替换 / 20 文件。规则：编号与出处删掉、其后的规则/契约内容保留；编号删后句子不成立的改写为直接陈述。**只动注释**（脚本验证：剥掉 `//` 后的代码体与 HEAD 逐文件字节相同）。
- 未动（有意，边界声明）：
  - 测试**名字字符串**里的 `(F5)/(F6)/(F3)/F9`（`test_github_sync_card.cpp` / `test_github_sync_logic.cpp`）——那是代码不是注释，且是 ctest 过滤串的一部分；本票边界为「只动注释」。
  - 领域词（`审计行` / `审计日志` / `无请求可审计`）与反重构警告（「挪回 INSERT 之前建」「勿顺手改回」）——按纪律属必留项。

### S2（硬违规）静默失败：`pruneBackups` 吞 `remove_all` 错误码

- `src/module_settings/github_sync_logic.cpp` 的 `pruneBackups` 抄自 `core::dbmaint::pruneFilesWithPrefix`，但 `remove_all` 的错误码被丢弃：删除失败既不计数也不留痕 → 备份目录会超保留数（各 5 份）无限增长而无人察觉。
- 处置：**已修** `04161d5`。**未委托 core 函数**，因语义不同——core 版用 `fs::remove`（删不了目录、首错即 fail 返回 `Result`），gsync 版的笔记备份是**目录**、契约是「返回删除数」；委托会直接改掉笔记备份的清理语义。改为 ec 检查 + `LOG_WARN` 显形；同函数内的扫描中断（`directory_iterator` 的 ec）同属静默失败面，一并补 WARN 后 break。

### S3（smell，登记不修）

- `putFile` / `putFileOnBranch` 重复体；EUI-NEO `SwitchBuilder` 无 `position` 导致 `ui.stack` 包裹 workaround 双处重复（`github_sync_card.cpp`）。两条已由主会话登记 backlog（后者属 EUI-NEO 上游 PR 线）。

## Spec 轴：3 缺失 / 1 未接线 / 3 对不上

### 缺失（3）

| # | 依据 | 缺口 | 本票处置 |
|---|---|---|---|
| a1 | §5「笔记写入/**删除** 或 待办**增删改** → 置 dirty」+ 决策 #20 | dirty 只订 `md.saved` / `todo.created` / `todo.completed`；笔记删除、待办改标题 / 换列 / 删除 / 取消完成都不置位 → 那类改动永不自动同步 | **已修** `63950b1` |
| a3 | §5「保存配置 → 校验（测试连接三步）→ 首次方向」 | `applyGithubSyncEnabled(true)` 直接跑 FirstSync，跳过校验 | **已修** `8f39a9f` |
| c8 | §6「认证失败（401）→ 弹窗文案指向『请检查/更新 PAT』」 | 文案只有「令牌无效或已过期」，用户不知下一步 | **已修** `38c6d77` |

### 未接线（1）

| # | 依据 | 缺口 | 本票处置 |
|---|---|---|---|
| b4 | §4 / 配置键「`auto_push`：是否自动 push（默认 true）」 | 卡片渲染 + 持久化的 `autoPush` 无人读，`maybeAutoGithubSync` 照推 → 关掉开关后自动推送照发 | **已修** `a651015`（退出补推同属自动路径，一并按同一开关收口） |

### 对不上（3）

| # | 依据 | 偏差 | 本票处置 |
|---|---|---|---|
| c6 | §5「失败：重试 3 次」+ §4 `RetryPolicy` | 默认 `attempts=3` 只发 3 次请求、退避表第 3 档 9s 永不使用；口径应为「首次 + 3 次重试 = 总请求数 4」 | **已修** `c5d41dc`（spec §4 口径由 `1352875` 回写） |
| — | 决策 #15「本地 git 可用时 best-effort 本地版本历史（init + commit）」 | 未实现：现仅探测 + 灰字，git 可用时开关量零效果 | **不在本票**（已登记 backlog，L 级） |
| — | §4 / §10「F7 全树拉取」 | `truncated=true` 硬失败（大仓 >10 万条不可拉） | **不在本票**（已登记 backlog；代码头注已声明取舍） |

## 本票逐项 commit

| 项 | SHA | 一句话 |
|---|---|---|
| S2 pruneBackups 显形 | `04161d5` | remove_all 的 ec 不再吞；扫描中断一并 WARN |
| S1 注释纪律清扫 | `fa80dfa` | 103 处替换 / 20 文件，只动注释 |
| b4 autoPush 接线 | `a651015` | 闸沉到 `gsync::autoPushDue`；退出补推同口径 |
| c6 重试默认值 | `c5d41dc` | attempts 3→4（总请求数口径），退避 1s/3s/9s 全用上 |
| c8 401 文案 | `38c6d77` | 补「请检查/更新 PAT」 |
| a1 dirty 触发源 | `63950b1` | +5 事件常量，笔记删除 + 待办增删改全 emit |
| a3 启用流程校验 | `8f39a9f` | 新增 `SyncAction::ValidateFirstSync`：三步过了才首同步 |

## 复审再修（2 项，2026-10-16 审计复核后）

| 项 | SHA | 问题 | 修法与证据 |
|---|---|---|---|
| 必修 | `2dc3c92` | `pruneBackups` 用 `remove_all(...) > 0` 判成功——失败时返回 `(uintmax_t)-1`（非 0）→ 失败被计成成功、`else if (rmEc)` 成死代码（WARN 永不打印，S2 的 finding 以另一种形态复活） | 先判 `rmEc`（同 `core/db_backup.cpp` 判据）再按返回值计数；`directory_iterator` 构造失败 / `is_directory` 判定出错也不再静默。新用例两档失败（全失败 / 部分失败）+ WARN 逐次计数 + 前提断言（失败返回 -1 且 ec 非空）；**变异验证**：回退判据 → 用例红（`removed != 1`） |
| 建议 | `b103399` | 退出补推按 autoPush 收口后，关开关 + dirty 退出既不推也不落 F5 提示 → 用户不知道改动没上传 | 早退前补 `enabled && !autoPush && dirty → setUnsyncedNotice()`（不推不等待）；新用例经 `runGithubSyncExitPush()` 驱动，两条观察口 + 无改动对照；**变异验证**：回退 → 用例红（`withDirty.noticed` 假） |

转票（本票不动，已由审计方登记）：dirty 触发源残余（笔记重命名/新建、待办列写路径、`deleteColumn` 级联删任务不置 dirty）；首次启用路径②跳过校验。

## 验证

### 复跑（worktree 内）

```
DEVDESK_TEST_TMPDIR=/tmp/devdesk-t5-tmp bash scripts/build-test-run.sh --no-run
```

- 结果：`Total: 1428 / Passed: 1422 / Failed: 0`（`Total − Passed` = 6 条按平台 `DOCTEST_SKIP`，非失败）
- 基线对照：本票开工时同命令 = `1421 / 1415 / 0`；本票新增 7 条用例（`github_sync_auto_push_due` / `github_default_policy_three_retries` / `settings_github_sync_autopush_switch_gates_auto_path` / `settings_github_sync_dirty_from_write_events` / `todo_ui_event_wiring_write_paths` + 复审两条 `github_sync_backup_prune_failure_visible` / `settings_github_sync_exit_push_keeps_unsynced_notice_when_autopush_off`）
- 静态守卫：6 道守卫自检全过（模块隔离 / 探针跳过登记 / 探针门禁标签 / 余量落库路径 / 余量停机顺序 / Windows 交叉编译 + std-includes 自证非恒真）
- 定向复跑（改动面）：`--filter todo_ui` 52/52/0 ｜ `markdown_module` 78/78/0 ｜ `settings` 118/118/0 ｜ `github_sync` 28/28/0 ｜ `github`（platform）64/64/0
- 变异验证（修复类改动的「先红」证据）：`2dc3c92` 与 `b103399` 各自回退一次，对应新用例均红，恢复后均绿

### 覆盖缺口（诚实清单）

- **a3 无自动化用例**：启用流程的「校验 → 首同步」只能经真 API 驱动，而 `startGithubSyncAction` 会把 token 草稿写进**用户真实 keychain**（`DEVDESK_DATA_ROOT` 不隔离钥匙串）；不填草稿则走 `loadToken()` 读真 token、行为随机器状态漂移。两条路都不可接受 → 只做代码修改 + 静态走查。补用例需先加 token 注入缝（建议并入 backlog「编排层零测试覆盖」票）。
- **a1 的消费侧**用「无仓库 URL 时自动闸一放行即报错」的卡片红字作观察口（不起线程、不出网、不读 keychain），非直接读 `githubSyncDirty_`（该成员无外部观察面）。
- **agent 工具写路径不 emit**（`todo_tools.cpp` / `markdown_tools.cpp` 直连 service）：本票未动，属既有 backlog 登记项（工具有自触发循环风险，待用户拍板）。
