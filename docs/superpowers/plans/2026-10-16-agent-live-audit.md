# 审计：worktree-agent-live（80c070c，基 dev d6845ad）— 2026-10-16

## A. Scheduler 修（notify_one→notify_all）
- ①唤醒点全核：scheduler.cpp 3 处 notify（:69 enqueueResident / :77 stopAll / :119 onEvent）全 notify_all，无残留 notify_one；两等待点（:171 常驻 / :197 事件）均谓词循环含 stopFlag_||!queue.empty()，假唤醒防护齐；tick 线程轮询不用 cv。✅
- ②谓词-等待者对应逐条核 ✅；:77 注释只写"常驻线程"实唤醒两者，措辞瑕（info）。
- ③变异实测：onEvent 改回 notify_one，离线复现（双等待线程+空 store，20 轮 onEvent 看抽干）：修复 LOST=0/20 → 变异 LOST=10/20（~50%/轮吻合）→ 还原 LOST=0/20；git status 干净、HEAD=80c070c。✅
- ④惊群：等待者=2，代价可忽略（info）。
- ⑤最小性：diff 仅 2 处+注释 ✅。缺口：事件路径单测全直调 drainEventQueue 不起事件线程，共享 cv 丢唤醒离线零守卫（live D 不进门禁且烧 LLM）→ **必修**：补离线单测（start() 后 onEvent×20 轮、有界轮询 eventQueueSize()==0；变异态 ~99.9% 红、修复态恒绿，零 LLM，已实测可行）。

## B. live 驱动卫生
- ⑥CMake：agent_live 整段 `if(DEVDESK_BUILD_LIVE_TOOLS && UNIX)`（tests/CMakeLists.txt:351）；live/ 剔出 devdesk_tests glob（:144）；LABELS "live" 无 probe；门禁脚本零引用；驱动内再查 DEVDESK_AGENT_LIVE=1（agent_live.cpp:748）✅。info：该开关未 option() 声明（仅 -D 可达）；check-probe-gate-labels.sh 只守 quota_live_probe 不守 agent_live（默认 OFF 时 add_test 不存在，风险低）。
- ⑦凭据：真库只读副本取 key 后即删（:655-737）；key 只进内存/Authorization；捕获缝只记 body/状态码不记头（:109-122）；全输出 grep 无 key；Keychain 须 DEVDESK_AGENT_LIVE_ALLOW_KEYCHAIN=1；run script 无 set -x；隔离根用完 remove_all ✅。info：appDb 打开失败的早退路径不删 stage/root（/tmp 空目录残留，trivial）。
- ⑧护栏：waitUntil 全带超时；http timeoutMs=30000（http_client.h:23）；run 有墙钟上限（agent_loop.cpp:185）；不绑 8888/15721 ✅。info：无 ctest TIMEOUT 属性（有上述兜底）。

## C. 实跑
- ⑨`build-test-run.sh --no-run` 全量复现：`Total: 1413 / Passed: 1407 / Failed: 0`，EXIT=0，与提交声明一致 ✅（6 差额=DOCTEST_SKIP）。

## 备注
审计中途该 worktree 曾出现瞬时外部 merge 态（ghsync 文件 staged、tests/CMakeLists.txt UU），数秒内消失，现 clean @80c070c；主会话并发 git 操作注意避让在飞审计树。

**裁决：放行带必修**（必修=⑤的离线事件等待线程单测）。
