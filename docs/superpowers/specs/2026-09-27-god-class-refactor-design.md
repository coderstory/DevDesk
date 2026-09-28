# DevDesk 上帝类治理设计（CPX-01 / CPX-05 / THS-02）

- 日期：2026-09-27 ｜ **2026-09-28 刷新**（R10 段基线重测 + 8 项缺口补全 + 前置状态更新 + 独立审计 10 项 required_change 已合入【v1.1】；待主会话 review + 再一轮审计后再派开发）｜ 状态：**设计（未实施）** ｜ 基线：master `753fc92`
- 关联：`2026-09-27-refactor-incremental-design.md`（R0–R9 总路线，本文**不重排其顺序**）、`2026-09-20-devdesk-design.md` §4.2/§4.2.1（依赖边界）、`2026-09-27-full-audit-claude-proxy.json`、`2026-09-27-full-audit-todo-home-settings.json`、`2026-09-27-full-audit-pet-app-build.json`
- 证据口径：行数/成员数/调用点为**实读实测**（§2 CPX-01 段 = **2026-09-28 重测**，§3–§4 = 2026-09-27 基线；`wc`/`grep -c`/brace-matched awk）；行号只作定位线索、**会漂移**（R spec §9 V7），动手前按符号名重新定位
- **只出设计，零代码改动**。红线沿用 R spec §1（C1 增量 / C2 不行为变 / C3 测试是护栏）与 §2（R1–R7），不重复

---

## 1. 与 R0–R9 的关系（先定归属，避免两套路线）

| 本文组 | 归属 | 落地方式 | 依据 |
|--------|------|----------|------|
| **CPX-05** `claude_module` 拆分 | **并入 R4** | R4 原范围是"4 个新文件 + logic 头"；本文补两步：**状态 struct 收拢**、**详情 pane 返回内容底沿契约** | 同一文件、同一里程碑，新开 = 拆散验收 |
| **THS-02** `TodoModule` 拆分 | **并入 R5** | R5 原范围是"文件级拆"；本文补 **dialog struct 化**（成员与 draw/submit 同居） | 同上；且 R5 的 6 对话框清单与 THS-02 实测一致 |
| **PAB-02** 模块接入成本 | **并入 R8** | R8 已有"托盘两表合一"；本文补 `ModuleSet` 定长结构 → 表驱动 | 同属装配面，R8 缺的正是这一步 |
| **CPX-01** `ClaudeService` 拆分 | **另开 R10** | R0 只消解**跨模块调用方式**（D6/D7）；R spec §9 V6 明写"若要一并收敛单例债须单开里程碑" | 服务层拆分与任何模块内闭环都不重叠，塞进 R4/R5 会撞车 |

> 本文不新增技术栈、不引入依赖、不改 `core::Module` 接口（R8 已拍板的 `shortName()` 除外）。

---

## 2. CPX-01：ClaudeService 服务层拆分（R10）

### 2.1 现状量化与证据

| 指标 | 实测值 | 证据（file:符号名） |
|------|--------|---------------------|
| 头文件 / 实现 | **469 行 / 2005 行**（2026-09-28 重测；刷新前基线 336/1420） | `src/module_claude_proxy/claude_service.h`、`.cpp` |
| 公开方法 | **43 公开成员 = 41 非静态 + 2 公开静态 `setGlobal`/`global`（h:374/375）**（含 1 个 inline 转发 `reapplyTakeOver`；**私有**静态 `sha256HexPrefix`/`makeApiKeyHint`（h:407/408）与 constexpr 不计入；TwoPhaseKeyWrite 类自身 4 方法不计）｜ 增量（自 33）= shared-config 5 公开 + `runProfile`/`setTerminalLauncher`/`testConnection`/`cleanupOrphanKeychainEntries`/`reapplyTakeOver` | `claude_service.h` 类体逐条实测（TwoPhaseKeyWrite 独立类 `:93-118` 不计；计数口径见 §8 V2） |
| 职责 | **7 类**：Profile CRUD / Keychain / 调用日志 / 代理生命周期+在途流 / settings.json 接管 / 反向同步 / 桌宠事件邮箱 | 同上，各分区以 `// ---- 标题 ----` 分节 |
| 状态组 | **6 组，各带不同线程约定**（+ `terminalLauncher_` 终端注入槽） | `.h` 私有成员区 `db_`+`keychainService_`、`server_`+`router_`、`streamsMutex_`+`activeStreams_`+`streamsStopping_`、`configuredProxyPort_`、`proxyEventMutex_`+`proxyEventStatus_`+`proxyEventAt_`、`syncPaths_`+`settingsTakeOverEnabled_`+`lastSyncWarnings_`、`callLogVersion_`+`terminalLauncher_` |
| 最长单函数 | `syncCaptureActiveProfile` **168 行**（2026-09-28 实测 `:1748` → `:1915`；反向同步协议整段塞在 CRUD 类里） | `claude_service.cpp: ClaudeService::syncCaptureActiveProfile` |
| 已抽出的邻居（说明"协议"部分其实已经出去了） | `provider_config` / `settings_sync` / `streaming_proxy` / `proxy_router` / `upstream_dialect` 5 个单元 + **`TwoPhaseKeyWrite`（CPX-07 已独立成类）** | `src/module_claude_proxy/CMakeLists.txt` 源文件清单（:2-14）；`TwoPhaseKeyWrite` 类声明 `claude_service.h:93-118` + 实现段 `claude_service.cpp:116-190`（**仍在服务文件内，随 P-A2 整类迁出，见 §2.3**）、4 处复用 `claude_service.cpp:415/571/683/1846` |
| 调用点 | `claude_module.cpp` **11**、`main.cpp` **1**（装配层，合法）、`settings_module.cpp` **3**（跨模块，R0 范围） | `grep -rn "ClaudeService::global()" src/`（2026-09-27 基线；R0 落地后 settings_module 侧可能已走定位器，开工前重测） |

**没有被"顺带塞进来"的两处**（本文不动，只登记）：`deleteProfile` 内联「删激活 profile 则停代理」策略、`setActiveProfile` 内的接管时机判断——它们是**策略**，正确归宿是门面而非子对象。2026-09-28 新增同族候选：`runProfile`/`setTerminalLauncher`/`testConnection` 归属见 §2.3 尾注与 §7 Q4/Q5（均【待拍板】）。

### 2.2 目标职责边界

铁律一条：**子对象互不认识、互不引用**；一切跨对象顺序由门面按既有顺序编排（例如现状 `startProxy` 内部调 `syncTakeOver`，拆分后该调用点**上移到门面**，调用顺序逐字保持）。

