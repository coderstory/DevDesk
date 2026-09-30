# 高级设置 JSON（非 env 属性）+ 生成链合并 + 共享配置片段重构——实施验证落证

日期：2026-09-30 ｜ 分支：`feat/advanced-json-editor`（worktree，自 master `0f1f734` 切出）
依据：`docs/superpowers/specs/2026-09-30-advanced-json-editor-design.md`（唯一权威，已按独立审计 10 findings 修订）+ 审计留痕 `plans/2026-09-30-advanced-json-audit.json`

## 1. 交付五件（原子 commit，显式 pathspec，顺序按依赖）

| # | 内容 | sha |
|---|------|-----|
| 0 | docs: 设计文档 + 审计留痕 | `cf43dbe` |
| 1 | §3 数据层：Profile::extraSettingsJson + migration v11 + Repository 五处同步 + duplicateProfile（F-3） | `0391e1d` |
| 2 | §4+§5 生成链纯函数（三件套）+ sidecar v2（merge-only 补拍，F-1）+ takeOver/restore/runProfile 接线 | `3914514` |
| 3 | §6 导出完整 settings.json 形态 + toast 文案 | `903d7de` |
| 4 | §7 高级配置 tab JSON 编辑框 + 绑定下拉迁入 + submitForm 校验/pretty 落库 | `751ac11` |
| 5 | §8 片段弹窗 JSON 化 + 生成时叠加（片段赢）+ env 域管线真拆 + 详情 pane 键来源（§8.5） | `d8056c2` |
| 5b | fix(test): pane 两用例补 ui.layout（修变异⑥初证暴露的假绿，见 §3⑥） | `c32581a` |
| 6 | verify: 本文档 + probe 扩展 + 截图 | （本 commit） |

## 2. 测试数（全部实跑，用例总数以运行输出 `Total:` 为准）

- 全量：`Total: 885 / Passed: 879 / Failed: 0`（Total−Passed = 6 条环境门控 SKIP，含 `DEVDESK_SOAK=1` 门控 soak）
- 定向：`--filter claude` → **138 / Failed 0**；`--filter json` → **29 / Failed 0**；`--filter settings` → **100 / Failed 0**；`migration` → 6/0；`片段` → 21/0
- 探针：`ctest -L probe` → **8/8 全绿**（claude_probes 含新增 shared_dialog 落图 + JSON 域 seed）
- 新增用例（本批）：迁移 v11（`migrate_extra_settings_v11`：v10 旧库升级 + extra_env 列已删 + 新列读写回）／纯函数四组（effective 片段赢五态 / apply 抑制 / validate 五态 / F-10 槽位派生）／往返①-⑤+⑥（F-1 反例、F-8 门面装配端到端）／D-2 格式化器四断言／UI（compact 落库 pretty、非法+env 键拒绝、格式化按钮、绑定迁移等价、片段弹窗 JSON 往返、pane 键来源+溢出护栏）／导出完整形态
- 三边界（§10）：超 1MB / 含 env 键 / 非法 JSON 全部在 `provider_config_extra_settings_validate` 五态 + UI 拒绝用例覆盖，红→绿见 §3①

## 3. 变异自检（§10 全清单，各回退一处 → 对应用例红 → 复原 → 绿，全部实跑）

| # | 变异 | 红 | 绿 |
|---|------|----|----|
| ① | 删 submitForm 校验闸门（validate 改恒过） | `高级设置 JSON 非法/含 env 键 → formError_ 红字不落库` 红（坏 JSON 落库 + env 键文案消失） | 62/62 |
| ② | 删 model 抑制（apply 步 `if (writeModel && false)`） | `JSON model 键抑制 ANTHROPIC_MODEL，managedKeys 并集仍含该键` 红 | 1/1 |
| ③ | restore 删 root 层记账（重建块 `if (false)`） | `restore 按 nonEnvKeys/nonEnvSnapshot 恢复或删除顶层键` 红 | 1/1 |
| ④ | 落库 pretty 改存原文（**表单输入 = compact 单行**，审计 F-7 口径） | `compact 输入落库为 pretty 文本（变异④钉子）` 红 | 1/1 |
| ⑤ | `effectiveExtraSettingsJson` 叠加顺序对调（改 profile 赢） | `effectiveExtraSettingsJson 片段赢/解绑回落/…` 红 | 1/1 |
| ⑥ | `drawDetailAdvanced` 退回 `(void)contentBottom`（行数预算拆掉，恒画 8 行） | `键清单溢出——行数按 contentBottom 推、不越内容框` 红 | 3/3 |

