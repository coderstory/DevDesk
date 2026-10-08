# Wave B 项 B + 项 C 实施验证留痕

日期：2026-09-30 ｜ 分支：`feat/advanced-config-and-copy`（worktree `.claude/worktrees/adv-config-copy`，自 master `73af50b` 切出，含 0edd3cf env-pane 合并——满足 spec §2.4 项 B 对 `claude_ui_detail.cpp` 的串行施工前提）
依据：`docs/superpowers/specs/2026-09-30-batch-fix-design.md`（§3/§4/§5）+ `docs/superpowers/plans/2026-09-30-batch-fix-audit.json`（9 findings 处置见下 §4）

## 1. 提交

| 项 | sha | 内容一句 |
|----|-----|---------|
| B | `70ea836` | 表单尾插第 4 tab「高级配置」（方言下拉 + failover 下拉自基础页整段迁入、基础页 66px 重排、submitForm 两处接线）+ 详情 tabs 4→5（高级配置 idx 3 只读 pane，最近调用顺移 idx 4）+ `upstream_dialect` 只增 `dialectKindDisplayName` |
| C | `2e6d9ed` | 「复制」改名「建副本」（按钮/菜单/toast）+ 新增「复制配置」（`buildProfileExportJson` 纯函数复用 buildDirectEnv → `{"env":{...}}` → `setClipboardText`，占位 sk-ant-***<尾4>）+ 详情 7 键（actW /7）+ 右键菜单五项（case 重映射） |

## 2. 测试报数

| 套件 | 命令 | 结果 |
|------|------|------|
| 定向单测 | `bash scripts/build-test-run.sh --filter claude --no-run` | **Total: 131 / Passed: 131 / Failed: 0** |
| uiModern（含右键菜单用例） | `./build/tests/devdesk_tests 右键菜单五项` + `uiModern` | 1/1 + **17/17** |
| probes | `(cd build && ctest -L probe)` | **8/8 全绿**（claude_probes 含新增 7 键键宽断言） |

新用例（B）：`claude_ui_advanced_tab_dialect_persists`（新建/编辑落库、自动档、接线回归钉、表单 tab3 几何）、`claude_detail_advanced_pane_readonly`（显式覆盖/自动推断/备用名/悬空「（已删除）」/tab4 仍最近调用/页签文字↔索引映射钉/pane 不越内容框）、`claude_detail_actions_anchor_four_tabs` 扩 5 页签。
新用例（C）：`claude_export_json_shape`（顶层 env 恰一键/尾斜杠剥离/bearer·api_key·both 三态键名/空占位无凭据键/片段冻结键赢/坏 env 容错）、`claude_copyconfig_ui_seven_keys`（改名/7 键齐/warn/派发 toast）、`claude_copyconfig_seven_keys_geometry`（actW=(w0−72)/7 + 末键右缘对齐）。

测试耦合点同步（spec §3.5 风险 3）：`claude.detail.tabs.hit.3` 导航锚 test_claude_ui.cpp 10 处 +1（`detailTab_` 在测试零引用——grep 它会漏改）；`claude_probes.cpp` tab 循环上界 3→4；test_ui_modern.cpp 调用记录 virtualList 用例 hit.3→hit.4（audit 未列入耦合清单的第 11 处，实改）；原 failover 用例（`:2481`）迁入 form.tabs.hit.3（重开表单回 tab 0，fb_e7 轮询内补切）。

## 3. 变异自检（三组，红→绿留证）

