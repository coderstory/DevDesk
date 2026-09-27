# DevDesk 会话交接 — 2026-09-27 深夜（cache 闭环后）

> **本文件是会话级交接**（决策 / 环境 / 下一步）；仓库态细节见同目录 `2026-09-27-HANDOFF.md`；审计清单见同目录 `2026-09-27-full-audit-consolidated.md`。
> 用途：换会话/压缩后据此恢复。**先读本文件，再读仓库内同目录 `docs/superpowers/plans/2026-09-27-HANDOFF.md`（更细的仓库态）与 `docs/superpowers/plans/2026-09-27-full-audit-consolidated.md`（审计总表）**。
> 本文只放"别处没有的"：会话级决策、环境变更、下一步动作。

## 0. 一句话现状
Claude 代理/Profile 反向同步全链（M1–M5）已交付并合 master；六片全量代码审计完成（79 条，去重 77，高 9）；**编译缓存闭环**（冷 647s → 热 35s，命中 99.2%）；上游 EUI-NEO 吃了两批改动（U1 尺度契约 / U8 构建打包）。用户已**冻结新派单**直到 cache 处理完 —— **cache 已闭环，可以恢复派单**。

## 1. 仓库状态（一手实测）
| 项 | 值 |
|---|---|
| DevDesk master | `9dd9ee2`（今晚 `e99ee21..HEAD` 共 **48 个 merge**） |
| 工作区 | 干净（仅未跟踪 `?? .claude/`） |
| 测试 | 见 `build/tests/devdesk_tests.exe` 最新一轮输出（ccache 热构建下全量 ~35s） |
| 残留 worktree | `agent-a19de92747187fb7b`（**R0 线**，无提交；当时我 TaskStop 了它，transcript 还在 → `SendMessage` 唤醒即可续跑） |
| EUI-NEO | `269a8c4`（U1 已合），**领先 origin 70 个提交未 push**（本机无推送通道，属已知阻塞） |

## 2. 今晚交付了什么（细节在 artifact 里，勿重抄）
- **反向同步 + 复制 M1–M5**：spec `docs/superpowers/specs/2026-09-27-profile-reverse-sync-and-copy-design.md`；含 G1–G10 守卫、takeOver 陈旧键清理、快照污染守卫、无 sidecar 兜底、失败可上报、第 5 键「复制」UI。
- **六片全量审计**（core+platform / claude_proxy / todo+home+settings / pet+app+构建 / tests+基建 / markdown）：`docs/superpowers/plans/2026-09-27-full-audit-*.json`（6 份）+ 汇总 `docs/superpowers/plans/2026-09-27-full-audit-consolidated.md`（含 **Top10 + 三波修复计划（波内文件不冲突）+ 8 条待拍板**）。
- **已修的高危**：core 在 compose 内调 EUI 成员（CP-01）、JsonValue 双表示静默错值（CP-02）、接管失败不可见（CPX-02 服务+UI 两侧）、快照无来源判定（CPX-03）、构建脚本 worktree 不可用（PAB-01）、实时态锚冻结（MD-01，M2 引入的真缺陷）。
- **编译缓存**：提交 `3e7a600`（脚本+文档）+ `dc727cd`（CMake 自动探测 + CMakePresets）。
- **上游 EUI-NEO**：U8（不再强加 `-Os` / 放开 yyjson writer / macOS 守卫）+ U2①（markdown 缓存 8→64）→ `3d231be`；U1 尺度契约（`app::contentScale()` / `Screen::scale` / 指针事件 `scale`+`space`，纯增量）→ `269a8c4`；DevDesk 侧已采纳（todo 拖拽改吃 `contentScale()`，pet 判明不需要）。候选清单 `docs/superpowers/plans/2026-09-27-eui-upstream-candidates.md`（剩 U2②③④/U3/U4/U5/U6/U9/U10/U12）。
- **Markdown Phase 3**：M0（门禁 spike，V1 成立→单输入方案安全）/ M1（第四态骨架）/ M2（活动块单输入+内容贴合高度，`[live] input over=0` 达标）/ MD-01 修复，均已合。

## 3. 用户已拍板（不要再问）
- **cc-switch 是过渡**：不为它做兼容，DevDesk 是 `settings.json` 托管终局（过渡期"配置被还原"当噪音）。
- **审计口径**：必须**用子任务分片**跑 + 按**专业角色**（C++ 专家/架构师）+ 主线是**架构/设计模式/OOP/可维护性**（impact 写维护成本）；`memory/audit-focus-architecture-maintainability.md`。
- **子任务失败/被打断 → 唤醒原 agent 续跑**，不重派、不主会话继承；环境事件（系统休眠）同理。
- **并发上限 10**；**同一文件同一时刻只有一个写者**；构建类任务走 worktree。
- 共享配置片段：0..1 绑定、G10 守卫按"捕获目标所绑片段"、failover 请求级+60s 内存记忆、模型名换家时按槽位反查改写、片段 UI 落在 Claude 页内。
- 用量统计：**两者都做**（代理侧落库 ✅ 已合；会话 JSONL 扫描 ⏳ 未排）。