**⑥ 的过程记录（重要）**：初证未红——排查发现是**两用例自身假绿**（compose 后未 `ui.layout(w,h)`，所有节点 frame 恒 0，`行底 ≤ 内容框底` 恒真），非护栏失效。补 layout（`c32581a`）后以真实 frame 复证红→绿。教训已印证审计 F-7 的同型风险：几何断言必须核对「变异真的会让它红」，恒真断言即假绿。

## 4. probe `--dump` 真渲染截图（glReadPixels，40 帧预热，wave-b 先例；非 screencapture，锁屏/旧帧免疫）

`docs/screenshots/2026-09-30-advanced-json/`：
- `advjson_form_adv.png` —— 表单「高级配置」页：绑定片段下拉（迁入）+ 高级设置 JSON 多行框（预填 pretty）+ 格式化按钮 + 「同名键以片段为准」提示行
- `advjson_shared_dialog.png` —— 片段编辑弹窗 JSON 化：name + 高级设置 JSON 框 + 格式化 + 编辑态删除
- `advjson_detail_adv.png` —— 详情「高级配置」pane：生效 JSON 逐键来源标注（hookA=片段 / model·permissions·statusLine=本 Profile）+ 「model 覆盖模型槽位（ANTHROPIC_MODEL 不写入）」说明
- `advjson_actions7.png` / `advjson_ctx_menu.png` —— 回归基线（7 键操作组 / 右键菜单，与 wave-b 同位）

初版 shared_dialog 曾叠着右键菜单残影（ctx_menu 在前、菜单常驻构建保持开态）→ dump 顺序对调后干净。

## 5. 关键实施裁决与落点回写（§8.4 表「实施再遇清单外引用点按同语义处置并回写」）

- grep 全仓对账后无清单外死引用残留；`isModelSlotEnvKey` 自 provider_config.cpp 匿名命名空间提升为公开判定（F-10），`claude_ui_detail::isModelMappingEnvKey` 改转发；`envSourceLabel` 提升公开（§8.5 compose 装配复用，单一来源）。
- 过渡态口径：v11 DROP 后、§8 UI 切换前，片段 JSON 校验经 parseExtraEnv 的过渡分支已在 §8.1 落地时切换为 validateExtraSettingsJson（repo 与 UI 同一套规则）。
- 测试拆除面（§8.4 四组）：`sync_build_proxy_env_shared_config`/`sync_takeover_with_shared_config` 重写为「域搬迁不残留双通道」钉；`sync_service_g10_frozen_key_not_captured` 删（G10 死语义）；`sync_service_shared_config_delete_retakeover` 重写为 JSON 域删片段回落（顺带钉陈旧非 env 键删除）；`claude_shared_config_*` 四处 sharedEnvKeysFor 断言拆、绑定 DB 往返保留；`provider_config_guard_G10_shared_config_key` 删。

## 6. 遗留项

- 上层覆盖检测扩非 env 键：按裁决④登记不修（§5.2⑦ env 键域维持）。
- `screencapture`/真剪贴板人工核验：照旧留真机验收（probe `--dump` 不写真剪贴板）。
- Windows 侧（MSYS2 sqlite ≥3.45）v11 DROP COLUMN 未在真机复跑——本机 macOS 实测通过；两端平台 SQLite 均满足 ≥3.35 下限（§11 首日动作，回归风险登记）。
- 未合并 master（本任务边界 = 分支交付；合并与 `/Applications` 覆盖安装按流程由主会话执行）。
