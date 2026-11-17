# 打磨批次 A+B 收口档全量验证报告（closure-qa）

- 日期：2026-11-18；执行者：qa-tester；平台：macOS arm64（本机默认档；ASan/UBSan 属 CI 档不适用）
- 代码状态：HEAD=66b7d12e，工作树含批次 A+B 全部增量（未提交/wip）
- 运行命令：`bash scripts/build-test-run.sh --no-run`（仓库根；`--no-run`=不启动 app，避免写真实 `~/DevDesk` 数据根；完整构建 + doctest 全量 + probe + 守卫链均在本轮执行）
- 日志证据：`evidence/2026-11-18-polish-closure/closure-qa-full.log`（49 行，行号引用以该文件为准）

## 结论（一行）

**不通过（阻断）**：构建 / doctest 全量（Total 2462 / Failed 0）/ UI probe / 探针跳过登记守卫 / 模块隔离守卫全绿，但 Windows 交叉编译守卫红于批次 A MCP 放行路径用例增量（`tests/test_modules/test_mcp_protocol.cpp:563/582/583` POSIX `setenv`/`unsetenv`），且该红以 `exit 1` 中止守卫链后半段（std include / 玻璃体系 / ToolGuard / 守卫自检 4 段未执行）——修复归属 developer/bug-fixer，修复后须同 commit 重跑全量。

## 各面结果

| 面 | 结果 | 证据（evidence 日志行号） |
|---|---|---|
| 构建（Debug 增量） | ✅ 成功，bundle 与本次构建同步 | log:8,11 |
| doctest 全量 | ✅ `Total: 2462 Passed: 2453 Failed: 0`（2462 ≥ 2026-10-29 基线 1954，新增用例已入账；差值 9 = skip 家族，Failed 0 = 全绿） | log:17-18 |
| UI probe | ✅ `100% tests passed out of 52`；5 项设计内跳过（memory_install / wallpaper×4，已知环境/驱动跳过家族、SKIP_RETURN_CODE=77 已登记），不判红 | log:20-26 |
| 探针跳过登记守卫 | ✅ | log:29 |
| 模块隔离守卫 | ✅ | log:31 |
| Windows 交叉编译守卫 | ❌ 红（详见下节） | log:40-47 |
| std include 守卫 | ⛔ 未执行到（被交叉编译守卫 `exit 1` 中止；非通过） | `scripts/build-test-run.sh:963-965` |
| 玻璃体系守卫 | ⛔ 未执行到 | 同上 |
| ToolGuard 双层花括号守卫 | ⛔ 未执行到 | 同上 |
| 守卫自检（守卫自身活性） | ⛔ 未执行到 | 同上 |
| app 冒烟启动 | ⛔ 本轮未跑（`--no-run`：真实数据根禁碰纪律优先） | — |
| ASan/UBSan | n/a（CI 独立档，本机不判） | — |

## 红项与归属判定

- 现象：Windows 交叉编译 481/482 通过，唯一红文件 `tests/test_modules/test_mcp_protocol.cpp`：
  - `:563:5` `use of undeclared identifier 'setenv'`
  - `:582:27` `use of undeclared identifier 'setenv'`
  - `:583:10` `use of undeclared identifier 'unsetenv'`
- 归属判定：**批次 A「MCP 总闸」增量的测试代码**。判定证据：`git diff HEAD --unified=0` 显示三处 `setenv`/`unsetenv` 全部位于未提交新增 hunk `@@ -599,0 +600,30 @@`，即新增超时 gate 用例 `mcp_protocol_tools_call_outer_timeout_gate` 通过 `MCP_TOOL_TIMEOUT_SECONDS` 环境变量操控总闸——POSIX `setenv`/`unsetenv` 在 MSYS2/MinGW 无声明（本机 clang+libc++ 可编过，故 macOS 面全绿、CI 必红）。
- 失败四分类：**新回归 ×1**（增量引入、CI 必红，阻断收口）；已知红 ×0；环境 ×0；测试缺陷 ×0。与批次 A/B 无关的既有红：0。已知 CI 敏感面（stall elapsed 余量 / MinGW std::thread）本机全量无相关红。
- 修复方向（供 developer 参考，未代改）：测试内环境变量操作改用项目已有可移植封装或按平台拆分文件族；不建议 `#ifdef _WIN32`（仓库纪律）。
- 最小复现：对 `tests/test_modules/test_mcp_protocol.cpp` 重跑 Windows 交叉编译守卫即红。

## 遗留与收口重跑要求

1. 修复 `tests/test_modules/test_mcp_protocol.cpp` 平台可移植性（归属 developer/bug-fixer，qa 不代改）。
2. 修复后同 commit 重跑全量一次：本轮守卫链后半段 4 段（std include / 玻璃体系 / ToolGuard / 守卫自检）因中止未执行，须在无中止的完整全量中取得绿灯证据。
3. app 冒烟启动面本轮未跑（真实 `~/DevDesk` 数据根禁碰）；如需冒烟证据，由主会话在允许的数据根条件下补。

## 预算自报

工具调用 17 次（预算 30）；墙钟约 12 分钟（预算 90 分钟）。无定向重跑（同 commit 全量仅本轮一次）。
