# 审计报告：worktree-agent-ghsync-live（e8d0dc9，基 589d8f5，17 文件 +1328/−70）

审计树：/Users/coderstory/dev/gui/DevDesk/.claude/worktrees/agent-ghsync-live（HEAD e8d0dc9，工作区净）。只读 + 离线单测；未跑真机 live。

## A. 四修正确性
1. **makeLocalNote 收口** ✅。全仓聚合初始化仅 2 处（settings_module.cpp:1084-1086、1318-1320）均已收口；grep 无 `LocalNote{` 残留（仅注释）。hash 对移动前内容算。单测（test_github_sync_logic.cpp:65-76）是强变异判据：变异回聚合初始化 → hash 退化为 cbf29ce484222325 → 两条断言必红。弱点（info）：单测只护 helper，护不住调用点再犯——靠「唯一入口」约定兜底。
2. **branch 参数链** ✅。github_api.h:52-58 加参（空=默认分支）；全仓调用点恰 3 处（settings_module.cpp:1157/1197/1240）全显式传；putFileOnBranch 委托 putFile（testConnection 临时分支路径语义不变）。🟡低：spec `docs/superpowers/specs/2026-10-15-github-sync-api-design.md:97-101` 的 putFile/deleteFile 签名无 branch 形参，与实现不符（按规则只记不改 spec）。
3. **exists 位** ✅。platform 层 getFile 404→ok(exists=false) 基线即如此，修复全在调用方：FirstSync missing 改 exists 位（:961）、branchMissing 改 listTree 404 + 建分支后重取树闭环（:1053-1063）、database.sql sha 改同树取（:1150-1156）。全部 3 个 getFile 调用点（:956/:1358/:1411）逐一核过，均按 exists 语义处理，无漏改。
4. **RemoteModified→Put** ✅。pushActionForState(github_sync_logic.cpp:154-159)；单测钉死（test:259-261）。推演：Put 成功→登记新 sha+hash→下轮 InSync，无推拉死循环；与 pull KeepLocal（远端已删本地保留）最终语义一致、少一轮空转。场景 8 真机覆盖（自述 15/15）。ℹinfo：该路径 PUT 带过期登记 sha（settings_module.cpp:1190-1196），依赖「GitHub 对分支上不存在的文件按创建处理、不校验过期 sha」的外部行为——仅真机证据，无离线覆盖；与 :1400 KeepLocal 注释「失效 sha PUT→409/422」假设不同但可并存（文件在场 vs 不在）。

## B. 基建卫生
5. **测试缝** ✅。双变量门正确（仅 MODE=1 认 TOKEN；门关/无 TOKEN 均走真钥匙串，生产零行为变化）；缝函数生产路径零调用。DEVDESK_BUILD_LIVE_TOOLS 源码默认 OFF，devdesk_tests 排除 live/（tests/CMakeLists.txt:142-146），ctest 条目 LABELS "live" 不含 probe（实测 `ctest -N -L probe` 含 github_sync_live = 0）。ℹinfo：本 worktree 的 CMakeCache 被实施者留成 ON（build/tests/github_sync_live 在），属本地产物非源码缺陷。
6. **live 脚本** 基本 ✅：token 不落文件/日志/驱动 argv；临时响应体 mktemp+trap 清理；-destroy 一次性语义清晰（reset/ensure/delete-destroy 分立，删后场景 10 自动重建）；退出码 0/1/2 明确。🟡低：ghsync-live-fixture.sh:57 curl `-H "Authorization: Bearer ${TOKEN}"` 把 token 放进 curl 进程 argv（本机 ps 可见窗口），与脚本头「绝不进 argv」自述不符。
7. **settings 侧** ✅最小无夹带：config seam +27 行、probe 改名+inline wrapper（生产签名不变）、harvest 搬运 conflict/dbImported 为缝所需且保留原 if(exchange) 语义（:1593-1603）。
8. **全 diff** 无调试残留、无密钥；注释多为反重构警告/契约，合纪律；行数/功能比合理（608 live 驱动 + 346 脚本是票面交付物）。

## C. 实跑（离线）
- 全量 `--no-run`：**Total 1434 / Passed 1428 / Failed 0**（差 6=平台 SKIP；test_claude_service.cpp:2991 本轮未复现，与基线口径不冲突）；UI probe 全绿；**脚本整体 exit 1**（见下）。
- `--filter github_sync`：**29/29/0**；makeLocalNote 用例单独 1/1 PASS。`--filter gsync`：**0 命中**（用例名前缀是 `github_sync`，"gsync" 不匹配；二进制对零命中诚实 exit 2）——派单口径词不准（info）。

## 必修（一行级，不触及修复逻辑）
- 🔴高 `tests/CMakeLists.txt:354-378` + `tests/live/github_sync_live.cpp:38,100-109`：驱动用 `sys/wait.h`/`popen`/`bash`（纯 POSIX），但 add_executable 未按 UNIX 门控（只有 add_test 有；还写了 MSVC/MINGW 链接选项，自相矛盾）。受编集合解析器（check-windows-cross-compile.sh:224-229）视 `if(DEVDESK_BUILD_LIVE_TOOLS)` 为 Windows 分支 → 该文件**恒**进 Windows 受编集合，与开关取值无关。实测本机 llvm-mingw 守卫红；Windows CI 原生 GCC 同样编不过 → 合进 dev 即门禁红。修法：add_executable 挪进 `if(UNIX)`。
- 🟠中 `github_sync_logic.cpp:43`（std::move 缺 `<utility>`）、`tests/live/github_sync_live.cpp:127`（std::error_code 缺 `<system_error>`）——check-std-includes.sh 实测红 2 条；且 build-test-run.sh 在 3b-5 就 exit 1，3b-6 跑不到，全量门禁在本分支必红。
- 验收备注「全量 Total 1434/Failed 1」只跑了 doctest、未跑门禁脚本，两道静态守卫的红未报告（memory「合并验收必须跑两条门禁」的坑型复现）。

## 裁决
**放行带必修** —— 三处一行修复（CMake UNIX 门控 + 两个标准头 include）后重跑 build-test-run.sh 全量应 exit 0；四修与基建主体正确。
