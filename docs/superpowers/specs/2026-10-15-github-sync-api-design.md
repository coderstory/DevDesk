# GitHub Sync（API 版）— 设计与决策

> 日期：2026-10-15 ｜ 分支基线：`dev` @ `de456cb`
> 前身：`2026-10-15-github-sync.md`（PRD，git CLI 路线）+ `2026-10-15-github-sync-handoff.md`（交付说明）
> 本文件**取代**前两份的「调外部 git」部分：同步不再依赖 git 可执行文件。

## 1. 目标

用户在设置页配置 GitHub 私有仓库 + PAT 后，DevDesk 把**笔记（`notes/*.md`）与全库 SQL dump** 同步到该仓库，实现多设备数据同步。

**零外部依赖**：不需要用户安装 git / gh / MinGit。传输层复用仓库内已有的 libcurl（`src/platform/http_client.*`）与 OpenSSL。

**非目标**：GitLab / Gitee / 自建 git 远程；实时协同；冲突自动合并（三栏 IDEA 风格弹窗不在本批，见 §9）。

## 2. 决策表（拷问产出 20 条；2026-10-16 真机验收回写续 #21–#23）

| # | 决策 | 说明 |
|---|---|---|
| 1 | 零外部 git | 不检测 git、不内置 gh/git；同步走 GitHub Contents API |
| 2 | 仅支持 GitHub | 用 `api.github.com`；其他托管平台不在范围 |
| 3 | 测试连接 | `GET /user` → `GET /repos/{o}/{r}`（含私有性）→ 建临时分支 `devdesk-test-<epoch>` + 写探针文件 → 删分支；测完不留痕 |
| 4 | 重试 | 3 次，退避 1s/3s/9s。重试对象：连接失败/超时/断开/DNS 解析/TLS 握手失败 + 5xx + 429（读 `Retry-After`，上限 60s）。不重试：401/404/409/422 与本地错误（无 token、URL 非法、文件读写失败）。**403 分态（2026-10-15 T2 实现修订）**：`x-ratelimit-remaining: 0` 的限流态 403 按 429 同规则重试（带 `Retry-After`）；其余 403（真无权限）不重试 |
| 5 | 失败弹窗动作 | 重试 / 详情 / 关闭 |
| 6 | 详情内容 | 每次尝试一条请求记录：方法 + URL（token 脱敏）+ 状态码 + 耗时 + 响应体尾部；可复制。**已知张力（2026-10-15 用户拍板 B，接受现状）**：拉取 `database.sql` 时响应体尾部 = dump 末尾 INSERT 片段，密钥列（macOS 明文列）可进详情弹窗——决策 #8 的 maskUrl 管 URL 脱敏，不管 body；用户接受该暴露面，真机验收不复议 |
| 7 | 409 冲突 | 独立弹窗（文案不同）：拉取远端后再推（**二次确认**：将用远端覆盖本地、已自动备份）/ 详情 / 关闭 |
| 8 | 同步范围 | `notes/*.md` + 全库 dump（含 `claude_profiles`，**含密钥列**） |
| 9 | 旧 git 遗留 | 删 `src/core/git_sync_service.{h,cpp}`；`paths::sqlDumpFile()` 保留但改落 appData；用户机器上既有 `~/DevDesk/.git` 不自动删，仅在日志/提示中说明 |
| 10 | 同步分支 | 用户可配置（默认 `main`）；分支不存在 → 以仓库默认分支为基点**自动创建**，卡片提示已建 |
| 11 | 拉取备份 | 覆盖本地库前备份 `backups/devdesk-<ts>.db`，保留最近 **5** 份；备份目录不进同步 |
| 12 | 自动方向 | 默认只自动 push；pull 只走设置页按钮（409 弹窗的拉取动作除外，见 #7） |
| 13 | 仓库可见性 | 测试连接检查是否私有；public 仓黄字提醒（笔记/待办/密钥将公开可见），不硬拦 |
| 14 | commit 节流 | **5 分钟内最多一次** commit；commit 后紧跟 push（两者联动，不独立） |
| 15 | 本地 git 可选 | 探测到可用 git → best-effort 做本地版本历史（init + 每次 commit）；不可用不影响同步。**无 git 时在用户点「配置」按钮时弹一次警告**，之后仅在修改配置时再弹 |
| 16 | 首次启用 | 远端有该分支 → 备份本地库后拉取覆盖；远端没有 → 用本地数据创建分支并首次上传 |
| 17 | 手动同步 | 点「立即同步」**绕开** 5 分钟节流 |
| 18 | 远端布局 | `notes/*.md` + `data/database.sql` |
| 19 | 删除传播 | 本地删除笔记 → 同步时 `DELETE` 远端同路径；重命名 = 删旧 + 建新 |
| 20 | dirty 触发源 | 仅**笔记写入/删除 + 待办增删改**标记 dirty；其他配置变更不主动触发，但随下次 dump 一并同步 |
| 21 | 推送分支定向（2026-10-16 真机回写） | `putFile`/`deleteFile` 签名增 `branch` 形参：推送三路（database.sql PUT / 笔记 PUT / 删除 DELETE）全传配置分支——修「配置非默认分支时读 A 写 B、写全落仓库默认分支」。`branch` 空串 = 写仓库默认分支（**软契约**：API 不校验，调用方负责显式传配置分支；`deleteFile` 的 `branch` 为必传位置参数，无默认值）。出处：真机矩阵票分支定向写修复（`worktree-agent-ghsync-live` `e8d0dc9`）+ branchfix 票 `ebf815c` |
| 22 | getFile 404 语义（2026-10-16 真机回写） | platform 层 `getFile` 对 404 返回 `ok(exists=false)` 而**非 NotFound 错误**（§4 状态码映射的 404→NotFound 对 `getFile` 不适用）；FirstSync 远端 missing / branchMissing 判定改用 `exists` 位与 `listTree` 404。空仓首次启用、同步分支不存在时自动建分支依赖此语义（原 404→NotFound 判定为死代码：空仓首启恒失败、自动建分支从不触发）。出处：真机矩阵票修复 3 |
| 23 | 推进状态机 RemoteModified 分支（2026-10-16 真机回写） | `pushActionForState`：RemoteModified 且远端文件不存在（`remoteExists=false`）→ **Put**（原 Skip 会使分支被删重建后笔记永不回传；与 pull「远端已删→本地保留」构成双向最终一致）。出处：真机矩阵票修复 4 |

