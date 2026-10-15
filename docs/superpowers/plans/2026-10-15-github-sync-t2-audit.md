# T2 独立审计 — `platform/github_api` Contents API 客户端（b13b212 + 05cc0bc）

- 审计对象：`worktree-agent-ghapi` @ `b13b212`（worktree `/Users/coderstory/dev/gui/DevDesk/.claude/worktrees/agent-ghapi`），前置 `05cc0bc`（sql_dump 补 `<set>` 头，纯头补齐零行为）
- 依据：plan T2 节 / spec §2（决策 #3/#4）+ §2.1 F9 / `git show b13b212`
- 审计者与实现者无关联；本审计除本文件外零写动作
- 复跑证据：worktree 内 `build/tests/devdesk_tests`（`git status` 干净；二进制 mtime 22:07 早于 commit 22:20，但源码无未提交改动）——实测 `github` 过滤 **27/27 全绿**、`http` **14/14**（http_client 5 + http_server 9）、`Result` **15/15**

## Findings

| # | 位置 | severity | 问题 | 影响 | 建议 |
|---|---|---|---|---|---|
| 1 | `src/platform/github_api.cpp:690-777`（testConnection ③）＋ `:656-668`（createRef） ｜ low ｜ createRef 的 POST 建分支在「响应丢失但服务端已受理」场景：重试 POST → 422 → createRef 失败 → testConnection 走 `created.isFail()` 分支**不尝试 deleteRef**，远端残留临时分支且 summary 无残留提示（探针文件也未写）。与「测完不留痕」承诺冲突 | 极小概率（网络错恰落在建分支响应上）残留孤儿分支，用户无提示 | createRef 对 422 且 body 含 "already exists" 视为成功；或 testConnection 失败路径也尝试 deleteRef 兜底 |
| 2 | `src/platform/github_api.cpp:386-389`（appendLog bodyTail） ｜ low（安全口径） ｜ `entry.bodyTail` 存响应体末 512 字节并经 `recentLog()` 进 UI 详情——spec 决策 #6 明确要求「响应体尾部」。但 T4 拉取 `data/database.sql`（raw）时 tail = dump 末尾 INSERT 片段；决策 #8 密钥列本就进 dump（macOS 明文），等于**其他凭据片段可出现在详情弹窗**。「token 永不入日志」只覆盖 PAT，不覆盖 dump 内容——spec #6 与 #8/安全段自身张力，非实现 bug | 详情弹窗/可复制日志可能含 DB 密钥列片段 | 对 `data/database.sql` 响应跳过 bodyTail（置 "***"），或 spec 决策层拍板接受并写明 |
| 3 | `src/platform/github_api.cpp:353-356` ＋ 重试语义 ｜ low ｜ PUT（无 sha 新建）在网络错后重试可能产生重复 commit（首次已受理、响应丢失）。带 sha 的更新第二次 409 不受影响（Conflict 不重试）。spec 决策 #4 明确要求重试连接失败，实现遵 spec；T4 按 F1 对更新一律带 sha 后，残余仅「新建文件重复 commit 污染历史」 | 历史噪音，非数据错误 | 无需改码；T4 编排知晓即可 |
| 4 | spec `2026-10-15-github-sync-api-design.md` 决策 #4 vs `github_api.cpp:101-104` ｜ low ｜ spec 字面「不重试 403」，实现为「限流态 403（`x-ratelimit-remaining: 0`）重试、普通 403 不重试」。实现更正确（GitHub 用 403+remaining:0 表限流），但 spec 未像 F2 那样回写 | 文档口径漂移：按 spec 复核会判为偏离 | spec 决策 #4 补一句回写，认可限流态分支 |
| 5 | `src/platform/github_api.cpp:274-284`（validateConfig）／`:322-325`（contentsUrl） ｜ low ｜ owner/repo 不做字符白名单也不编码；用户配置手误（如 owner 带空格）→ URL 畸形 → curl `CURLE_URL_MALFORMAT` 被 send 折成 networkError → 重试 3 次（1+3+9s）后才报「网络错误」 | 错误信息误导 + 无谓重试延迟 ~13s | validateConfig 对 owner/repo 加 `[A-Za-z0-9._-]` 白名单，非法归 ValidationError |
| 6 | `src/platform/github_api.cpp:521-553` vs `:580-616` ｜ info ｜ putFile 与 putFileOnBranch 除 `branch` 字段外逐行重复（~35 行）。865 行覆盖 8 端点+重试+编码+脱敏+URL 解析，行数/功能比整体合理，此为唯一明显复制 | 加一个响应处理改动要改两处 | putFile 委托 putFileOnBranch（branch 留空）可省 ~30 行；不阻断 |
| 7 | `src/platform/http_client.cpp:102`（既有行，非本票改动） ｜ info ｜ 网络失败路径 LOG_ERROR 打**全 URL**。github_api 的 URL 由构造保证不含 token（header-only），不受影响——但「token 不落日志」依赖的是这一构造约束，`github_api_log.h:14` 跨文件不变量成立 | 无 | 无需改；后续若有调用方把 token 拼进 URL 会直接漏，T4 集成时保持构造纪律 |
| 8 | tests `test_github_api.cpp` ｜ info ｜ 漏测分支：deleteFile 200 成功路径、maskUrl 的 scp 形态、Retry-After 非法值（HTTP-date → -1 → 默认退避）、createRef 网络错重试（finding 1 即此盲区） | 覆盖缺口均低危 | 修 finding 1 时补一条重试场景用例 |

