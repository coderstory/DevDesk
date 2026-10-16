# agent_ui 修复批审计（a5b7117 + 1d771db）

- 对象：分支 `worktree-fix-agent-ui-assertions`，两提交 `a5b7117`（tests）+ `1d771db`（scripts），基线 dev `fc7a248`
- 方式：只读审计 + 只读复跑；日期 2026-10-16

## 核查结论

1. **断言修改正确性** ✅
   - 两处 400→500 只改高度断言（宽 760 未动），标题/注释同步。来源核实：M4 `8646167` 把 `kTaskDialogH` 400→500，现源 `src/module_agent/agent_module.cpp:225` = 500.0f。测试跟上实现，方向正确。
   - contextNote：`EUI-NEO/components/input.h:146-157` 核实「程序赋值且未聚焦 → cursor=0」确为有意行为（注释含 2026-09-24 DevDesk 导入表单反馈出处）。测试时序成立：compose 预填拨 0 → 手动 cursor/selection 落末尾（此后无 compose，无重置触发）→ `typeInto`（`tests/test_modules/test_agent_ui.cpp:104`，逐 UTF-8 字符直发 onTextInput）插在末尾 → 期望串 `旧记忆内容新增内容` 未变。与真实用户「点输入框末尾再输入」等价的论证成立（裸 Ui 无 Runtime 焦点消费端）。
2. **无顺手扩散** ✅ — `a5b7117` 仅碰 `test_agent_ui.cpp` 三用例邻近行（+14/-8）；`1d771db` 仅碰 `build-test-run.sh`。无第三文件、无无关用例。
3. **pipefail + `|| true`** ✅ — 实际 8 处 `|| true`（见 F1）。逐处核：4 处在显式失败分支内、紧跟真实退出码（`exit "$_build_rc"` / `exit 1`），只豁免 set -e 保摘要打印，不兜真失败；4 处 display/统计管道，红绿判定由下方独立 `grep -q "Failed: 0"` / `"100% tests passed"` / `exit 1`（L452）承担。`file_sha256` 的 `|| true` 保全 pre-pipefail 旧语义；空 sha ≠ 非空期望串仍硬失败（L717），无掩蔽。未改动管道逐条扫过：L303/318 `grep -m1` 有外层 `grep -q` 键存在性前置守卫，不可能无命中；`list_dynamic_deps` 只走进程替换（bash 不对进程替换套 set -e）；L762 `sed -n` 无匹配 rc 0。无漏兜点。
4. **PIPESTATUS → `_build_rc`** ✅ — pipefail 下管道码 = 最左非零 = cmake 码（仅 cmake 成功而 tee 失败时取 tee 码，仍非零 → 保守报失败，正确）；`_build_rc=0` 初始化 + `|| _build_rc=$?` 豁免 set -e，失败分支透传真码。调用方自保证据核实：`.github/workflows/ci.yml:70` `rc=${PIPESTATUS[0]}`。
5. **测试有效性** ✅ — 复跑实测（`DEVDESK_TEST_TMPDIR=/tmp/agentui-audit-tmp bash scripts/build-test-run.sh --no-run --filter agent_ui`）：`Total: 30 Passed: 30 Failed: 0`，SCRIPT-EXIT=0，守卫自检全过。变异验证可信：`nearPx` 容差 0.5px（`test_agent_ui.cpp:135`），500→400 差 90px ≫ 容差，变异必红。`:584`（修前行号）= `agent_ui_run_button_opens_detail_dialog` 的 run 线程 5s 墙钟等待用例，本批 diff 未触碰 → 豁免合理，但见 F2。

## Findings（均非阻塞）

- **F1（低）**：`1d771db` 提交信息写「6 处 `|| true`」，diff 实加 8 处；枚举按类目覆盖无遗漏，纯计数笔误。
- **F2（低）**：`:584` flake 豁免只在会话内，backlog / 提交信息 / 文档均无落痕。该用例是仓库少数墙钟等待型用例，并发负载下会超时；复发时建议登记 backlog。
- **F3（信息）**：脚本外再套管道的调用方仍拿 tail 的码——提交信息已自我声明且 ci.yml 已自保，无行动项。

## 裁决

**放行。** 三条 findings 均不阻塞合并；F2 建议复发时补登记。