## 2.1 审计补正（实现前必读，2026-10-15 审计）

审计对代码实证后发现 9 处缺口，处置如下。**F1–F4 是实现前必须先解决的硬伤**，其余为实现约束。

| # | 问题 | 处置 |
|---|---|---|
| F1 | **变更检测机制未定义**：如何判定某笔记是新增/修改/删除 | 维护本地同步状态：`app_config` 键 `github_sync:sha:<相对路径>` 存「上次同步成功后的远端 sha」，另存 `github_sync:sha:data/database.sql`。判定：本地有而状态无 → 新增；本地内容 hash 与状态记录不符 → 修改；状态有而本地无 → 删除（→ DELETE 远端）。**不用 mtime**（跨设备/备份恢复后不可靠） |
| F2 | **`sql_dump` 不导索引/触发器**（`sql_dump.cpp` 只遍历 `listUserTables`）：导入 = DROP+重建，索引/触发器丢失；且 dump 里的 `schema_version` 与本地一致 → 迁移不会重建 → schema 静默退化 | 导出补 `sqlite_master` 中 `type='index'`（排除 `sqlite_autoindex_%`）与 `type='trigger'` 的 DDL；段序改为：① 建表 DDL ② 索引 DDL ③ INSERT ④ 触发器 DDL。**触发器必须放在 INSERT 之后（2026-10-15 T1 实测修订，偏离本表最初写法）**：若先建 AFTER INSERT 触发器，回导 INSERT 会点火触发器，重建出的数据 = 原始数据 + 点火产物，还原悄悄多数据；回环测试已钉死该顺序。索引/触发器均带 `DROP ... IF EXISTS` 保证幂等。**本项属 `core/sql_dump` 修复，作为独立 ticket 先做** |
| F3 | **导入覆盖活库**：DROP TABLE 时其他模块持有缓存/prepared statement | 导入前：暂停后台任务（余量刷新、同步自身）；导入后：发一次全局数据重载信号（`requestUiUpdate()` + 模块 `onPageEnter` 语义的强制重载），并重新执行 `PRAGMA foreign_keys` 与索引校验。导入失败 → 从备份回滚并明确报错（不静默） |
| F4 | **skip 表清单未定**：`exportToString` 默认导全部表 → `claude_call_logs`（日志量大）会进 dump 并上传 | 固定 `skipTables = {"claude_call_logs", "claude_run_settings"}`（与 handoff 一致），单测锁死。**落位修订（2026-10-15，T1 落地）：常量落 `core::sql_dump::defaultSkipTables()`**，不放在 `github_sync_logic`——core 层单点、备份与同步两路共用，T4 编排引用该函数，不得另抄清单 |
| F5 | **退出时 dirty 未推** | 退出序列里：若 dirty 且距上次 commit ≥ 60s，后台尝试推一次（上限 5s 等待，超时则放弃并记日志 + 下次启动卡片提示「有未同步改动」）。不阻塞退出 |
| F6 | **409 拉取覆盖会丢本地未推改动**（备份只保库，不保笔记） | 该动作的二次确认框必须**列出将被覆盖的本地未推文件清单**；确认后：库走 `backups/`，**笔记也先整目录备份到 `backups/notes-<ts>/`**（同样保留 5 份） |
| F7 | 远端列举端点未写 | 拉取时用 `GET /repos/{o}/{r}/git/trees/{branch}?recursive=1` 拿全树，筛 `notes/**.md` 与 `data/database.sql`；大仓分页 `truncated=true` 时按需再取子树 |
| F8 | 本地 git 探测与「零检测」的边界 | 探测**只服务于本地版本历史**，不参与同步判定。macOS 必须先 `xcode-select -p` 判断 CLT 是否安装（未装则视作不可用，**不得**直接调 `/usr/bin/git` —— 会弹系统 GUI 提示）；Windows 探 PATH + `%ProgramFiles%\Git\cmd\git.exe` + `%LOCALAPPDATA%\Programs\Git\cmd\git.exe`。探测结果缓存，配置变更时失效 |
| F9 | 大 dump 的请求体积 | base64 使体积 ×1.37。PUT 单请求上限 100MB（≈ 原始 73MB）。超限 → 明确报错「数据库导出过大，暂不支持同步」+ 详情；不做分块。GET 侧 >1MB 走 `Accept: application/vnd.github.raw` |

