# 详情页「环境变量」pane 修复 — 审计处置验证记录

> 分支 `fix/detail-env-pane`（worktree `wt-detail-env-pane`），实现 commit
> `1ca87b0`，审计 findings：`docs/superpowers/plans/2026-09-30-detail-env-pane-audit.json`。
> 本文档 = E-AUD-4（变异自检证据落盘）+ E-AUD-2（挂账登记）。

## E-AUD-1 处置（必修，已修）

**Finding**：长模型键断言被显示截断掩蔽——keyMaxChars=27@100%/21@130% 把
`ANTHROPIC_DEFAULT_OPUS_MODEL`(29)/`ANTHROPIC_DEFAULT_HAIKU_MODEL`(31) 渲染成
「前 26 字符 + …」，按全键名 `count`/`find` 的断言在泄漏时也恒真（假绿）。

**修法**：测试改判据为**截断感知泄漏检测**（`tests/test_modules/test_claude_ui.cpp`
新增 `leakedModelEnvKey()`，两个 env 用例共用）：渲染文本**等于**某模型键全名，
**或**剥掉截断省略号「…」后是某模型键的**真前缀**，即判泄漏。
`ANTHROPIC_BASE_URL` / `ANTHROPIC_AUTH_TOKEN` / `MY_*` 等合法可见键不是任何
模型键的前缀，不误报。键集仍与实现同源（`module::claude::modelSlotTable()`）。

## E-AUD-4 变异自检证据（E-AUD-1 修复后复跑）

**变异方式**：`src/module_claude_proxy/claude_ui_detail.cpp` 过滤条件临时改为
`if (false && isModelMappingEnvKey(it->first))`（禁用模型键过滤，其余不动）。

**命令**：

```bash
bash scripts/build-test-run.sh --filter 页签 --no-run
```

**变异后（期望红）—— 实测红**：

```
[FAIL] claude_module: 环境变量页签列生效 env（模型键除外）+ 来源标注 [15ms]
[FAIL] claude_module: env 页签行几何不重叠 + 模型键不渲染（含 130% 档） [17ms]
Total:  3 Passed: 1 Failed: 2
```

两用例均在截断感知判据下变红（fail-fast 停在首个泄漏断言；泄漏键包括被显示
截断的长键形态——修复前该形态恒真，修复后可检出）。

**恢复过滤后（期望绿）—— 实测绿**：

```bash
bash scripts/build-test-run.sh --filter 页签 --no-run
# Total:  3 Passed: 3 Failed: 0

bash scripts/build-test-run.sh --filter claude --no-run
# Total:  126 Passed: 126 Failed: 0
```

探针：`(cd build && ctest -L probe)` → 8/8 全绿（2026-09-30 实测）。

## E-AUD-2 处置（low，挂账不修）

**Finding**：def 槽位空 + extra_env 手写模型键（如 `ANTHROPIC_MODEL`）→ 该键
在环境变量 pane 被过滤（E1）、「模型映射」页签又只显示 models_config 槽位
（槽位空 = 未配置）→ **两页都看不到这个键的生效值**。

**为什么挂账不修**：正常路径（槽位填充）模型键在两页的分工成立；角例的根因是
「生效 env 的模型键来源有两种（槽位 / 手写），两页各自只认一种」。修复需要在
模型映射页签加「extra_env 手写模型键」附加展示区或跨页提示——涉及另一页签的
信息架构，超出本缺陷修复范围，留待模型映射页签的下一次迭代。

**升级路径**：若用户反馈「我手写的模型键去哪了」，引导看 Profile 编辑表单的
高级 env 页（extra_env 全量可见、可改），或把 def 槽位填上（手写键被托管键
覆盖，值以槽位为准）。

**登记位置说明**：`docs/KNOWN_LIMITATIONS.md` 已在 commit `5bc5a0a`
（2026-09-29，用户指示「删除 KNOWN_LIMITATIONS 及其在 README 的引用」）中整档
删除，故本角例登记于本文档而非复活该文件；溢出提示文案已顺手带上
「extra_env 手写的除外」提示（同 commit 的 `claude_ui_detail.cpp`）。

## E-AUD-3 处置（info，已修）

文件头 P3 行补注：「E1 起模型映射键收窄至『模型映射』页签，见下方 2026-09-30 块」。