未发现 medium 及以上问题。

## 重点核查结论

1. **http_client.cpp DELETE 改动面**：已核。改动最小（+1 条件、+3 行注释）；全仓 grep，platform 层 DELETE 调用方仅 `proxy_router.cpp:154/366` 代理透传与 github_api 自身。透传 DELETE 此前 body 被**静默丢弃**，现原样转发（空体则 `Content-Length: 0`）——对 Anthropic 上游是修复而非回归；`CURLOPT_CUSTOMREQUEST`（:63）先于 POSTFIELDS 设置且优先级更高，方法串不被 POSTFIELDS 改写。定向 `http` 5+`server` 9 复跑绿。
2. **maskUrl 三形态**：已核。前缀表 `github_pat_/ghp_/gho_/ghu_/ghs_/ghr_` 完备（GitHub 全部现行 token 形态）；userinfo 掩码有 `pathStart` 误判防护；敏感查询键 15 个；token 只进 `Authorization` 头（`doRequest:349`），`appendLog` 只存 `maskUrl(url)` 与响应体尾——**日志写入点证毕**，url 无 token 是构造保证（finding 7）。`findHeaderCI` 大小写不敏感，`retry-after`/`x-ratelimit-remaining` 查找可靠。
3. **重试语义**：已核。分界正确——`http::send` 把全部 CURLcode 折成 networkError（含 URL 畸形，见 finding 5），github 层 networkError 一律重试等价于按 CURLcode 分诊；5xx/429 恒重试、403 仅限流态、401/404/409/422 不重试；上限 3 次（policy.attempts）、退避 1/3/9s、Retry-After 上限 60s（long 运算无溢出）。非幂等重放风险 = finding 3（spec 授权，残余可接受）。
4. **>1MB raw 二跳**：已核。GitHub 对 1-100MB 文件 JSON 响应 `encoding="none"`/`content=""`，先取元数据（sha）再 raw 直取——与 GitHub 行为一致。内存峰值 ≈ 2× 文件（body + 拷贝），PUT 侧 base64+JSON 又 ~2.4×；DevDesk dump 排除 call_logs 后典型 <10MB、同步在后台线程——量级可接受，无流式必要（F9 不做分块）。
5. **validateConfig / URL 编码**：已核。路径保留 `/`、branch 值全转义（`feature%2Fx` 用例 :591/:652 实证）；中文/空格路径 UTF-8 逐字节转义用例 :650 实证（`%E6%88%91...%20...`）。**`%2F` 在 GitHub refs 端点的真实行为未经真网验证**（零真网设计的固有边界），待 T5/真机验收覆盖。owner/repo 不校验 = finding 5。
6. **result.h Conflict**：已核。追加在枚举**末尾**（Internal 后无后继值），零数值位移；`errorCodeToString` switch 全覆盖 + "Unknown" 兜底；全仓 grep 无 ErrorCode 数值序列化/反序列化（无 cast、无持久化、无 IPC 编码），迁移/日志零影响；测试 :658 锁字符串。
7. **测试有效性**：已核。27 例断言全部内容/计数级（请求数、退避毫秒序列、URL 片段、base64 逐字），无恒真；Recorder 假发送器 + SleepGuard 复位静态 sleep 注入，「零真网」声明属实（`setSender` 为实例成员无需复位，`setSleepFunction` 为静态、SleepGuard RAII 复位正确）。漏测分支见 finding 8；「先红后绿」变异证据未做（同 T1 审计口径，断言非恒真可静态推得，不阻断）。
8. **行数/功能比**：合理。8 文件 +1645 行 = 实现 976（.h 88 + .cpp 865 + log.h 20 + result/CMake 各 1-3）+ 测试 662；无超需求实现（未做 spec §9 排除项），唯一复制 = finding 6。

## 六维度结论

1. **正确性**：已核。spec T2 项逐条对上——createRef/deleteRef、重试表（含 Retry-After 上限与限流 403）、raw 二跳、maskUrl 三形态、409→Conflict、testConnection 三步 + 删除失败残留提示（:773-776）。404 getFile 返回 `exists=false` 不当错误、createRef 基点 404 短路不发 POST，均有用例钉死。两处小偏差（finding 1/4）。
2. **回归面**：已核。DELETE 共享层改动最小、唯一既有调用方（代理透传）行为为修复非回归；http/server/json/Result/keychain 定向复跑全绿；CMakeLists 各加一行。
3. **平台对称性**：已核。纯 libcurl/OpenSSL EVP/std，无平台分支、无新增编译期依赖；Windows 仅到 CI 编译级（本批已知边界，spec §8 如实标注）。
4. **静默失败面**：已核。错误路径均带错误码与中文消息；本地校验失败「零请求零日志」是有意契约且有测试钉死（:612）；低危静默点 = finding 1（建分支已受理后的残留）。
5. **测试有效性**：已核（复跑 27/27 绿）。见上第 7 条。
6. **文档口径**：已核（除 finding 4）。commit message 与实现逐条一致（含「网络错折叠等价分诊」的推理）；`github_api.h` 契约注释（注入点复位义务、putFileOnBranch 存在理由、409 语义）与实现一致。

## 总裁决

**放行合并**。零 medium+ findings；finding 1（残留分支边缘路径）建议 T4 集成时顺手修或回写 spec 决策 #4（finding 4）时一并处理，其余 low/info 不阻断。