| 类（新文件） | 单一职责（一句话） | 持有的不变式 | 线程约定 |
|--------------|-------------------|--------------|----------|
| `ProfileRepository`（`profile_repository.h/.cpp`） | `claude_profiles` 行 + **SharedConfig 片段** + Keychain 条目的**唯一**持久化入口 | API key 明文永不入 SQLite；Keychain 条目与行一一对应；`is_active` 全局唯一；SharedConfig 悬空 id 读路径容错（退化为未绑定） | 无自有锁，随调用方（现状同款：`SqliteDb` recursive_mutex + Keychain 自身） |
| `CallLogStore`（`call_log_store.h/.cpp`） | `claude_call_logs` 读写 + 落库版本号 | 每次成功写入 bump 版本（UI 据此感知后台落库） | 写 = proxy worker 线程；读 = UI 线程；版本号 `atomic`（现状同款） |
| `ProxyRuntime`（`proxy_runtime.h/.cpp`） | 代理 server 生命周期 + 在途流登记 + 端口镜像 | **停机顺序不可变**：cancelActiveStreams → server->stop（否则退出留幽灵进程）；`stopProxy` 幂等 | worker 写登记表 / UI 线程发起 cancel；`streamsMutex_` 保护 |
| `ProxyEventMailbox`（并入 `proxy_runtime`，同文件一段） | 代理终态事件单槽投递（take 语义 + 过期丢弃） | 一个事件最多被读走一次；超 `kProxyEventFreshSec` 视为过期 | worker 写 / UI take；mutex。**与登记表同写者-读者对，故随 `ProxyRuntime` 走**，不单独成类 |
| `SettingsTakeOver`（`settings_takeover.h/.cpp`） | settings.json 接管/还原的**协议侧**（读文件 → 算期望 → 写文件/sidecar） | 键级还原；接管失败不阻断上层操作（错误并入 `lastSyncWarnings_`） | 仅 UI 线程（现状同款） |
| `ClaudeService`（**保留**，退化为门面） | **保留 43 个公开成员签名（41 非静态 + 2 公开静态）不变**（§2.1 口径；Q1=A）；唯一编排点与策略所有者（停代理策略 / 接管时机 / 失败呈现 / 终端启动编排） | 各子对象线程约定不变 | 各子对象内部约定不变；门面自身不新增状态 |

**为什么门面必须留**：43 个方法里约 1/3 是「先落库再动代理/接管」的**编排**（如 `deleteProfile` 的两段策略、`setActiveProfile` 的接管时机）——它不是"转发嘴"，而是策略层。判据见 §5.3 J2。

### 2.3 迁移路径（每阶段 = 1 原子 commit、行为零变化、可单独 revert）

| 阶段 | 动作（file:符号名 — 动作） | 为什么这个顺序 | 硬依赖 |
|------|---------------------------|----------------|--------|
| **P-A1** | 抽出 `CallLogStore`：`logCall`/`recentCalls`/`queryCalls`/`countCalls`/`callLogVersion` + `buildCallLogWhere`；门面转发 | 最独立（4 方法 + 1 atomic、零 Keychain/代理/接管交互），**无跨对象状态交互**（写侧 `proxy_router` 5 处 :250/:291/:387/:423/:462、读侧 `claude_ui_logs`） | — |
| **P-A2** | 抽出 `ProfileRepository`：CRUD 6 方法 + `loadApiKey`/`getActiveProfile`/`setActiveProfile`①/`duplicateProfile`/`rowToProfile`/`sha256HexPrefix`/`makeApiKeyHint` + **shared-config 全家**：`listSharedConfigs`/`createSharedConfig`/`updateSharedConfig`/`deleteSharedConfig`/`sharedEnvKeysFor`（h:209-218）+ 私有 `rowToSharedConfig`/`sharedExtraEnvJsonFor`/`requireSharedConfigExists`（h:398-405）+ **`cleanupOrphanKeychainEntries`**（def :728、内部 keychain:: 命中 :742——「Keychain 条目与行一一对应」= ProfileRepository 不变式）；**`TwoPhaseKeyWrite` 类整迁**（声明 h:93-118 + 实现 cpp:116-190，类内 keychain:: 命中 :124/:131/:132/:159/:160；4 处复用 :415/:571/:683/:1846——前 3 处随落库段进本类、:1846 在留门面的 `syncCaptureActiveProfile` 内改引迁出后类型；并入 profile_repository 或独立文件）；**4 种「Keychain+DB 两段写」策略原样搬迁不重写**；含编排尾巴的 5 方法按**段级拆分**执行（见下） | 先落库、后运行时；A3/A4 都要用它 | — |
| **P-A3** | 抽出 `ProxyRuntime`（含 mailbox）：`startProxy`/`stopProxy`/`isProxyRunning`/`proxyPort`/`configuredProxyPort`/`setConfiguredProxyPort`/`registerActiveStream`/`cancelActiveStreams`/`recordProxyEvent`/`takeProxyEvent`；`testConnection` 归属待拍板（§7 Q5，若归本阶段则并入）；**`stopProxy` 段级拆分**：`ProxyRuntime::stop()` = cancelActiveStreams → `server_->stop` → 状态 reset（:1551-1564 停机顺序不变式、幂等），门面留 `stopProxy(){ runtime->stop(); syncRestore(); }`（:1563 的 `syncRestore` 尾，A4 后改经 takeover 子对象） | 风险最高（线程 + 停机顺序），放中间验证窗口；A2 已就位便于隔离故障 | — |
| **P-A4** | 抽出 `SettingsTakeOver`：`syncTakeOver`/`syncRestore`/`recoverPendingSettingsRestore`/`setSettingsSyncPaths` 的路径判定 + `takeOver`/`restore`/`hasPendingSnapshot`（×2，cpp:1726/1961）/`readSettingsEnvForSync` **6 个** `settings_sync::` 调用点（`setSettingsSyncPaths` 签名另 1 处 `Paths` 类型引用一并归零）——**`syncCaptureActiveProfile` 内的读侧 1 处（:1763）除外**，归属裁定见 §2.5 否决 4（口径 b，断言 ≤1）；**`syncTakeOver` 双入参（激活 Profile + 当前代理端口）**：现状 cpp:1937 `settings_sync::takeOver(active, proxyPort(), ...)` 的 `active`（A2）与 `proxyPort()`（A3）两依赖即此，入参由门面从 `ProfileRepository` 取激活 Profile、从 `ProxyRuntime::proxyPort()` 取端口后一并传入，子对象不反向依赖彼此；**收尾 CPX-02 残项**：`syncRestore` 改 `Result<void>`（唯一调用 `stopProxy` cpp:1563，失败并入 `lastSyncWarnings_`）**【待拍板，Q6】** | 依赖 A2（取激活 Profile）+ A3（proxyPort）；与 A3 的调用点（起/停代理）相邻，顺序由门面保住 | **A2、A3** |
| （可选）**P-A5** | 调用方改持子对象（`claude_module` 直接拿 `CallLogStore&`）、删转发方法、消解 `ClaudeService::global()` 单例债 | 非必需；见 §2.5 否决 2 与 §7 Q1 | A1–A4 |