## 4. 环境变更（新会话必须知道，否则会踩）
- **ccache**：MSYS2 `mingw-w64-x86_64-ccache 4.14`；持久配置 `%LOCALAPPDATA%\ccache\ccache.conf` = `cache_dir D:/software/ccache-devdesk` + `base_dir D:/project/gui` + `hash_dir=false` + `max_size=20G`（**不需要任何 CCACHE_* 环境变量**）。CMake 已自动探测（`-DDEVDESK_CCACHE=OFF` / `-DCMAKE_CXX_COMPILER_LAUNCHER=` 可关）。实测：冷 647s，热 35s，237/239 = 99.2% 全 direct 命中；落后 40 提交的树 47.7%（正常）。
- **并行度**：12 核机器，多 worktree 并发时**必须** `CMAKE_BUILD_PARALLEL_LEVEL=3`（脚本已默认，可用 `DEVDESK_JOBS` 覆盖）。
- **裸 `cmake` 走 PATH masquerade 不可行**（MSYS2 shim 写死 `/mingw64`、自建 shim 破坏 `DEVDESK_MINGW_ROOT` 反推）——最小用法 = 原 `-D` 再加两个 launcher 参数（CMake 自动探测后连这两行也省）。
- **CodeGraph**：`D:\project\gui\codegraph.json` 已排除 `.claude/worktrees/**` 等；**待办：闲时跑一次 `codegraph index D:/project/gui`** 才会瘦身（当前索引 2.4GB、含 9 份副本）。
- **statusLine 已换 claude-hud**：备份 `~/.claude/settings.json.bak.20260927-223128`，旧命令存 `~/.claude/plugins/claude-hud/previous-statusline.txt`；注意该文件由 **cc-switch 托管**，可能被整体重写。
- **新装插件**：`mattpocock-skills@claude-plugins-official`（含 `/handoff`、`claude-handoff`、grilling、tdd、code-review 等）。

## 5. 已知坑（规则在 memory 里，别重抄）
`~/.claude/projects/D--project-gui/memory/`：
- `never-bulk-clean-live-worktrees.md`——**批量清 worktree 前必须判活 + 判定式含"有提交"**（我今晚误删三条在飞线的未提交改动，教训在此）。
- `cross-repo-work-needs-non-isolated-agent.md`——隔离 agent 碰不到别的仓库 git（连 `git -C` 都被拒）→ 跨仓库改动走"产补丁 + 非隔离线应用"；`git merge` 别与派单同一条消息并行发（git 锁会让隔离校验失败）。
- `devdesk-dev-workflow.md` / `devdesk-env-and-pitfalls.md` / `subagent-concurrency-cap-7.md`（现值 10）/ `main-session-no-direct-file-edits.md` / `audit-focus-architecture-maintainability.md` / `find-third-way-before-forcing-choice.md` / `resume-subagent-on-abnormal-exit.md`。

## 6. 下一步动作（按序）
1. **确认 master 全量绿**（本文件生成时在后台跑；若未完成，先跑 `scripts/build-test-run.sh --no-run`）。
2. **恢复派单**：把审计汇总的 **波 1（10 条线 / 约 25 条 findings）** 派下去——照汇总 §4 的"波内文件不冲突"清单，每条一个 worktree、限 `-j 3`、跑定向测试后原子 commit。
3. **8 条待拍板**拿给用户（托盘死代码、reorderColumns 负位、归档 kind、NotFound 承诺、逾期口径、皮肤生成器、-Werror 覆盖、macOS 侧声明）——见汇总 §5。
4. **R0 唤醒续跑**（`SendMessage` 给 `agent-a19de92747187fb7b`）：它是 P1 重构，但**必须同批改 `tests/test_settings.cpp:settings_module_isolation_guard`**（TIN-01：现守卫把 `settings→claude_service.h` 耦合钉成不变量，R0 一落地必红）。
5. 之后按队列：共享片段 UI 批 → failover 批（v7 migration + proxy_router 结构）→ cc-switch 参考剩余可抄项（上游连接复用等）→ Markdown M3–M10 → R1–R9。

## 7. 建议调用的 skills（下一位 agent 用 Skill 工具显式调用）
- **`mattpocock-skills:grilling`** — 派单前对方案/拍板项做拷问（用户偏好先碰硬问题）。
- **`mattpocock-skills:code-review`** — 合并前双轴审查（Standards + Spec）。
- **`superpowers:dispatching-parallel-agents`** + **`superpowers:subagent-driven-development`** — 多线并行的派单与逐 task 双审流程。
- **`x-coding-tools:cs-web-fetch`** — 任何联网检索（企业网下唯一可靠；Google 会撞人机校验，改用 Bing 或 GitHub API）。
- **`x-coding-tools:cs-pdca-auditor`** — 里程碑级多轮审计。
- **`verify-and-stop`** / **`surgical-patch`** — 只验收不改范围 / 最小层修复。
- **`cs-retro:cs-retro`** — 本轮多处"被用户纠正/非显然修复"，适合做一次复盘沉淀。

---
生成于 2026-09-27 夜，基于会话 `gui-97`。所有 sha 与数字均为当轮实测；仓库内更细的状态以其 HANDOFF 文档为准。

数字校准：2026-09-27 实测（master tip `dc727cd` → `9dd9ee2`（含 EUI rev 修正 `be9b44f`/`9dd9ee2`）；`e99ee21..HEAD` merge 数 47 → 48）。