| # | 变异 | 红灯证据（回退前） | 回退后 |
|---|------|-------------------|--------|
| B1 | submitForm `f.formDialectOverride_` 改回硬编码 `std::string()` | `FAIL test_claude_ui.cpp:2733 waitFor(...)`（新建落库 `opencode_zen` 不成立） | --filter claude 131 全绿 |
| B2 | 详情 tabs 尾插改插中间（环境变量前） | 首次未红——pane 内容按 dispatch 索引走、与页签文字解耦（假绿缝）→ 补「页签文字↔索引映射钉」（`claude.detail.tabs.label.3/4`）后：`FAIL :4098 lab3->text != "高级配置"` | 131 全绿 |
| C1 | `buildProfileExportJson` 占位改明文（硬编码 `sk-ant-PLAINTEXT-leak`） | `FAIL :3882 ANTHROPIC_AUTH_TOKEN != "sk-ant-***1234"` | 131 全绿 |
| C2 | actW 公式 `/7` 改回 `/6`（旧 6 键式） | `FAIL :4324 fabs(cc->frame.width - expect) < 0.01` | 131 全绿 |
| C3 | 右键菜单 case 2 改派发删除（index 重映射回退） | `FAIL test_ui_modern.cpp:397 toast 剪贴板 miss`。⚠️ 过滤器坑：按**用例名** `uiModernClaudeProfileCtxMenu` 跑匹配 0 条（过滤器匹配的是描述串）→ 曾得假绿；正确过滤 `右键菜单五项` 实测红 | `右键菜单五项` 1/1 绿 |

## 4. 审计 findings 处置落点

- F1（在飞状态失实）/ F2（编辑清覆盖假 bug）：spec 已修订；实施按「接线回归钉」口径（B1 用例第三分支），未做先红后绿的修复类验证。
- F3（hit.3 真耦合点）：§2 已全量 +1（含 audit 未列的 test_ui_modern.cpp 第 11 处）。
- F4（注释漂移）：state.h / detail.h / detail.cpp / form.cpp 的 tab 数与「三 pane/四页」口径全部同步（含锚定注释 ②③ 档固定行数清单 +214）。
- F5（剪贴板 void 无回读）：按 spec §4.2② 接线处固定 LOG_WARN 一行（profile id + 导出键数）；未做上游 PR（留 backlog）。
- F6（坏 env 静默丢键）：导出前试解析 + LOG_WARN 继续（`doCopyConfig`）；形状用例覆盖坏 env 导出仍成功。
- F7（显示名单一来源）：`dialectKindDisplayName`（upstream_dialect.{h,cpp}），表单 items 与详情 pane 共用。
- F8（headless 剪贴板行为）：已实测——devdesk_tests 进程 GLFW 未 init，`glfwSetClipboardString` 经 `_GLFW_REQUIRE_INIT` 早退（input.c:1459）为安全 no-op，全量用例无副作用；probe 不点 copyconfig 键（`--dump` 注释明示）。
- F9（半角括号）：「自动（按站点推断）」全角，用例断言逐字。

## 5. probe 键宽裁决（§5⑥）

最小窗 1200×720 档实测 **actW = 82.29px ≥ 60px**（四字文案 4×13=52 + 8 呼吸；即使按 audit 预估的 76px 宽口径仍达标）→ **单行 7 键维持，不切两行**。三档输出：
`1200x720: 82.2857 ≥ 60` / `1200x1000: 82.2857 ≥ 60` / `1262x801: 91.1429 ≥ 60`（末键右缘对齐三档全过）。audit 预估「大概率不达标」不成立。

## 6. 截图（probe `--dump` 真渲染 glReadPixels，非 screencapture——锁屏/旧帧坑免疫）

`docs/screenshots/2026-09-30-wave-b/`：
- `waveb_detail_adv.png` 详情「高级配置」pane（生效方言/备用 Profile 两行 + 五页签 + 7 键）
- `waveb_form_adv.png` 表单「高级配置」页（方言 + 备用 Profile 两下拉 + 提示）
- `waveb_actions7.png` 详情操作组 7 键（基础信息 tab）
- `waveb_ctx_menu.png` 右键菜单五项（编辑/建副本/复制配置/删除/设为当前）

首版 dump 曾抓到弹窗 fade 半透明中间态 → dumpState 补 40 帧预热（≈0.67s）后干净。

## 7. 遗留项

- `setClipboardText` 返回 bool 的 EUI-NEO 上游 PR（audit F5 建议②）未做——当前以 LOG_WARN 留痕 + 已知限制。
- 剪贴板真写与粘贴内容人工核验：留给真机验收（spec §4.4 截屏段；headless/probe 均未写真剪贴板）。
- 详情标题 24px 盒等 spec §2.3 tech-debt 项不在本批范围（项 A 边界）。