**段级拆分定义（P-A2，含编排尾巴的 5 方法）**——落库/Keychain 段随 A2 搬入 `ProfileRepository`；「策略尾」（停代理/接管/激活时机判断）**留门面**，P-A2 commit 内逐字不动，其后由 A3/A4 改为经对应子对象编排（调用顺序逐字保持）：

| 方法 | 随 A2 搬出的段 | 留门面的策略尾 |
|------|----------------|----------------|
| `createProfile`（自动激活路径 :481） | insert 行 + TwoPhaseKeyWrite 两段写（API key 入 Keychain，:415） | 自动激活仅调门面 `setActiveProfile`（不内联激活落库） |
| `updateProfile`（策略尾 :660-663） | DB 更新段 + 两段写（含 sharedConfigId 变更落行，:571） | `sharedConfigId` 变更 && 激活 && 代理运行 → `syncTakeOver`（:662；A4 后改经 takeover 子对象编排） |
| `deleteProfile`（策略尾 :720-724） | 两段写删除段 + 删 SQLite 行（:683、:694） | `wasActive && isProxyRunning` → `stopProxy`（:722；A3 后改 `runtime->stop() + syncRestore`） |
| `setActiveProfile`①（同步 :818 / 接管 :856） | 激活行更新段（is_active 全局唯一） | 接管时机判断 + `syncCaptureActiveProfile`（:818）+ `syncTakeOver`（:856）——留门面（Q2 否决 4、§2.1 策略归属） |
| `updateSharedConfig`（re-takeover 尾 :1131） | shared-config 行更新段 | 改后生效 → `syncTakeOver`（:1131；A4 后改经 takeover 子对象编排） |
| `deleteSharedConfig`（re-takeover 尾 :1189） | shared-config 行删除段（同事务解绑 shared_config_id） | 删后生效 → `syncTakeOver`（:1189；A4 后改经 takeover 子对象编排） |

① 补注（原悬空脚注）：P-A2 清单中 `setActiveProfile` 仅搬迁「激活行更新」段，「同步 + 接管」编排不随搬。

**A1/A2/A3 数据面相互独立（调用点零重叠），但同触 `claude_service.{h,cpp}` → 必须串行**（不可并行 worktree）；**跨对象顺序（startProxy 尾 `syncTakeOver` :1547、deleteProfile 尾 `stopProxy` :722、shared-config 两个 re-takeover 尾等）全部经门面**按既有顺序逐字编排。**`syncCaptureActiveProfile`（168 行反向同步，:1748→:1915）留在门面不搬**，理由与断言口径见否决 4。

**CMakeLists 补位（每阶段 commit 的固定成员）**：`src/module_claude_proxy/CMakeLists.txt` 为显式源文件清单、无 glob（现 :2-14 逐行列出）；4 对新文件随阶段补位——A1→`call_log_store.{h,cpp}`、A2→`profile_repository.{h,cpp}`、A3→`proxy_runtime.{h,cpp}`、A4→`settings_takeover.{h,cpp}`；验收含「新清单行存在、无悬空/缺失项」。

**新方法归属（2026-09-28 新增面，均【待拍板】，去向见 §7 Q4/Q5）**：
- ① `runProfile`/`setTerminalLauncher`（h:321/326、cpp:1585/1591，profile-run 设计 §6）——建议**留门面**：runProfile 编排「取 profile → 终端终态」，非 A1-A4 任一子对象，`terminalLauncher_` 注入槽随留；
- ② `testConnection`（h:226、cpp:1242）——建议**留门面**：编排「`loadApiKey`（A2）→ 上游 HTTP 直连」，与 ProxyRuntime 的本地 server 生命周期无关。

### 2.4 每阶段验收

定向测试（子串过滤，`devdesk_tests.exe <子串>`；括号内为 `REGISTER_TEST` 实测数）：

| 阶段 | 定向测试 | 结构断言（可机械检查） | 肉眼 |
|------|----------|------------------------|------|
| P-A1 | `claude_service`(30) + `claude_ui`(19) | `claude_service.cpp` 中 `claude_call_logs` 命中 → **0**（断言须排除注释：:694「FK CASCADE 会清空 claude_call_logs」属 `deleteProfile` 段二、P-A2 才迁出——随 P-A1 改写该注释去掉表名字面量，或断言仅统计非注释命中）；`call_log_store.cpp` > 0 | 否 |
| P-A2 | 同上 + `settings_sync`(21) | `claude_service.cpp` 中 `claude_profiles` → **≤1 且仅限 `syncCaptureActiveProfile` 内**（:1879 `UPDATE claude_profiles`，Q2 否决 4 保留）；`keychain::` → **≤2 且仅限 `syncCaptureActiveProfile` 及其唯一助手 `keyAtOldValue`**（:1811 `loadSecret` / :80；现状 9 处 = TwoPhaseKeyWrite 类内 5（:124/:131/:132/:159/:160）+ `cleanupOrphanKeychainEntries` :742 + `loadApiKey` :761 + 上记 2，前 7 处随 A2 迁出后余 2）；`TwoPhaseKeyWrite` 类体 → **0**（声明段 + 实现段全迁出） | 否 |
| P-A3 | `claude_service` + `proxy_router` + `streaming_proxy` + `streaming_stress` + `claude_ui` | `claude_service.cpp` 中 `streamsMutex_`/`activeStreams_`/`http::Server` → **0** | **是**：起代理 → 真实请求一次 → 停代理 → 退出不留幽灵进程 |
| P-A4 | `settings_sync`(21) + `claude_service` + `claude_ui` | `claude_service.cpp` 中 `settings_sync::` 命中 **≤1 且仅限 `syncCaptureActiveProfile` 内**（:1763 `readSettingsEnvForSync`——Q2 否决 4 裁定该读侧留门面，口径 (b)，见 §2.5 否决 4；如开发中选口径 (a)（经 `SettingsTakeOver` 暴露等价读接口、门面不再直呼）则归零）；其余 **5 处调用归零**：`hasPendingSnapshot` ×2 @:1726/:1961、`recoverPendingSnapshot` :1740、`takeOver` :1937、`restore` :1970；另 `setSettingsSyncPaths` 签名 `Paths` 类型引用 1 处同归零 | **是**：起代理看 `~/.claude/settings.json` 被接管、停代理还原 |
| 收口 | 全量全绿 | `wc -l claude_service.cpp` ≤ **400**、`claude_service.h` ≤ **120**（2026-09-28 重测基线 2005/469，收口断言不变）；四对新文件全部在 CMakeLists 显式清单 | — |

