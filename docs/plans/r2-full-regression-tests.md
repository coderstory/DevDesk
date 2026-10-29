# R2 收口全量回归测试清单卡

## 第 2 轮（2026-10-29 19:2x 启动，R3 收口）

- 触发：mac P1 `3d59d076` 交叉审无 P0/P1，coderstory 发令（发令枪=魔女审查结论）
- 命令：`DEVDESK_JOBS=2 bash scripts/build-test-run.sh --no-run`（降载规矩：老板无风扇 MacBook Air 易降频；后台 job `bash-20effb5bf57046ce`）
- 预期：单测 ≥1944 条 0 失败（上轮 1944 为基线）；probe 22 条全绿（5 wallpaper_* 设计内跳过）；守卫全绿
- 已知 flake（不算新红）：`md_preview_highlight`/`md_highlight` <8ms 墙钟、`test_claude_ui` 端口 28888
- 结果：**5 红（第 2 轮，2026-10-29 19:4x，job `bash-20effb5bf57046ce` exit 1）**
  - `Total: 1954 Passed: 1943 Failed: 5`（比上轮 +10 条，来自 mac P1 `3d59d076` agent 域新增用例）
  - 基线 `3d59d076`，主树 src 干净（仅 TODO.md/docs 记账未提交）→ 排除半成品污染
  - 5 条均稳定复现（按用例名单独复跑逐条红），定性**新回归嫌疑，非 flake 非环境**
  - BUG-3：badge 断言 3 条——`test_home.cpp:262`（notesBadge）/`:388`/`:489` 仍断言 `.badge` 元素存在，统计卡瘦身 `5694df44` 把 badge 并入指标名同行后单测未同步；**码农自验「home 12/12」只覆盖名字含 home 的用例，未覆盖这 3 条用例名（Token 速率卡片/实底统计卡/HomeService）= 定向漏测实证**
  - BUG-4：`test_ui_modern.cpp:884` 弹性双档高度断言红（bar8 h=243.67 ≠ kElasticPlotH），瘦身后布局变化未同步
  - BUG-5：`test_mcp_settings.cpp:215` mcpPort 往返断言 rows.size()==19 红（实测 ≠19），独立于统计卡，归属待查
  - 处置：已上报 coderstory 派修；修复后 QA 复跑定向 + 全量补绿后本轮才可销
  - **销账（2026-10-29 20:1x）**：修复 commit `73ffb0d4`（worktree wt-bug345）→ 合 dev `8f3a1e17`。QA 验证：①定向 5 域（Token 速率/实底统计卡/HomeService/uiModern/mcpPort）全绿——注意首跑直跑旧二进制仍红，取证前必须先增量构建；②全量复跑 `DEVDESK_JOBS=2 bash scripts/build-test-run.sh --no-run` exit 0（job `bash-463d19fea02f4766`），直跑二进制复核 `Total: 1954`、0 FAIL。**第 2 轮销账，BUG-3/4/5 一并关单**。

---

# 历史：第 1 轮（R2 双真源清理后收口）

- 日期：2026-10-29
- 触发：R2 双真源清理落盘后第一次全量回归（派单人：coderstory）
- 命令：`bash scripts/build-test-run.sh --no-run`（后台 job `bash-b73128d937bf4484`）
- 预期：≥1943 条、0 失败
- 已知 flake（不算新红）：`md_preview_highlight`/`md_highlight` 的 <8ms 墙钟断言（负载敏感）、`test_claude_ui` 固定端口 28888 冲突

| F-ID | 流程 | 验证方法与证据物 | 结果 |
|------|------|------------------|------|
| F-R1 | 全量构建（增量）+ 全量测试 | 脚本输出 `Total/Passed/Failed` 行 + job 输出留档 | 绿 |
| F-R2 | 失败分类（如红） | 失败四分类：新回归（报 coderstory 派修）/ 已知红（记录）/ 环境（建议）/ 测试缺陷（证据） | 不适用（0 失败） |

## 结果记录（2026-10-29 13:51 收口，job `bash-b73128d937bf4484`，exit 0）

- **单测**：`Total: 1944 Passed: 1938 Failed: 0` ✅（预期 ≥1943 达标；Passed 与 Total 差 6 条为条件跳过口径，失败 0 为硬判据）
- **UI probe**：ctest 21 条 100% 通过；5/21 设计内跳过（wallpaper_* 家族，macOS 壁纸二阶段未落地），跳过登记守卫通过
- **守卫段全绿**：模块隔离 / Windows 交叉编译（366/371 编过，5 个缺 curl 头跳过=未验证，如实记录）/ std include（684 文件）/ 玻璃体系 / ToolGuard；5 道守卫自检好树绿坏树红非恒真
- **已知 flake**：本轮未触发（md_highlight 墙钟、28888 端口均绿）
- **构建**：增量构建成功，版本号自动 bump 0.5.0 → 0.5.1
- **总判定**：完成——R2 落盘后全量回归 0 失败，无新回归、无已知红

## 跟踪项核销（2026-10-29 14:40 前后）

| 项 | 内容 | 终态 |
|----|------|------|
| retryWithoutCharge 覆盖缺口 | 前端 R2 审查 P2 发现无直接断言；后端 commit `1d3e342c` 补守卫用例（渠道并发满→retryWithoutCharge 置位且不落 delivery 记账行），并顺带修出真 BUG：`ChannelRegistry::deliver` 非递归锁回调内再入 registry 死锁 → 改 recursive_mutex（先红后绿证据链） | **已修复，回归绿**——QA 亲自复跑调度域定向 `Total: 85 Passed: 85 Failed: 0`（job `bash-4c9c0765965c469f`，exit 0），销单 |
| R3 回归（A1-A6） | 等前端实现落地后出定向清单卡再排 | 挂起 |
