# 债-20 process_runner 三平台共同待办 测试清单卡

- 范围:macos/posix 两版修复+真实现测试;win 版**未动**(本机不可验证,残留见尾)
- 执行人/时间:ZCode 主会话,2026-10-31 03:4x
- 登记:3d59d076(前序 P1 管道互锁修复,本单是其审查产出的三项残余)

## 三项修复

| 项 | 病灶 | 修法 | 文件 |
|---|---|---|---|
| ①收尸后 EOF 宽限 | 收尸后 `continue` 拖 EOF 无上限,孙进程持写端时随其挂满全程 | 收尸时起算 2s 宽限(kReapGraceMs),到点强关走人,结果已完整 | macos+posix 同步 |
| ②POLLERR/POLLNVAL 排水 | revents 只认 POLLIN\|POLLHUP,坏 fd 永留 poll 集 20ms 空转死循环 | revents 判定集加 POLLERR\|POLLNVAL,统一走 drainOnce 出错收尾 | macos+posix 同步 |
| ③posix ECHILD 口径 | posix 版 `break` 静默:丢输出丢退出码,与 macos 版(exec_capture 同口径显式 fail)不一致 | 对齐 macos 版显式 fail,带 close 清理 | posix |

## 测试(F-ID)

| F-ID | 内容 | 方法 | 结果 | 证据 |
|---|---|---|---|---|
| F-1 | 基础契约:回显/stderr/stdin/超时杀 | 真实现跑 shell(REGISTER_TEST pr_basic_contract) | **PASS** | filter 定向输出 |
| F-2 | >64KB 大输出不互锁回归(3d59d076 病灶防回退) | yes\|head -c 200000 断言 size | **PASS** | 同上 |
| F-3 | **先红**:孙进程持写端 `(sleep 5) & echo done` 无宽限 | 修复前跑,断言 elapsed<4500 | **红→绿** | 红:5016ms 判红(挂满孙进程);绿:修复后 ~2s 量级返回 |
| F-4 | ②③ 无直接红测试 | 进程内不可构造(管道 POLLERR 罕见、ECHILD 需全局信号干预),不写假测试;靠 macos/posix 同构对齐+评审 | **设计内豁免** | 本卡 |

判据输出:`bash scripts/build-test-run.sh --filter process_runner --no-run` → `Total: 4 Passed: 4 Failed: 0`;Windows 交叉编译守卫过(win 面用例整体空转)。

## 残留

- **win 版(process_runner_win.cpp)①②未动**:CreateProcess 管道面与本批 sh 语义用例不同源,本机(Mac)盲改禁;待 Windows 机按同构口径回归(池单关单时核对此项)。
- 全量回归:filter 定向绿+交叉守卫绿;全量 devdesk_tests 后台在跑,结果见池回单补注。