每阶段另跑一条**跨模块回归**：`module_gating`(11)（`settings_module` 经单例读 Claude 状态，R0 后走定位器，不能断）。

### 2.5 被否决的备选

| # | 备选 | 否决理由 |
|---|------|----------|
| 1 | 接口化 + DI 容器重写（`IProfileRepository` + 注入） | 无第二实现者（无 mock/无平台分歧）；R spec §4.3 已明确否决 DI 容器；用户不熟 C++，虚接口会掩盖真实数据流 |
| 2 | **首步就删 43 个转发方法、14 处调用点改为直持子对象** | 一步同时改契约 + 14 个调用点，无法单独回退、也无法用"测试即护栏"证明；降级为可选 P-A5 独立里程碑 |
| 3 | 同模块内拆成自由函数 + 显式传 `db_`/`mutex` | 自由函数**无法持有**互斥量与生命周期（`server_`/`activeStreams_`），违反 §5.3 J2；且 `core::Result` 链会退化成参数搬运 |
| 4 | 把 `syncCaptureActiveProfile`（反向同步 168 行）也搬进 `SettingsTakeOver` | 它跨 DB + Keychain + 文件三写，且带 S-2 语义（解析失败 → **中止本次切换、profile 一字未改**）与 4 种补偿策略之一；行为等价性难自证，而 168 行**已具名、已注释完备**，搬迁收益低于风险（§5.3 J5；2026-09-28 重测行号 :1748→:1915，判定不变）。**留门面的读侧归属裁定（2026-09-28 审计修订）**：体内容 `settings_sync::` 读侧 1 处（`readSettingsEnvForSync` :1763）、`keychain::` 2 处（`loadSecret` :1811 + 唯一助手 `keyAtOldValue` :80）、`claude_profiles` 写 1 处（:1879 UPDATE）**不随 A2/A4 迁出**，故 P-A2/P-A4 结构断言口径放宽：(a) 经 `SettingsTakeOver` 暴露等价读接口、门面不再直呼（断言可归零）；(b) **断言改「`settings_sync::`/`claude_profiles` ≤1、`keychain::` ≤2，且仅限本函数（含助手 `keyAtOldValue`）」**（推荐，最小改动）——A2/A4 验收按 (b) 执行 |

### 2.6 风险与明确不改项

| 项 | 说明 |
|----|------|
| 主要风险 | P-A3 的停机顺序与生命周期（`router_` 持 `server_` 引用、worker 线程 join）——回退即整段 revert，不允许半套 |
| 前置 | R spec §5.1 P1–P4；`2026-09-27-profile-reverse-sync-and-copy-design.md`（reverse-sync/copy）**已合 master**；profile-run 设计（§6）**已合**——`runProfile`/`setTerminalLauncher` 已入（h:321/326、cpp:1585/1591）；R4/R5 已合（`claude_ui_*` 6 文件已在 CMakeLists :3-8）。2026-09-28 复核全部满足 |
| 明确不改 | CPX-03（快照来源判定）、CPX-04（热切换残留键）、CPX-06（弹窗同步查库）——**只登记不修**；本次只做"移动 + 改名 + 参数化"。**前置状态变更（2026-09-28 复核）**：CPX-07（Keychain+DB 两段写）**已完成**——`TwoPhaseKeyWrite` 独立成类（声明 `claude_service.h:93-118`，实现段 `claude_service.cpp:116-190` 仍在服务文件），4 处复用（cpp:415/571/683/1846）；**2026-09-28 审计修订：实现段含 `keychain::` 命中 :124/:131/:132/:159/:160（另匿名 ns 助手 `keyAtOldValue` :80 同区，`syncCaptureActiveProfile` 唯一使用）——随 P-A2 整类迁出 `claude_service.{h,cpp}`**（并入 profile_repository 或独立文件，见 §2.3），调用点跟随，此前「P-A2 直接复用」表述作废；CPX-14（keychain 函数指针表）**已完成**——`keychain.h:34-101` Backend 表 + `setBackendForTest`，R10 无需动作；CPX-02（接管返回 void）**半修**——`syncTakeOver` 已返 `Result`（cpp:1917），残 `syncRestore` 仍 void（h:415 区、cpp:1956，唯一调用 `stopProxy` cpp:1563）——**推荐并入 P-A4 收尾【待拍板，Q6】**，与 `2026-09-27-full-audit-consolidated.md` 行 152「R10 承接收尾」口径对齐、消除两处文档冲突 |
| 明确不改 | `ClaudeService::global()` 单例本身（R0 只管跨模块那 3 处；同模块 11 处见 §7 Q1） |

---

## 3. CPX-05：claude_module UI 与逻辑分离（并入 R4）

### 3.1 现状量化与证据

| 指标 | 实测值 | 证据（file:符号名） |
|------|--------|---------------------|
| 单文件 | **2612 行**（审计期 2577，已增长） | `src/module_claude_proxy/claude_module.cpp` |
| `compose` | `:135` → `:1018` = **884 行**（审计期 863） | `void ClaudeModule::compose(eui::Ui&, const eui::Screen&)` |
| 类边界 | `class ClaudeModule : public ::core::Module` `:120` → `private:` `:1105` → `:2606` | 同上 |
| 成员 | **41 项**（状态区 `// ============ 状态 ============`）：缓存/分页/滚动/导入/表单/弹窗/toast 混排 | 同上，成员区尾部 |
| 已抽出的方法（说明"该拆"早有先例） | 8 个 `draw*`：`drawProfileCard` / `drawFormBasicTab` / `drawFormModelsTab` / `drawFormEnvTab` / `drawProfileFormDialog` / `drawDeleteConfirmDialog` / `drawCallsDialog` / `drawToast`，+ `serializeFormData` / `applyCallsFilter` / `refreshDataCache` / `applyDataRefresh` | 同上 |
| 未抽出的部分 | **右栏三个详情 pane**（`detailTab_ == 0/1/2` 的 if/else 内联，含"基础/模型映射/环境变量"字段块）、右栏操作按钮组、左栏列表、header + 引导条 | `compose` 内 `detailTab_ ==` 三处分支 |
| 纯逻辑混在 UI 文件 | `formatLatency` / `truncateUtf8` / `modelsSummary` / `suggestImportName` 全在**匿名命名空间**（无法单测） | 文件顶部匿名 `namespace {` |

