# B1 材质登记表落地（池 64）QA 验收清单卡

- 日期：2026-11-02　验收对象：wip commit 1a9b419f（HEAD）　结论：**通过**
- 验收执行：ZCode 主会话代验（前次 qa-tester 子代理 2026-11-02 13:38 因配额中断；恢复时用户两次婉拒子代理派发，授权主会话直做——流程偏差记录在案）
- 前次中断遗留产物已复用：qa/pages 12 页 + agent-task 三态图；本验收补齐探针裁决日志与像素判据

## 判据 × 结果 × 证据

| # | 判据（方案 §10.2） | 结果 | 证据 |
|---|---|---|---|
| 1 | 探针断言 A1 三态可辨（normal/hover/pressed 两两不同） | **PASS** | `probe-run.log`：normal=35,33,32 / hover=32,30,29 / pressed=30,28,27（暗色档） |
| 2 | 探针断言 A2 控制点不漂（次卡三态一致，状态不串卡） | **PASS** | `probe-run.log`：card2.inner=35,33,32 三态一致 |
| 3 | 探针断言 LAYER 分层生效（L1 containerSurface ≠ L2 itemSurface） | **PASS** | `probe-run.log`：panel.gap=31,29,28 ≠ card1.inner=35,33,32；页面底 26,24,22 < L1 < L2 三层梯度清晰 |
| 4 | 像素判据：卡内 vs 卡外采样有差（半透生效） | **PASS** | agent 页（qa vs before）卡区 228,248,237→216,245,229（绿分量透出）；release 页卡内 178,234,204 vs 卡外 217,243,229 |
| 5 | 回归：非改动页与改前一致 | **PASS** | 逐页 diff（TH=18, 1/4 采样）：非改动页差异 0~0.32%，与同版两采噪声地板（qa vs after：0~0.20%）同量级，无系统偏移=采集噪声（动态背景光环时移）；markdown 0.00%、todo 0.01% 基线不动 |
| 6 | 壁纸选择卡（wallpaper_ui.cpp:155） | **N/A（平台口径）** | wallpaper 模块按平台能力注册，macOS 不渲染该页（uat62 探针注释明示）；改动以编译通过+代码走查（wip 1a9b419f diff）计，Windows 侧待发布验证 |

## 数值附录（采样环境：uat62 离屏 1600x1000@2x，浅色档；b1_material_probe 1600x1000，暗色档）

- 探针 frame：card1(42,110,1516,68) card2(42,186,1516,68) panel(32,100,1536,868)
- rgb 矩阵（normal/hover/pressed × card1/card2/panel.gap/page.bg）见 `probe-run.log`
- 逐页回归表：01-home 0.00% / 02-markdown 0.00% / 03-todo 0.01% / 04-schedule 0.10% / 05-claude_proxy 0.00% / 06-agent 0.04% / 07-memory 0.02% / 09-mcp 0.14% / 11-release_tasks 0.16% / 12-release_config 0.00% / 13-settings 0.12% / 14-about 0.32%（对照噪声地板 qa-vs-after：06 0.00% / 09 0.04% / 13 0.05% / 14 0.20%）
- 工具：/tmp/pnglib.py 采样、b1_qa_scan{,2}.py（/tmp，本次自写）

## 悬空与移交

- 壁纸页 Windows 侧验证 → 记入发布检查（B6 后重验不含此项，平台不可达）
- 无新增 bug
