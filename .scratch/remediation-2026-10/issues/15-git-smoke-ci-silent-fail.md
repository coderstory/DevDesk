# 15: git.* 冒烟测试 CI 静默失败诊断（git.status 在 runner 上异常）

**What to build:** 修掉 `builtin_git_smoke`（git.status 对仓库根冒烟 + git.commit isDestructive + 坏仓库报错）在 GitHub Actions runner 上双平台（Windows/Windows-2025 与 macOS）必失败、本地（含干净克隆 Release 构建）全绿无法复现的问题。本票先诊断后修复：拿到 runner 上的真实失败点，再决定修工具（`git.*` 的 spawn/env 处理）还是修测试的环境假设。

**已知事实（2026-10-26 三轮 CI + 本地实验收集）：**
- 失败特征：`[FAIL] git.*: ... [0ms]`——函数体近乎瞬间返回 false；**零断言失败行、零异常行、零 lastFailNote**（7f053a2b 已加宏级笔记 + runner 兜底打印，笔记为空 = 体 内任何 CHECK 都没执行到）。源码上该函数唯一 return 是 `true`，CHECK 失败必打 cerr——逻辑上矛盾，指向「runner 二进制里的执行路径与源码意图不符」或某个未知的早期退出机制。
- 本地不复现：主仓 Debug/Release 全绿；干净克隆（7f053a2b）Release 构建后单跑 196ms PASSED。
- 差异方向：runner 的 git 版本/配置（actions/checkout 注入的 http.extraheader、includeIf 凭据配置）、msys2 shell 的 PATH（可能有 msys2 git 与 Git-for-Windows 之分）、HOME/环境变量。工具实现 `GitStatusTool::execute` → `runGit(repo, {status --porcelain -b})`，spawn 失败/非零退出都会被测试断言捕获——但断言根本没执行到。
- 测试文件已带载荷诊断（CHECK_MESSAGE 打印 contentJson），下次 CI 失败时若走到断言会直接给出 git 报错文本。

**排查建议（按序）：**
1. 重跑 job 并勾选「Run with debug logging」，或用 tmate/SSH action 进 runner 手跑 `build-release/tests/devdesk_tests.exe "status smoke"` 与裸 `git -C <repo> status --porcelain -b` 对照。
2. 怀疑点 A：actions/checkout 注入的 `http.https://github.com/.extraheader` 让工具 spawn 的 git 在非交互环境下等待/报错（工具是否继承了异常多的 GIT_* env）。
3. 怀疑点 B：`runGit` 的 spawn 方式（shell?）在 runner PATH 下解析到不同 git；0ms 失败更像 spawn 前就退出（路径校验？repoRoot() 为空串？）。
4. 修复后本票验收 = 双平台 CI 该用例 PASS；顺带确认 `assert:` 兜底行机制工作正常。

**Blocked by:** 无（需有 GitHub Actions 操作权限的会话配合重跑/SSH）。

**Status:** open（诊断数据见上；2026-10-26 整改会话移交）

- [ ] 拿到 runner 上真实失败点（断言级或工具级错误文本）
- [ ] 根因修复（工具或测试）
- [ ] 双平台 CI git.* PASS
