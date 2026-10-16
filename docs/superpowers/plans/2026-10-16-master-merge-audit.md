# 审计报告：merge/master-into-dev（4e6e6fc + ab82e92）

被审树 `/Users/coderstory/dev/gui/wt-master-merge`，HEAD ab82e92，diff 基线 1d5696a。

## A. 语义完整性

**A1 schema.h 终态 — 迁移逐条核对（PASS，零漏折）**

| master 迁移 | 效果 | 终树 DDL 对应物 | 结论 |
|---|---|---|---|
| v1 claude_profiles 8 列 | api_key_encrypted/extra_settings_json/models_config/extra_env/shared_config_id/auth_field/dialect_override/fallback_profile_id | 全在 CREATE TABLE（dev 已有） | ✓ |
| v1 claude_shared_configs.extra_settings_json | 补列 | 在 CREATE TABLE | ✓ |
| v1 claude_call_logs 5 usage 列 | input/output/cache_read/cache_write_tokens、first_event_ms | 全在 CREATE TABLE | ✓ |
| v2 todo_tasks.archived_at+回填 | 列+存量回填 | 列在 schema.h:72；回填仅涉旧行，新库无对应物 | ✓ |
| v3 claude_profiles 表重建（keychain_account 除约束）+idx_claude_active | 仅涉带遗留列旧库 | 新库无 keychain_account（=0 命中）；17 列与重建表一致；idx_claude_active 在 :326；头注释明示仅涉旧库 | ✓ |
| v4 DELETE pet_config% | 数据清理 | 纯 DML 无 DDL 对应物；头注释明示 | ✓ |

schema.h diff dev→final 仅 +archived_at +注释改写，无删列。migrations.{cpp,h}/test_migrations 零残留，user_version 零命中，database.cpp migrate() 纯 applySchema。

**A2 settings_module.cpp PASS**：dev sync 符号逐数相等（githubSync 251=251、sync 总数 449=449、firstSync/throttle/kDbImported/SyncStatus 全在位）；master quotaEnabled 接线 :215/:408/:413；pet 卡片/回调删净。

**A3 main.cpp PASS**：pet 启动删净（剩 1 行说明性注释，:602）；db_backup 启动备份留；single_instance :500 接线；folder_picker 由 claude_module 导入弹窗接线（main 不需要）。

**A4 claude_module.cpp PASS（1 项低危）**：POST /v1/messages 探测 :995；db-import 暂停/恢复订阅 :336-348。合并新增的 end handler 总开关门控（:344-345 `&& isQuotaFeatureEnabled()`）：逻辑正确——开关关闭时导入结束不复活刷新，compose 翻转检测兜底；`startQuotaRefresh` 幂等（:799 `if (quotaRefresher_) return`），导入中途开开关的双启动安全；onStop 先关原子闸再退订，迟到事件安全。**低危**：门控行为无直接单测（test_github_sync_card 仅覆盖 store 层事件序 begin/end/imported，不触及开关门控）。

**A5 markdown_module.cpp PASS**：NoteGitAutoCommit onStart/onStop/onFileSaved 全接线；kMdSaved 与 git 钩子共存 :2950-2955；「已打开」toast 已删（showToast 21→20）且事件链未踩；newItemTargetDir_/contentKey 变更经核实在 master 侧（479f5fe 有 9/3 命中），非合并夹带。

**A6 CMake PASS**：无 pet/migration 编译单元（命中全是注释）；single_instance×3/folder_picker×3/exec_capture/git_error_dialog/md_note_git 均已登记；无注释掉的源文件行；构建过=无死引用实证。

## B. 测试完整性

**B7 差额精确闭合 PASS**：macOS 计 −44(pet) −2(settings：删 8 pet 增 6) −1(assembly) −7(menu_beautify，全 pet) +8(theme) +10(note_git) +1(md) +1(md_service) +4(todo_service) +4(todo_ui) = **−20**，与 1430→1410 精确吻合。claude_service +3 与 single_instance +1 在 `#if defined(_WIN32)` 块内（macOS 注册 0），Windows 口径净 −16。无 dev 用例被吞。

**B8 fix ab82e92 PASS**：真因=master 删 SettingsCardsIn 三个 pet 字段，dev 夹具按旧布局位序初始化→-Werror 编译红。修法=删夹具死成员并对齐新布局头三引用字段（已核 struct 字段序 {cfg, proxyPortDraft, logTail}），断言零改动。真修非凑绿。

**B9 抽查 PASS**：test_md_note_git 10 用例/21 断言、test_theme_accents 8/53、single_instance 1/4（win 门控有注释说明）、quota 开关 3 用例在 test_settings（default_off/bridge_flag/data_preserved）。均非空壳。

## C. 卫生

**C10 PASS**：合并自身改动（23 文件 combined diff）全为已记录裁决的冲突解法（注释换名、database migrate、settings 卡高、CMake 登记）；ab82e92 仅 6 行夹具。无夹带私货。
**C11 PASS**：审计前后 `git status` 均干净。

## D. 实跑

**D12 PASS**：终树新跑 `bash scripts/build-test-run.sh --no-run` → **Total: 1410 / Passed: 1404 / Failed: 0**，与票面一致；差 6=平台 DOCTEST_SKIP。test_claude_service.cpp:2991 未红。

## Findings 汇总

| # | 位置 | 问题 | 严重度 |
|---|---|---|---|
| 1 | claude_module.cpp:344-345 | 合并新增的 end handler 开关门控无直接单测（逻辑审查正确+幂等兜底） | 低 |
| 2 | scripts/check-module-isolation.sh:11-14,206,210 | pet 关键词 5 处命中，全为守卫脚本注释与自测夹具字符串，无功能残留 | info |
| 3 | main.cpp:602、CMakeLists.txt:167 | pet 摘除说明注释保留（非代码） | info |

**总裁决：放行**（低危 #1 建议后续补一条开关门控单测，不阻塞合并）。
