# R3 定时规则编辑弹窗 定向回归清单卡（A1-A6）

- 日期：2026-10-29
- 被测：dev `29a61928`（R3 编辑弹窗接线：新建/编辑/删除入口 + 动态载荷 + 删除确认）
- 设计与验收清单：`docs/superpowers/specs/2026-10-29-schedule-edit-dialog-ui-design.md`（A1-A6 见 §表）
- 派单人：coderstory；实现：傲娇毒舌魔女

| F-ID | 流程（验收条目） | 验证方法与证据物 | 结果 |
|------|------------------|------------------|------|
| F-A1 | 列表「新建/编辑/启停/删除」四入口可见可用，系统段无操作控件 | 真窗 probe（schedule_ui_probe）+ 截图 | 绿 |
| F-A2 | run_task 下拉选 key，落库 JSON 键 `"key"`，action_runner 可消费 | 单测（调度域定向） | 绿（单测层；live 端到端见未验证口径） |
| F-A3 | 非法 spec 保存被拦、红字就地、不落库 | 单测（既有） | 绿 |
| F-A4 | 删除：默认软删、勾选走物理删；确认弹窗默认焦点取消 | 单测 + probe | 绿 |
| F-A5 | kind 切换保留字段；预览 5 次与引擎同源 | 单测（既有 previewNextFires） | 绿 |
| F-A6 | 「每 N 个周几」「每年 X 月 X 日」摘要正确（P2-6） | 单测 | 绿 |
| F-R3a | 新建默认时间为明天 9 点（本轮修复） | 单测/实现者证据复验 | 绿 |
| F-R3b | 回归行：schedule 域 86 条 + probe 层 | `--filter schedule` + `ctest -L probe` | 单测 86/86 绿；probe 20 绿 5 跳（设计内）+ **2 红（home_probes，非 R3 范围，见 BUG-2）** |

## 未验证口径（预登记）

- 真机人手点验（新建→编辑→暂停→软删→彻底删实点）属全功能点验清单（PM 持有），装机后 UAT 层做；我负责 probe 真窗 + 单测层回归。

## BUG 台账

- BUG-1：主窗无法最小化+置顶 → 已在他卡销账（`09d8aa62`）。
- BUG-2（新回归，非 R3 引入）：`home_probes` / `home_probes_min_window` 红——5 个统计卡 badge 结构缺失（`home.stat.*.badge`）+ 组 K(d) 滚动吸收 FAIL（内容高 524 > 视口却无滚动条）。**终态：误报，销单**——根因=主树有码农未提交的统计卡半成品，QA 在共享主树构建探到的是半成品（非 `30ad823c`）。coderstory 裁定统计卡挪独立 worktree、主树还原；复跑 probe 层 22/22 全绿（job `bash-3a9a8f35d5ec4b3e`，exit 0），销单。流程教训已由 coderstory 落规矩：构建型验证前主树必须干净。
