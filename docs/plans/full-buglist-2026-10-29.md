# 全模块回归 BUG 清单（2026-10-29 22:3x 第 3 轮收口）

- 触发：老板令「所有功能模块都要测，给出 BUG 清单」
- 范围：全部 8 个 Router 模块（首页/笔记/待办/Claude/智能体/壁纸/设置/关于）+ core/platform + 守卫
- 命令与结果：
  ① `bash scripts/build-test-run.sh --no-run`（job bash-3bb2e7c6a83944b4，exit 0）：单测直跑复核 Total 1957 / Failed 0
  ② `ctest -L probe`：22/22 通过，5 条 wallpaper_* 设计内跳过（macOS 壁纸二阶段未落地，非 BUG）
  ③ 守卫段全绿（模块隔离 / Windows 交叉编译 / std include / 玻璃体系 / ToolGuard 及各自 self-test）
- 结论：**新回归 0 条**；开放项 2 条（1 flake 嫌疑 + 1 观察缝缺口），待派；另有 3 条测试缺口 QA 自留

## 待派（交老板娘分配）

| 编号 | 现象与证据 | 定性 | 严重度 | 建议归属 |
|---|---|---|---|---|
| BUG-7 | agent_e2e_probe 首跑失败：输出停在「密文搬运完成（scratch 库激活 profile 已指向生产上游）」后 Subprocess terminated（32.6s，真网关调用段）；随后复跑两连绿（97.3s / 通过） | 真网探针稳定性（环境/网络敏感 flake），非功能回归（复跑绿、单测 1957 全绿佐证） | 低 | 智能体后端（辛勤码农/元气小萝莉）+ QA 配合定性：探针加重试与失败段日志 |
| BUG-8 | 归档折叠动画「动画期重复触发禁止」无外部观察缝，无法断言（todo_module 私有状态机） | 测试缺口（需开发开观察缝/测试钩子），非功能缺陷 | 低 | todo 前端归属（jQuery）开钩子，QA 补断言 |

## QA 自留（不派）

| 编号 | 项 | 处置 |
|---|---|---|
| GAP-1 | 跨模块 action:new_task 落待办同口径（B4）无用例 | QA 补 |
| GAP-2 | 归档窄条 Enter/Space 切换（C2 部分） | QA 补 |
| GAP-3 | 归档展开 20 条上限断言（C5 部分） | QA 补 |

## 观察条目（bug 池）

| 编号 | 现象 | 定性 | 优先级 | 状态 |
|---|---|---|---|---|
| BUG-9 | 「agent_ui: 运行中停止钮」全量负载下偶发红（惨品报 1957/1950/1 那轮），定向单跑绿（2026-10-30 00:1x QA 复核绿，57ms） | 负载敏感 flake（复现条件=全量负载下） | P2 | 观察：连续 2 轮全量不触发即关单 |

## 已知 flake（本轮未触发，记录）

- `md_preview_highlight` / `md_highlight` <8ms 墙钟断言（负载敏感）
- `test_claude_ui` 固定端口 28888
- `agent_e2e_probe`（见 BUG-7，本轮首跑触发）