## 3. 架构

```
设置页「同步」tab（github_sync_card）
  ├─ 启用开关 / 仓库 URL / 分支 / Token（keychain）
  ├─ [测试连接] [立即同步] [拉取]   ← 运行中三键禁用
  └─ 进度行（阶段文案）/ 错误行（红字 + 详情入口）
        │  SettingsCardActions
        ▼
SettingsModule（settings_module.cpp）
  ├─ GithubSyncStore（app_config 持久化，`github_sync:*`）
  ├─ 同步编排：dump → 变更检测 → PUT/DELETE → 节流 → 失败弹窗
  ├─ 本地历史（可选）：探测 git → best-effort commit
  └─ 后台线程 + 每帧 compose 收割（照 QuotaRefresher / quotaTestThread_ 模式）
        ▼
platform::github::GithubApi（新增，libcurl + OpenSSL）
  ├─ getRepo / getAuthenticatedUser / getFile / putFile / deleteFile
  ├─ createRef / deleteRef（测试连接用）
  ├─ testConnection（三步 + onStep 回调）
  ├─ 重试包装（RetryPolicy）
  └─ 请求日志环形缓冲（上限 50，token 永不入内）
```

**分层约束**：`github_api` 放 `src/platform/`（`core` 不得依赖 `platform`）。`git_error_dialog` 只依赖注入的日志函数与结构体，不直接依赖 platform 实现。

## 4. 关键接口

