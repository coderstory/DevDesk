# 审计 C1/C2 处置台账（2026-10-28 release 批次 batch34）

本台账补全审计 C1（`2026-10-28-release-audit-C1-batch34-logic.md`，2 major + 7 minor）与 C2（`2026-10-28-release-audit-C2-batch34-ui.md`，0 blocker + 2 major + 7 minor）共 18 条 finding 的逐条处置结论（交付纪律阶段 3 留痕）；证据 sha 均经 `git show --stat` 核实存在且改动文件与描述相符。

| 编号 | 级别 | 一句话内容 | 处置 | 证据 sha | 备注 |
|------|------|-----------|------|----------|------|
| C1 F-1 | Major | 测试时钟口径与实现不一致：deriveStatus 用例非 UTC 时区必红、「恰好等于基准」边界未覆盖 | 已修 | `52f68391` | helper 改本地 mktime 同口径 + 反重构警告 |
| C1 F-2 | Major | 唯一索引对存量脏库「启动即硬崩」无迁移守卫，schema 注释与实际行为相反 | 已修 | `52f68391` | 四条幂等去重 DELETE 前置于建索引，反义注释修正 |
| C1 F-3 | Minor | 写入口校验缺失：外键违约与非法输入仍以 Internal 冒出 | 收窄修复 | `bd11fc52` | isValidIsoDate 公开化、planDate/分钟范围/action 白名单落地；父行存在性预检缓置（UI 层 findTaskByDate 预检 + 外键 CASCADE 覆盖现有全部可达路径，导入/脚本入口出现时再补） |
| C1 F-4 | Minor | ticker 异常保护无任何测试 | 已修 | `749aeec7` | 与 F-5/F-6 同批三条缺口用例 |
| C1 F-5 | Minor | listTaskStepsByTask（两级 JOIN）无直接测试 | 已修 | `749aeec7` | 同上 |
| C1 F-6 | Minor | setStepDone 幻影行守卫无测试 | 已修 | `749aeec7` | 同上 |
| C1 F-7 | Minor | transaction() txDepth 非 RAII：fn 抛异常后线程永久失去事务语义 | 已修 | `749aeec7` | TxDepthGuard RAII 守卫 |
| C1 F-8 | Minor | createTask/upsertTaskConfig 路径缺唯一冲突映射 | 已修 | `bd11fc52` | — |
| C1 F-9 | Minor | setStepDone 契约未声明「从未置位过的步骤撤回不补行」 | 已修 | `bd11fc52` | — |
| C2 M1 | Major | 自动撤回链零自动化断言（spec §5.3 核心契约） | 已修 | `d1b34740` | M1-a/M1-b 两条用例 |
| C2 M2 | Major | 提醒窗调度/抑制/渲染零自动化断言 | 已修 | `8126eaa9` | 纯决策函数抽 release_reminder_logic.* + 6 条无头用例；窗口/句柄耦合块（openReminder/closeForEmpty/collectReminderView/buildDemoView/绘制）未抽取留原实现（与 OS 窗口生命周期耦合，硬抽会改行为） |
| C2 m1 | Minor | applyRemoveService/applyRemoveEnv 无 readOnly 守卫 | 已修 | `aa0adf6b` | readOnly 守卫 |
| C2 m2 | Minor | void/restore 状态变迁不记日志（spec §9 词汇未落地） | 已修 | `aa0adf6b` | void+restore 日志 |
| C2 m3 | Minor | 编辑层保存失败即关层、丢弃草稿 | 已修 | `aa0adf6b` | 保存失败保草稿 |
| C2 m4 | Minor | applyRestore 不重置 statusWriteFailed | 已修 | `aa0adf6b` | statusWriteFailed 重置 |
| C2 m5 | Minor | 只读态打开配置编辑层仍会补建缺行 | 已修 | `aa0adf6b` | 只读跳过补建 |
| C2 m6 | Minor | 列表页卡片装配每卡 N+1 查询 | 已修 | `bd11fc52` | listTaskServicesByTask/listTaskConfigsByTask 归并，每任务 4 条 SQL |
| C2 m7 | Minor | 字数统计/失败路径/Esc 分层三处断言缺口 | 已修 | `d1b34740` | 7 条新用例覆盖全部三项 |
| A-02 | 关联 | 列表与详情进度分母口径不一致 | 不修（仅记录） | — | 待用户拍板；非本批引入 |
| 调度文档 | 关联 | 调度 handoff/research 两文档合并取侧 | 不修（仅记录） | `76aebdde` | 合并中按 delta 规则取 dev 侧（路径仓库相对化），归调度线所有 |

## 未验证声明

本处置批全部改动（52f68391/749aeec7/bd11fc52/aa0adf6b/8126eaa9/d1b34740/76aebdde）**无编译与测试证据**——本机按用户指令一律不执行编译测试；CI 验证环节经用户指示取消；该批将以"未验证"状态合入 dev，首个实测点为后续任何一次构建。