### 3.2 目标职责边界

| 单元（新文件） | 单一职责 | 形态 |
|----------------|----------|------|
| `claude_module.cpp`（保留） | `Module` 接口 + `compose` **骨架**（分区委派 ≤120 行）+ 事件回调接线 + onStart/onStop | 目标 **≤600 行** |
| `claude_ui_detail.cpp` | 右栏三 pane（基础/模型映射/环境变量）+ 操作按钮组 | `float drawDetailBasic/Models/Env(ui, x0, y0, w, const DetailState&) → 内容底沿`（按钮组按底沿定位，取代手算 y 偏移） |
| `claude_ui_list.cpp` | 左栏 Profile 列表 + header/引导条 | 自由函数 |
| `claude_ui_form.cpp` | 表单 4 tab + 编辑弹窗 + 删除确认弹窗 | 自由函数（`serializeFormData` 随之搬迁） |
| `claude_ui_logs.cpp` | 调用记录弹窗（过滤/分页） | 自由函数 |
| `claude_ui_state.h` | 41 成员按区域收成 struct：`ProfileListState` / `DetailState` / `FormState` / `LogsState` / `ImportState`（+ `testThread_`） | 纯值类型，无方法（除必要的不变式断言） |
| `claude_ui_logic.h/.cpp` | 4 个纯函数 + `kCalls*` 常量，从匿名 ns 提升 | 可无头单测 |

### 3.3 迁移路径

| 阶段 | 动作 | 硬依赖 |
|------|------|--------|
| **P-B1** | 纯函数提升：`formatLatency`/`truncateUtf8`/`modelsSummary`/`suggestImportName` + `kCallsStatusClasses`/`kCallsPageSize` → `claude_ui_logic.{h,cpp}`，补等价断言（R spec N9 允许） | — |
| **P-B2** | 41 成员按区域收进 `claude_ui_state.h` 的 5 个 struct；`compose` 内引用改 `state_.xxx.yyy_`（纯机械改名，编译期全量校验） | B1 |
| **P-B3** | 右栏三 pane 抽 `claude_ui_detail.cpp`，确立**返回内容底沿**契约 | B2 |
| **P-B4** | 左栏列表 + header/引导条 → `claude_ui_list.cpp` | B2 |
| **P-B5** | 表单 4 tab + 2 弹窗 → `claude_ui_form.cpp`；日志弹窗 → `claude_ui_logs.cpp` | B2 |

B2 必须在 B3–B5 之前：抽出的自由函数若还要捕获 `this`，等于没拆（41 个成员就是 884 行的根因）。B3/B4/B5 三者互不重叠，但同触 `claude_module.cpp` 的 `compose` 骨架 → **串行**落地。

### 3.4 每阶段验收

| 阶段 | 定向测试 | 结构断言 | 肉眼 |
|------|----------|----------|------|
| P-B1 | `claude_ui`(19) + 新增 `claude_ui_logic` 子串 | `claude_module.cpp` 匿名 ns 中 4 个函数名 → **0** | 否 |
| P-B2 | `claude_ui` | 成员区 `sed` 计数：`claude_module.cpp` 顶层成员 ≤ **12**（只剩 state 容器 + 服务指针 + 线程） | 否 |
| P-B3 | `claude_ui` | `claude_module.cpp` 中 `detailTab_ ==` 分支 → **0**；`claude_ui_detail.cpp` > 0 | 是：右栏三 pane + 操作按钮全点一遍 |
| P-B4 | `claude_ui` | `claude_module.cpp` 中 `drawProfileCard` → **0** | 是：左栏列表 + 选中态 + 引导条 |
| P-B5 | `claude_ui` | `claude_module.cpp` 中 `drawFormBasicTab`/`drawCallsDialog` → **0** | 是：表单 4 tab + 编辑/删除弹窗 + 日志弹窗（含搜索/翻页） |
| 收口 | 全量全绿 | `compose` 跨度 ≤ **120 行**（awk brace-matched）；`wc -l claude_module.cpp` ≤ **600** | — |

肉眼点验沿用 R spec 已拍板口径（D-R1：EUI-NEO 组件无头用例覆盖不到真实渲染）；无头 `compose` 冒烟测试（`test_claude_ui`，照 settings T11 模式）是每阶段的机器护栏。

### 3.5 被否决的备选

| # | 备选 | 否决理由 |
|---|------|----------|
| 1 | 只在 `compose` 内把大段拆成 lambda（不建自由函数） | 不解决成员耦合与"行号级绝对坐标"问题——审计 M5/M6 那类对偶字面量缺陷会原样复发 |
| 2 | 自研 widget/组件树层（或引入 React 式 diff） | 属 EUI-NEO 上游职责（CLAUDE.md 上游政策）；R spec §4.3 已否决自造抽象 |
| 3 | 三个 pane 各自做成 Router 页（`PageId` 新增） | **改产品行为**（页签语义 → 导航语义），违反 C2/R6 |
| 4 | 顺手修 CPX-06（日志弹窗同步查库、搜索框逐键两查） | 行为/性能面，不是结构搬移；同时改两件事 = 无法用"行为零变化"验收。只登记 |

---

## 4. THS-02：TodoModule 拆分（并入 R5）

### 4.1 现状量化与证据

| 指标 | 实测值 | 证据（file:符号名） |
|------|--------|---------------------|
| 单文件 | **2474 行** | `src/module_todo/todo_module.cpp` |
| 类边界 | `class TodoModule : public ::core::Module` `:170` → `private:` `:713` → `:2469` | 同上 |
| 成员 | **54 项声明**（审计记 ~55） | 同上，成员区（`service_`/`columns_`/`tasks_` 起） |
| `draw*` 方法 | **14 个**：`drawToast`/`drawHeader`/`drawBoard`/`drawColumn`/`drawColumnContent`/`drawCard`/`drawNewTaskDialog`/`drawNewColumnDialog`/`drawContextMenu`/`drawRenameDialog`/`drawConfirmDeleteDialog`/`drawDatePicker`/`drawOverdueDialog`/`drawDetailDialog` | 同上 |
| 浮层/弹窗 | **6 个对话框**（新建任务/新建列/重命名/删除确认/逾期/详情）+ 右键菜单 + datepicker + toast | 同上 |
| 已抽出的邻居（正面先例） | `todo_service.cpp`(813) / `todo_view_state.{h,cpp}`(204) / `todo_ui_logic.h`(235) | `src/module_todo/` |
| 无法在类型上表达的不变式 | ① `detailDiscardArmed_` 只在 `detailOpen_` 时有效（有未存改动时首次点遮罩只警告）② `confirmTaskId_` 与 `confirmColumnId_` 二选一 ③ 右键菜单 `ctxTaskId_` 与 `ctxColumnId_` 二选一 | 成员区注释自陈 |

