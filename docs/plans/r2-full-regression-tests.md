# R2 收口全量回归测试清单卡

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
