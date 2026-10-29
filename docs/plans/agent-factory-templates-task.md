# 任务书：智能体出厂任务模板 ×3（需求池 需-6，S 级）

mode: edit

## 目标
在 `src/module_agent/` 新增三个出厂任务模板，用户「新建任务」时可选模板一键生成任务（与既有 `kCronTemplates`（agent_module.h:86）cron 模板下拉同模式）：

1. **日报**：cron `0 18 * * *`，`git.log` 汇总当日 commit → `fs.write` 落笔记目录
2. **发版 changelog**：手动任务（无 cron），按 tag 区间 `git.log` + `git.diff` → 按模板出草稿
3. **测试环境巡检**：cron `0 9 * * *`，`bash.run` curl healthcheck，异常置顶提醒

## 硬约束
- 零新工具：只用现有 14 件内置工具（agent_builtin_tools）。
- 模块隔离：只动 module_agent；改完 `bash scripts/check-module-isolation.sh` 必须绿。
- 构建一律 `bash scripts/build-test-run.sh`，禁手敲 cmake；只跑定向用例（agent 子串）。
- worktree 隔离、原子提交、显式 pathspec、禁 `git add -A`。

## 验收标准（缺一不算完成）
- [ ] 新建任务弹窗可看到 3 个模板入口，选中后名称/cron/提示词预填且可改。
- [ ] 模板提示词文本以模块内常量定义，中文、零黑话。
- [ ] 定向单测：模板常量完整性（3 条、字段齐）+ 弹窗组合用例（选中回填）绿。
- [ ] 涉及可见 UI：probe 或无头几何断言覆盖。
- [ ] 模块隔离守卫绿。
- [ ] 回单三件套：分支 commit sha + 改动文件清单 + 「改了什么、为什么」说明；测试有先红后绿证据。

## 边界（非目标）
- 不做模板管理页（增删改模板）、不做模板市场、不动 core/platform/migrations。
- git.log/tag 区间参数由用户在生成的任务里手填，模板不自动探测仓库布局。

## 报告格式
极简：结论一行 + sha + 文件清单 + 测试数字（Total/Passed/Failed）+ 未验证项。