### 4.2 目标职责边界

| 单元（新文件） | 单一职责 | 持有的不变式 |
|----------------|----------|--------------|
| `todo_module.cpp`（保留） | `Module` 接口 + `compose` 骨架 + **拖拽状态机** + 数据 reload | `drag_` 的 `pressX`/`pressY`/`pressScrollOffset` **同帧快照**（tech-debt B，不得动摇）；目标 ≤700 行 |
| `todo_dialogs.h` | 6 个对话框 struct，各自「open 状态 + 表单字段 + 渲染 + 提交」四者同居 | `DetailDialog`：`discardArmed` 仅在 `open` 时有效，守卫方法（现 `requestCloseDetailDialog`）随之同居；`ConfirmDeleteDialog`：task/column 二选一；`ContextMenu`：task/column 二选一 |
| `todo_ui_dialogs.cpp` | 6 个对话框 + 右键菜单 + datepicker 的绘制 | — |
| `todo_ui_board.cpp` | `drawHeader`/`drawBoard`/`drawColumn`/`drawColumnContent`/`drawCard` | 与 `boardColW_`/`boardCardW_` 等几何快照同源 |
| `todo_ui_logic.h`（已有，合并） | `boardGeom`/`cardLeft`/`cardTop`/`chipWidth`/`dragDropTarget` 命中换算等纯函数 | 不碰 `Ui`、不碰 DB |
| `ToastState` | **迁 `ui_kit`（R1 已排）**，本模块只留接线 | 单实例节流语义 |

对话框成员聚类（实测，供 struct 化对齐）：`NewTaskDialog`(4 项) / `NewColumnDialog`(3) / `RenameDialog`(4) / `ConfirmDeleteDialog`(4) / `DetailDialog`(11 + `datePickOpen_`，datepicker 由详情弹窗的到期日按钮打开，实测 `:2200` 在 `drawDetailDialog` 内) / `ContextMenu`(5) / `OverdueDialog`(3)。

### 4.3 迁移路径

| 阶段 | 动作 | 硬依赖 |
|------|------|--------|
| **P-C1** | 纯函数收口：`boardGeom`/`cardLeft`/`cardTop`/`chipWidth`/`dragDropTarget` 命中 → `todo_ui_logic.h`（与既有 235 行合并），补等价断言 | — |
| **P-C2** | dialog struct 化，**4 批**（每批独立 commit/revert）：a) NewTask + NewColumn + Rename；b) ConfirmDelete + ContextMenu；c) DetailDialog（含守卫与 datepicker）；d) OverdueDialog + toast 迁 `ui_kit::ToastState` | C1；d 依赖 **R1** |
| **P-C3** | 看板区抽 `todo_ui_board.cpp` | C2 |
| **P-C4** | 浮层区抽 `todo_ui_dialogs.cpp` | C2；建议在 C3 之后（两阶段都改 `compose` 的 draw 调用段） |

先 struct 化、后抽文件：抽文件只是搬动**已经内聚**的单元；反序会在成员仍散落时先造成跨文件裸成员访问。

### 4.4 每阶段验收

| 阶段 | 定向测试 | 结构断言 | 肉眼 |
|------|----------|----------|------|
| P-C1 | `todo_ui`(32) + `todo_service`(21) | `todo_module.cpp` 中 `boardGeom`/`chipWidth` 定义 → **0**（改为 `todo::ui::` 调用） | 否 |
| P-C2 a/b | `todo_ui` | `newTaskOpen_`/`newColumnOpen_`/`renameOpen_`/`confirmOpen_`/`ctxOpen_` 顶层成员 → **0** | 是：4 弹窗 + 右键菜单开合 |
| P-C2 c | `todo_ui` | `detailOpen_`/`detailTitle_`/`detailDiscardArmed_` 顶层成员 → **0** | 是：详情弹窗（含"改完点遮罩第一次只警告"）+ 到期日 datepicker |
| P-C2 d | `todo_ui` | `toastOpen_`/`toastTitle_` 顶层成员 → **0**；改调 `ui_kit` | 是：成功/失败两态 toast |
| P-C3/C4 | `todo_ui` + `view_state` | `todo_module.cpp` 中 `void drawBoard`/`void drawCard`/`void drawDetailDialog` → **0** | 是：拖拽跨列 + 6 弹窗 + 搜索/逾期视图 |
| 收口 | 全量全绿 | 成员区声明 ≤ **25**；`wc -l todo_module.cpp` ≤ **700** | — |

### 4.5 被否决的备选

| # | 备选 | 否决理由 |
|---|------|----------|
| 1 | 拆成两个 `core::Module`（看板模块 + 弹窗控制器） | Router 一个 `PageId` 一张页；拆模块 = 改装配/门控/托盘语义（改产品行为），且与 PAB-02 的接入成本问题对冲 |
| 2 | 泛型 `Dialog<T>` 模板基类统一 6 个弹窗 | CLAUDE.md §2.3 禁高级黑魔法（用户不熟 C++）；6 个弹窗字段各异，模板只省下 `bool open` 一行 |
| 3 | 把拖拽状态机独立成类/文件 | 风险最高、收益最低：`DragState` **已是**嵌套 struct（内聚已达标），且 tech-debt B 的同帧快照语义刚修好（R spec §3.4 B），再搬一次 = 白担一次回归 |
| 4 | 顺手把 6 个弹窗统一改用 `ui_kit::dialog_form`（R2 的产物） | R2 已单独排期；本文只做 struct 化。两件事叠加会让"尺寸/文案逐字不变"的验收面翻倍 |

---

## 5. 跨类问题

### 5.1 模块接入成本（PAB-02）

**现状：新增一个页面模块 = 9 处编辑 / 5 个文件**（含托盘入口再 +2）。证据：