```cpp
// src/platform/github_api.h（platform 层）
namespace platform::github {
struct RepoInfo  { std::string fullName, defaultBranch; bool canPush = false; bool isPrivate = false; };
struct FileInfo  { bool exists = false; std::string content, sha; };
struct TestStep  { std::string name; bool ok = false; int durationMs = 0; std::string detail; };
struct TestReport{ bool ok = false; std::vector<TestStep> steps; std::string summary; };
struct RetryPolicy { int attempts = 4; std::vector<int> backoffMs{1000, 3000, 9000}; };  // attempts = 总请求数：首次 + 3 次重试（与决策 #4 对齐，2026-10-16 T5 review 修订；原写 3 会让第 3 档退避永不使用）

class GithubApi {
public:
    GithubApi(std::string token, std::string owner, std::string repo, RetryPolicy policy = {});
    core::Result<RepoInfo>    getRepo();
    core::Result<std::string> getAuthenticatedUser();
    core::Result<FileInfo>    getFile(const std::string& path, const std::string& ref = "");
    // putFile/deleteFile 增 branch 形参（2026-10-16 真机回写，决策 #21）：空串 = 仓库默认分支（软契约）；
    // deleteFile 的 branch 必传；推送三路（database.sql / 笔记 / 删除）全传配置分支。
    core::Result<std::string> putFile(const std::string& path, const std::string& content,
                                      const std::string& message, const std::string& shaIfMatch = "",
                                      const std::string& branch = "");
    core::Result<void>        deleteFile(const std::string& path, const std::string& message,
                                         const std::string& shaIfMatch, const std::string& branch);
    core::Result<void>        createRef(const std::string& branch, const std::string& fromBranch);
    core::Result<void>        deleteRef(const std::string& branch);
    TestReport                testConnection(std::function<void(const TestStep&)> onStep = {});
    std::vector<ApiLogEntry>  recentLog() const;                     // 环形，上限 50
    static std::pair<std::string,std::string> parseRepoUrl(const std::string& url);
    static std::string maskUrl(const std::string& url);
};
}
```

- 认证：`Authorization: Bearer <token>` + `Accept: application/vnd.github+json` + `X-GitHub-Api-Version: 2022-11-28`。token 只进 header。
- 内容编码：base64（OpenSSL `EVP_EncodeBlock` / `EVP_DecodeBlock`）；`getFile` 响应 >1MB 时改 `Accept: application/vnd.github.raw` 直取。
- 状态码映射：401→认证失败；403→无权限（`x-ratelimit-remaining: 0` 视为限流）；404→NotFound（**修订（2026-10-16 真机回写，决策 #22）**：`getFile` 例外——404 返回 `ok(exists=false)`，不报错）；**409→新增 `ErrorCode::Conflict`**（远端 sha 与 `shaIfMatch` 不符）；422→ValidationError。
- 可测性：发送器（`std::function<core::Result<platform::http::Response>(const platform::http::Request&)>`）与 sleep 均可注入，默认走真实实现。

## 5. 数据流

**自动同步（默认方向）**
```
笔记写入/删除 或 待办增删改  →  置 dirty
   → 每帧 compose 检查：dirty 且距上次 commit ≥ 5 分钟
   → 后台线程：
       sql_dump::exportToFile(db, sqlDumpFile())          // 全库 dump
       notes/ 与 database.sql 的变更集（新增/修改/删除）
       逐个 PUT / DELETE（单文件粒度 = 单次 commit）
       （本地 git 可用时）best-effort 本地 commit
   → 成功：清 dirty、刷新卡片状态
   → 失败：重试 3 次 → 仍失败：卡片红字 + 详情入口（不弹窗打断）
```

**手动同步**：同上，但绕过节流窗口。

**拉取（手动按钮 / 409 弹窗动作）**
```
（F6）列出将被覆盖的本地未推文件 → 用户确认
   → 备份：库 → backups/devdesk-<ts>.db；笔记整目录 → backups/notes-<ts>/（各保留 5 份）
   → GET /git/trees/{branch}?recursive=1 取全树（F7）
   → GET data/database.sql（远端）→ 比对登记 sha：未变即跳过导入（2026-10-16 T4 实现修订，省一次全库重建）；变了才 sql_dump 导入覆盖本地库（F3：导入前后暂停/重载）
   → GET notes/*.md → 与本地按 sha 状态比对（F1）：新增/更新覆盖、远端已删的本地保留
   → 刷新 UI（模块数据重载）
```

**登记键是设备本地状态，永不跨设备（2026-10-16 T4 审计 C1 裁决）**
F1 的 `github_sync:sha:*` / `hash:*` 登记描述的是「本机与远端的一致性基线」，必须与本机绑定：不得随 dump 导出（实现：登记移入独立表并加入 `defaultSkipTables()`，或导出路径按前缀剔除；两者择一，以测试钉死「导出不含登记键」）。跨设备传播登记会让新设备首拉误判「笔记未变」不下载，继而下轮 push 见「有登记无本地」而删远端笔记（可清空远端）。

**首次启用（初始化）**
```
保存配置 → 校验（测试连接三步）
   → 远端有该分支：备份后拉取覆盖本地
   → 远端无该分支：以仓库默认分支为基点创建 → 上传本地全量
   → 无 git：点「配置」时弹一次警告（同步仍可用，仅无本地版本历史）
```

## 6. 错误处理与可见性

