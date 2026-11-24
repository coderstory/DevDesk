# need505 测试分片并行——独立抽查审计（只读）

- 对象：worktree `agent-adf1ac9f9ba669266`，commit `1c6386db`，基线 `671e47f0`
- diff：`tests/test_main.cpp`（--shard=i/N FNV-1a 分桶）+ `scripts/build-test-run.sh`（--parallel-tests 编排），仅两文件
- 审计角色：code-reviewer（独立，非实现者）；口径 M 级抽查；预算内完成

## 逐维度结论

1. **正确性 — 已核，3 findings（F3 important / F4 nit）**。FNV-1a 为名字纯函数，同用例恒定落同片、跨运行稳定；`--shard=` 在 parseFilterTerms 剥离不混入过滤词（test_main.cpp:246-248）；i/N 越界退出 2（:367-371），与 0 命中防假绿语义同码；汇总 T/P/F/K 跨片求和含 Skipped 合并，实证 par2/par3 两跑 Total 2117 与串行一致；「片 N：[FAIL] 用例名」确来自片日志 grep（脚本 :591-596）。分叉见 F3/F4。
2. **脚本健壮性 — 2 findings（F1/F2 important）**。任一片 rc 非 0 → TEST_RC=1 → 整体红（含崩溃/0 命中/坏参数，无 [FAIL] 时打「退出码 rc + 片日志路径」）；SHARD_DIR 先 rm -rf 再建、sN.log/sN.rc 无片间冲突；串行路径逐行比对零改动（仅外包 else/fi，默认关）；DEVDESK_TEST_SHARDS 未校验（F1）、并行支 TEST_RC 未初始化（F2）。片日志目录并发互删风险见 F5。
3. **守卫兼容 — 已核，无问题**。全仓（scripts/.github，排除 build/EUI-NEO）grep `gate-summary` 零命中——本仓无该文件的生产/消费代码；并行红绿判定走退出码，与串行同构，probe 段/守卫链未被本 diff 触及。若 gate-summary 判读在外部工具（仓外），需该工具侧自证，本仓层面无风险。
4. **F2 零改动声明 — 属实**。git diff 基线核实 `tests/test_modules/` 零 diff；pickFreePort 动态端口属实（test_claude_ui.cpp:86，bind 0 借 http Server）；8ms 记录式属实（test_md_block_model.cpp:505、test_md_highlight.cpp:559「记录式，不设墙钟红线」）。注：pickFreePort 在 :86 非任务书写的 :88。
5. **测试纪律（注坏验证）— 属实**。/tmp/need505_par2_red.log：片 0「复位自证①」+ mdspike V4a/V4b + menu styled = 4 失败（3 既有 + 注坏 1）；par3_green.log：Failed 3、「复位自证①」消失——真红真还原。命名瑕疵：par3_green 实为「还原基线 3 失败」非全绿，文件名易误读（/tmp 临时产物，不立案）。

## Findings

| # | 位置 | 问题 | 影响 | 建议 | severity | lens |
|---|---|---|---|---|---|---|
| F1 | scripts/build-test-run.sh:570 | `DEVDESK_TEST_SHARDS` 零校验：0/负数/非数值 → `seq 0 $((N-1))` 空序列 → 0 片 → 打 `Total: 0`「✅ 测试全绿（并行 N 片）」 | 0 例假绿——本文件注释自认最危险形态；一个空变量静默放行全门禁 | 数值+≥1 校验（case 非数字或 <1 → 打 ❌ 退出 2），校验放循环前 | important | robustness |
| F2 | scripts/build-test-run.sh:601 | 并行支未初始化 `TEST_RC`（串行支 :607 有 `TEST_RC=0`）：全片通过时变量为空，`[ "" -ne 0 ]` 报 integer expression error → 错误退出 2 → if 走 false 分支侥幸打绿 | 现行为碰巧正确但依赖报错语义；将来 `set -u` 即崩，或被改坏成真假绿 | 聚合循环前补 `TEST_RC=0` | important | robustness |
| F3 | scripts/build-test-run.sh:564-605 | `--parallel-tests` 与 `--filter` 组合语义分叉：filter 命中集中少数片 → 其余片 0 命中 → C++ 退出 2 → 整体红；同 filter 串行绿 | 定向跑测切并行预期提速实则假红，误判回归；头注用法未声明该组合受限 | 同用 --filter 时回退串行，或 0 命中片豁免退出 2；至少头注写明 | important | correctness |
| F4 | tests/test_main.cpp:361 | `atoi` 无数值校验：`--shard=abc/6` 静默按片 0 跑 | 仅直跑二进制可触发（脚本恒产数字）；误读片身份难排查 | strtol + 全串数值校验，或注释声明脚本专用入口 | nit | correctness |
| F5 | scripts/build-test-run.sh:575 | SHARD_DIR 固定 build 目录 + `rm -rf`：同 build 目录并发两脚本实例互删片日志/互抢 rc（串行支无盘上状态，此为新增耦合） | 「同时 1 个测试」纪律兜底下低危；破坏纪律时失败定位信息互相污染 | `mktemp -d` 或 `$$` 子目录 | nit | robustness |

## 总判定

**可合入（带条件）**：F1/F2/F3 合入前必须处置（三处都是并行路径，默认关不影响 CI；但 F1 是假绿形态、F3 是用户可踩的组合分叉）；F4/F5 入台账。串行默认路径零改动已证，等价性实证在案。