| 位置 | 编辑点 |
|------|--------|
| `CMakeLists.txt` | `add_subdirectory`(:88-90 区)、`target_link_libraries`(:124-126 区)、`target_include_directories`(:136-138 区) |
| `src/main.cpp` | `extern "C"` 工厂声明(:40-44)、`static std::unique_ptr` (:139-143)、`ModuleSet{...}` 结构字面量(:153)、`trayNavAvailableFromConfig` switch(:331 区)、`executeTrayAction` switch(:345 区) |
| `src/app_assembly.h` | `ModuleSet` 定长 5 字段 —— **注册顺序 = 字段声明顺序，仅注释保证** |
| `src/app_assembly.cpp` | `assembleAndStart` 三段复制粘贴式 `if/else` 门控（home/settings 恒注册） |
| `tests/CMakeLists.txt` | include 目录(:65-66 区) + link 库(:74-76 区) |

**目标（并入 R8）**：`ModuleSet` 定长结构 → `std::vector<ModuleEntry{moduleId, Module*, alwaysOn}>`；三段门控 → **一行表遍历**（恒启用项 `moduleId` 留空）；注册顺序由调用方传入列表顺序决定（消除"字段顺序即注册顺序"的隐式不变量）。

**收益**：新增模块降到「1 行注册表 + 1 行 extern 工厂 + 2 处 CMake」，门控从 3 处复制粘贴变 1 处；`tests/test_app_assembly.cpp` 已有装配层用例（`:61 ModuleSet set()` 辅助 + `:358 ModuleSet{}` 字面量）→ **ModuleSet 类型变更需同 commit 更新测试**。

**否决**：① X-macro 单表（§2.3 黑魔法）；② CMake 目录 glob 自动发现模块（隐式构建图，违反"显式优于隐式"，且编译期无法发现漏注册）；③ 保留定长结构只加注释（现状即此，不解决）。

### 5.2 UI 与逻辑分离的统一手法（与 `ui_kit` 的关系）

**统一的是形状，不是库**——两者**不合并**：

| 层 | 收什么 | 依赖方向 |
|----|--------|----------|
| `src/ui_kit/`（R1 新建） | **跨模块**可复用的渲染单元：`toast` / `dialog_form` / `card` / `chip` / `metrics.h` | 只依赖 `core` + EUI-NEO 组件；**不得依赖任何 `module_*`** |
| `module_*/X_ui_*.cpp` | **模块领域**渲染：引用模块自己的 `Profile`/`Task`/`Column`/enum | 依赖本模块 service + `ui_kit` |

合并的代价：`ui_kit` 将持有各模块领域类型 → 违反 R2（模块互不依赖）/ R3（core 边界）的**同一精神**，且把"通用组件层"变成"上帝 UI 层"。统一的是**约定**：

1. 每块可视区域 = 一个自由函数（不用类，照 `md_*` 9 个单元的既有模板）；
2. 签名固定为 `(ui, screen/几何, tokens, 只读或引用状态)`，状态**显式入参**，不捕获 `this`；
3. 元素 id 由调用方传（保留测试可断言性）；
4. 不碰 `Ui`/不碰 DB 的纯逻辑住 `X_ui_logic.h`——**能进 logic 头的就进**（这条同时是"可单测"的判据）。

判据：一个函数如果必须 `#include "claude_service.h"` 才能画，它属模块层；如果只需要 `tokens` + 值类型，它属 `ui_kit`。

### 5.3 "God 类拆到什么程度为止"的判据（防过度设计）

| # | 判据 | 用法 |
|---|------|------|
| **J1** | **变更轴**：一条未来的需求（加字段 / 加页签 / 加弹窗 / 换协议）应只落 1~2 个文件 | 某文件在 **≥3 条**变更轴上都是必改点 → 继续拆；否则停 |
| **J2** | **状态所有权**：一组状态若有**独立的线程约定 / 生命周期 / 不变式** → 成类；纯 UI 瞬时字段 → 同文件顶部收成一个 **struct**，不成类 | `ProxyEventMailbox` 有独立线程约定 → 成类（但可与同写者-读者对的 `ProxyRuntime` 同文件）；`datePickOpen_` 一个 bool → 并入所属 dialog struct |
| **J3** | **停手线**：不建只有 1 个实现者的虚接口、不做 Pimpl/ABI 隔离、不建"将来可能用"的抽象 | 沿用 R spec §4.3 不选用表 |
| **J4** | **可机械验证**：每阶段必须写得出 grep/行数断言 | 写不出断言 = 这一步还没定义清楚，先别动手 |
| **J5** | **行数只作体检，不作目标**：`compose ≤120` / 模块 `.cpp ≤600` / 服务 `.cpp ≤400` 是**结果**不是 KPI | 为凑行数而拆 = 过度设计；`syncCaptureActiveProfile` 168 行不搬即此判据的应用 |

**明确的"到此为止"清单**：`core::Module` 接口不再加虚函数（R8 已拍板的 `shortName()` 除外）；`*_service.cpp` 数据层不拆（SQL 已收口、单测最厚）；hover/滚动/选中这类瞬时 UI 字段不建类；`markdown_module.cpp`（2847 行，最大单文件）按 R7 原范围走，**不因本文而加码**。

---

## 6. 总依赖图与并行

```
R0 → R1 → R2/R3 → { R4(+本文 B1–B5)  ‖  R5(+本文 C1–C4) } → R8(+PAB-02) → R10(CPX-01, A1–A4)
                                                                         └─ 可与 R4/R5 并行（文件零重叠）
```

| 组 | 说明 |
|----|------|
| R4 ‖ R5 | 分属 `module_claude_proxy` / `module_todo`；本文的增补**不改变各自可并行性** |
| R10 ‖ R4/R5 | 文件零重叠（`claude_service.*` vs `claude_module.cpp`）；**R4/R5 已合（2026-09-28 复核）**——CMakeLists 显式清单（:2-14）已含 `claude_ui_*`，R10 仅追加四对新文件，原"并发冲突面"消失 |
| R10 内部 | A1/A2/A3 同文件 → 串行；A4 依赖 A2 |
| R5 内部 | C2 四批可单独回退；d 批依赖 R1 的 `ui_kit::ToastState` |

---

## 7. 需用户拍板（原 3 条：Q1/Q2 仍开、Q3 已答；2026-09-28 新挂 Q4–Q6）