| 场景 | 表现 |
|---|---|
| 自动同步失败（重试耗尽） | 卡片红字摘要 + 「详情」按钮；**不弹窗**（避免打字时打断） |
| 手动操作失败 | 弹窗：重试 / 详情 / 关闭 |
| 409 冲突 | 弹窗：拉取远端后再推（二次确认）/ 详情 / 关闭 |
| 认证失败（401） | 弹窗文案指向「请检查/更新 PAT」 |
| 无 git 可用 | 点「配置」按钮时弹一次；改配置时再弹；不阻塞同步 |
| 本地历史 commit 失败 | 记日志；卡片灰字「本地版本历史：不可用」 |

**安全**：token 只存 keychain（`DevDesk/github` / `token`）与请求 header；日志、详情弹窗、dump 中一律脱敏。密钥列按决策 #8 进入同步 —— 已知代价：密钥进 git 历史、跨平台（macOS 明文 ↔ Windows DPAPI）不可用需重填。

## 7. 文件面

| 文件 | 动作 |
|---|---|
| `src/platform/github_api.h/.cpp` | 新增（客户端 + 重试 + 环形日志 + testConnection） |
| `src/platform/github_api_log.h` | 新增（日志条目结构） |
| `src/core/git_sync_service.h/.cpp` | **删除**（零引用） |
| `src/core/result.h` | 追加 `ErrorCode::Conflict` + `errorCodeToString` |
| `src/core/paths.h/.cpp` | `sqlDumpFile()` 改落 appData；`gitRepoDir()` 若无人用则删 |
| `src/module_settings/github_sync_card.h/.cpp` | 改：测试连接按钮 / 进度行 / 错误行 / 详情入口 / 三键禁用 |
| `src/module_settings/git_error_dialog.h/.cpp` | 新增（错误详情 + 409 变体） |
| `src/module_settings/settings_module.cpp` | 改：占位 → 真同步编排 + 节流 + 备份 + 首次方向 + 删除传播 + git 可选探测 |
| `src/platform/CMakeLists.txt`、`src/module_settings/CMakeLists.txt` | 注册新文件 / 移除删除项 |

## 8. 测试

- `tests/test_platform/test_github_api.cpp`：重试分类与次数（计数假发送器）、退避序列、base64 往返、`maskUrl` 脱敏、409→Conflict、环形日志上限 50、`parseRepoUrl` 变体、testConnection 三步（建 ref → PUT → 删 ref）与删 ref 失败时的 summary 提示。**零真网**。
- `tests/test_core/test_sql_dump.cpp`（F2）：导出含索引/触发器 DDL、`sqlite_autoindex_%` 被排除、导入后索引与触发器存在（回环断言：导出→导入→查 `sqlite_master` 比对）、skip 表不进 dump（F4 常量锁死）。
- 变更检测（F1）：状态机单测 —— 新增/修改/删除三态判定、sha 记录写入与读取、状态缺失时按新增处理、笔记改名 = 删+建。
- 备份策略（F6）：保留 5 份、超额删最旧、笔记目录备份与库备份各自独立计数。
- `tests/test_modules/test_github_sync_card.cpp`：无头 compose —— 三按钮存在/禁用态、进度行、错误行 + 详情入口、弹窗内容（注入日志）。
- 编排层：节流窗口（假时钟）、首次方向分支、删除传播、备份保留 5 份（临时目录断言）。
- 边界：Windows 侧仅到交叉编译守卫 + CI 编译级；运行时未验（无 Windows 机器），如实标注。

## 9. 不在本批

- 冲突三栏合并弹窗（IDEA 风格，左/中/右）—— 本批 409 走简化弹窗
- 非 GitHub 托管（GitLab / Gitee / 自建）
- 增量/二进制 diff、历史版本浏览 UI
- 多设备实时协同

## 10. 风险

| 风险 | 处置 |
|---|---|
| 密钥进 git 历史（决策 #8） | 已明确告知；详情/日志脱敏；不主动上传到 public 仓（黄字提醒） |
| 首次拉取覆盖本地丢数据 | 强制备份（5 份）+ 卡片/弹窗明示 |
| 大库 dump 体积（>1MB 需 raw） | getFile 走 raw；PUT 分块无需求（Contents API 上限 100MB） |
| 节流 5 分钟导致数据晚到 | 手动「立即同步」绕开 |
| 无 git 环境下本地无版本历史 | 备份文件兜底；警告弹窗告知 |
