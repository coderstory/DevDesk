# 审计：worktree-agent-branchfix（基 4ea6e4a，ebf815c，+207/−6）

1. **修法完整性 — 过**。全仓写调用点仅 settings_module.cpp:1152/:1192 putFile、:1236 deleteFile，三处均已传 branch；手动推/PullThenPush/首同步双向/退出补推（:960/:968）全汇聚到 runGithubSyncPush(:1000) 唯一函数，branch 线程启动时按值捕获(:829-831)。拉取侧 getFile ?ref=(:497)/listTree 本带分支，一致。测试层不传 branch 是有意测空值语义（空=默认分支，读写两侧行为一致）。
2. **setDefaultSender — 过，2 info**。与 setSleepFunction 同型属实（均函数局部 static + 无锁赋值，github_api.cpp:317-322 vs :265-284）；生产零调用点，默认行为零变化；REQUIRE=return false 无异常（doctest.h:57），SenderGuard RAII 必析构复位，无跨测试泄漏。info① github_api.cpp:278 构造函数两次读 defaultSender()，与 setDefaultSender(nullptr) 并发时窗口内可留下空 std::function（bad_function_call）——仅测试期可达，建议单读局部变量。
3. **deleteFile 签名 — 过**。全仓唯一调用点 :1236 已更新；无宏/模板隐式调用（含 scripts 全 grep 零命中）。info② github_api.cpp:647：「必传」仅编译期强制传参，显式传 "" 仍静默落默认分支；配置默认 "main"（github_sync_config.h:20）且空值推拉两侧行为一致，纯理论路径。
4. **测试强度 — 强**。真按钮→真 token 输入框→后台线程→onStop join 全真路径；断言录到的 HTTP 请求 wire body（branch 字段）+ DELETE URL + PUT 冲突 sha + 拉取侧 GET ref=sync-branch（防两边同退默认分支假绿）。变异验证：摘掉 SQL PUT 的 branch → [FAIL] 1/0/1 红；还原 → 1/1/0 绿，git status 干净。info③ 新测试未断言 DELETE body 的 sha 字段（仅 URL+branch）——既有行为，不在本票范围。
5. **卫生 — 净**。+176 测试 / +31 实现，占比合理；无调试残留、无密钥（假 token/假 repo）；注释符合契约纪律。
6. **实跑** — `build-test-run.sh --no-run --filter github` → Total 65 / Passed 65 / Failed 0，全守卫绿，复现成功。

**总裁决：放行**（3 条 info 均不阻塞：①② 测试期竞态/软契约且与既有 sleepFn 同型，③ 范围外）。