| # | 问题 | 选项 | 影响 |
|---|------|------|------|
| Q1 | **CPX-01 拆分后，门面是否保留 43 个转发签名**（§2.1 重测口径；原 33）？ | **A（推荐）**：保留 → 14 处调用点零改动，测试即护栏，每阶段可单独 revert | A 的代价：门面短期看是"转发嘴"（但含策略，见 §2.2）；B 一步到位，diff 放大到 14 个调用点，回退粒度变粗 |
| | | B：同步删除转发、调用方直持子对象（并入 R10） | |
| Q2 | **反向同步 `syncCaptureActiveProfile`（168 行，:1748→:1915）是否搬出？** | **A（推荐）**：不搬，只登记（§2.5 否决 4） | B 能进一步瘦门面，但跨 DB+Keychain+文件三写 + S-2「失败即中止切换」语义，行为等价性难自证 |
| | | B：搬进 `SettingsTakeOver` | |
| Q3 | **服务层（R10）与 UI 层（R4/R5）的先后** | **✅ 已答（2026-09-28 复核）**：R4/R5 已合 master（`claude_ui_list/detail/form/logs/state/logic` 6 文件已在 CMakeLists :3-8），"先用 UI 层验证手法再动服务层"按原 A 执行完毕，本问关闭 | 不再占拍板名额 |
| Q4 | **`runProfile`/`setTerminalLauncher`（h:321/326、cpp:1585/1591，2026-09-28 新增）归谁？** | **A（推荐）**：留门面——runProfile 编排「取 profile → 终端终态」，非 A1-A4 任一子对象，`terminalLauncher_` 注入槽随留 | B 把「profile-run 编排」拆进 ProxyRuntime（终端 ≠ 代理生命周期）或 ProfileRepository（取行 + 执行混在一起），边界反而糊 |
| | | B：归入某子对象（如 ProxyRuntime） | |
| Q5 | **`testConnection`（h:226、cpp:1242）归谁？** | **A（推荐）**：留门面——编排「`loadApiKey`（A2）→ 上游 HTTP 直连」，与 ProxyRuntime 的本地 server 生命周期无关 | B 让 ProxyRuntime 依赖 A2 的 `loadApiKey`，跨子对象耦合；归门面则保持「门面 = 编排 + 策略」判据（§5.3 J2） |
| | | B：归 ProxyRuntime（并入 P-A3） | |
| Q6 | **CPX-02 残项 `syncRestore` 改 `Result<void>`**（现 void h:415 区/cpp:1956，唯一调用 `stopProxy` cpp:1563；§2.6 原写"只登记不修"与 `2026-09-27-full-audit-consolidated.md` 行 152「R10 承接收尾」口径冲突） | **A（推荐）**：并入 P-A4 收尾——A4 整体搬迁接管/还原协议侧时顺带改签名，失败并入 `lastSyncWarnings_`，与 `2026-09-27-full-audit-consolidated.md` 行 152 口径一致 | B 维持 void = 两处文档继续冲突；C 另开里程碑 = 徒增一次触碰 `claude_service.cpp` |
| | | B：维持 void，仍只登记不修 | |

---

## 8. 待验证项（动手前先取证）

| # | 待验证 | 验证方法 |
|---|--------|----------|
| V1 | `claude_module` 41 成员的**完整**区域分组（本文按区域抽样确认，未逐项归类） | 重扫成员区，按 `// ----` 注释与命名前缀逐项归入 5 个 struct；有异议项留在模块层 |
| V2 | `ClaudeService` 公开方法计数口径（**2026-09-28 审计复核 = 43 公开成员 = 41 非静态 + 2 公开静态 `setGlobal`/`global`（h:374/375）**：含 1 个 inline 转发 `reapplyTakeOver`；**私有**静态 `sha256HexPrefix`/`makeApiKeyHint`（h:407/408）与 constexpr 不计入；TwoPhaseKeyWrite 类自身 4 方法不计。研究 agent 另计 ≈46，差异仅「是否计入新类/静态方法」，以本表 43 为开发口径） | `grep -nE '^    (core::Result\|void \|bool \|int \|...)' claude_service.h` 逐条按"职责归属"打标，确认无一组可独立成类 |
| V3 | 所有行号会漂移（审计期 `claude_module.cpp` 2577 → 今日 2612；`todo_module.cpp` 2432 → 2474） | 每个阶段开工前按**符号名**重定位，禁止沿用本文行号盲改（R spec §9 V7 同款要求） |
| V4 | `module_pet` 侧读 `takeProxyEvent` 的 app 级桥（`main.cpp` 装配）在 `ProxyEventMailbox` 迁移后是否需要同步改签名 | 读 `main.cpp` 的 pet 桥接线 + `pet_window.h` 契约注释；预期零改动（门面签名保留），需确认 |
| V5 | 结构断言里的字符串是否全部唯一（如 `boardGeom` 是否在别处也有同名） | 每条断言执行前先 `grep -c` 取基线值，写进该阶段 commit message 作前后对比 |
| V6 | `tests/test_app_assembly.cpp` 对 `ModuleSet` 的依赖面（PAB-02 改类型时的测试改动量） | 已见 `:61` 辅助 + `:358` 字面量两处；动手前跑一次该文件确认无其他隐式依赖 |

---

**Spec 版本**：1.1（2026-09-27 初版 1.0；**2026-09-28 刷新**：R10 段基线重测（cpp 2005 / h 469 / 公开 43 / master `753fc92`）、前置完成度更新（CPX-02 半修 / CPX-07 / CPX-14 / R4-R5 已合，Q3 已答）、补 8 项缺口（shared-config 归 A2、runProfile/setTerminalLauncher 与 testConnection 归属、settings_sync:: 6 处、CPX-02 口径、CMakeLists 补位）、新增 Q4–Q6 待拍板；**另：独立审计 10 项 required_change 已全合**——split-boundary 5（P-A2 段级拆分定义（编排尾巴 5 方法逐条划段）+ TwoPhaseKeyWrite 整类迁出 + cleanupOrphanKeychainEntries 入 A2、P-A3 stopProxy 段级拆分、P-A4 断言口径与 syncTakeOver 双入参、否决 4 读侧归属裁定 (b)）、baseline-accuracy 2（公开 43 = 41 非静态 + 2 公开静态口径、头文件行号 h:321/326/226 更正 4 处）、testability 1（同 split-boundary 根因）、minor 2（P-A1 :694 注释与消费方口径、`2026-09-27-full-audit-consolidated.md` 行 152 全文件名引用）；决策未翻转——Q1 门面保留 43 签名 / Q2 syncCaptureActiveProfile 留门面 / Q4 runProfile 留门面 / Q5 testConnection 留门面 / Q6 CPX-02 并入 A4 均保留【Q4/Q5/Q6 仍待用户拍板】；待主会话 review + 下一轮审计）
**上游依据**：`2026-09-27-refactor-incremental-design.md`（R0–R9 + 红线 R1–R7）、`2026-09-20-devdesk-design.md` §4.2/§4.2.1
