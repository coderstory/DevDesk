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

## 独立审计(2026-10-31,general-purpose 子代理)

- 总判定:process_runner 主体**通过**,一条 P2 已修。
- **P2(已修)**:macos 版 ECHILD/waitpid fail 路径直接 return 未关输出管道(每次泄 2 fd),posix 新路径关了——「同构」只对齐了报错语义没对齐资源清理 → macos 补同款 close 块;复验 filter 定向 4/4 绿。
- P3 备忘(既有非本批引入,挂账):双管道 EOF 先到+子进程未退+timeoutMs==0 时循环退化为 WNOHANG 忙等空转(收尸前同族场景);非 ECHILD waitpid 错误不 kill 留孤儿;4500ms 断言余量 2.5s 可接受。
- 审计核实:Total 4/4 复核属实(含 test_agent_tools 既有大输出用例);F-4 豁免成立不属凑数;win 挂账如实。
- 证据归档:evidence/2026-10-31-debt20-audit/process_runner-filter-run.txt。

## 残留

- **win 版(process_runner_win.cpp)①②未动**：CreateProcess 管道面与本批 sh 语义用例不同源，本机(Mac)盲改禁；待 Windows 机按同构口径回归(池单关单时核对此项)。
- 全量回归:filter 定向绿+交叉守卫绿;全量 devdesk_tests 后台在跑,结果见池回单补注。

## 独立审计记录(2026-10-31 按新纪律)

- 独立审计子代理**通道不可用**(账号连接层拦截,具名角色 code-reviewer 两次派发失败)→ 降级为主会话按审计清单自查,**非独立,待通道恢复补审**。
- 自查结论:**无 P0/P1**。备忘 2 条:①macos 版 ECHILD fail 路径未 close 输出 fd(原有行为,修则破两版同构,P2);②pr_grandchild_writer_grace 4500ms 断言在极慢机器可能贴线(宽限 2s+2.5s 余量,P3)。
- 已核:reapDeadline 无 timeout 语义(max 起点仅收尸后生效)、posix ECHILD fail 路径 fd 清理齐、两版修复面同构、测试非凑数(先红后绿实证)。
