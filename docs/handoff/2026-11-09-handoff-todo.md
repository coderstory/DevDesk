# DevDesk 交接卡 · 2026-11-09（另一台电脑开工用这份）

> 生成：2026-11-09，Mac 侧会话收尾。HEAD = `be7babd8`（已推 `origin/dev`）。
> 池台账是**状态唯一真相源**（`docs/pool/pool.db`，走 devflow pool 工具读写，别手改库）；本卡是给人读的一次性快照 + 开工顺序。

---

## 〇、开工三步（新机先做这些）

```sh
cd <DevDesk 仓库>
git fetch --all --prune && git pull --ff-only     # dev ← origin/dev（HEAD 应为 be7babd8 或更新）
git submodule update --init                        # EUI-NEO ← fork/main
bash scripts/build-test-run.sh                     # 首次完整档（会 configure，Windows 上约 10-20 分钟）
```

- **构建唯一入口** `bash scripts/build-test-run.sh`；开发内循环加 `--dev --filter <词>`；收口/QA/发版前**必须**不带 `--dev` 跑一次完整档。
- Windows 上若 `build/` 是从别的平台拷来的或报 `CMAKE_BUILD_TYPE` 不符 → `rm -rf build` 重建。
- 当前基线（Mac 实测 2026-11-09）：doctest `Total: 2293 Passed: 2284 Failed: 0`、probe 46/46、守卫链 5 道自检全过。**Windows 侧未实测本批**（见 §四）。

---

## 一、本批已交付（Mac 侧，全部已推远端、池内已关单）

| 池号 | 内容 | commit | 备注 |
|---|---|---|---|
| bug248 | 终端 rowSliceText 宽字占位格写 NUL → 中文连续匹配失效 | 89e3a08b | 上批交付 |
| need241/debt226 | gguf 嵌入模型内置随包 | 42f01792 | 上批交付 |
| bug243 | DMG 图标 + 排版 | 2dbb0389 | 上批交付 |
| need242/debt126 | ui_driver 终端页输入注入（type/key 焦点路由 + app 级键兜底 + 拖选复制场景） | 2d22980c | 场景 `terminal-input.json` 14 步 |
| bug245 | 终端拖选行号恒偏 +40px → Router 补 `pageOriginY` 缝，两轴统一页面坐标换算 | cffffb72 | 场景断言 `sel.0` 实证 |
| need244 | MCP 详情页「安装到 Claude Code」按钮（幂等安装/卸载 ~/.claude.json） | 0c302d99 | 审计 P1×5 已修；P2 残余挂 debt250 |
| bug246 | 模块默认清单内容自校验（版本号到位也补回缺模块）+ 用户关闭豁免集 | 3ca3db8d | developer 交付，真库自愈实证 |
| bug249 | memory 登记迁移行（bug246 同型残留） | be7babd8 | 漂移机器启动自愈补回记忆模块 |
| need247 | 终端真跑 claude 验证（16 步场景，回答上屏） | 场景资产已在 89e3a08b | 证据 `evidence/2026-11-08-terminal-claude-verify/` |

---

## 二、待办全清单（open 43 条，按"能不能马上开工"分档）

> 全部取自池台账 2026-11-09 快照。**开工前先 `pool_trace` / `pool_search` 查重**，状态以台账为准。

### A. 待验收 23 条 —— 最该先做（都已有产物，只缺验收）

**A1 机器面验收可直接做（有新机就能验，无需老板在场）**

| 池号 | P | 内容 | 验收要点 |
|---|---|---|---|
| #234 | P1 | build-test-run.sh probe 跳过双验假绿缺口 | 修已在 c6f6634c；核对 `PROBE_FIT_FAM_HAS_MEMBER` 判红分支在场 + 隔离脚本复现 |
| #239 | P1 | mac 包主二进制硬链 Homebrew openssl（发布阻断） | 打包后 `otool -L` 主二进制无 `/opt/homebrew/` 前缀 + 干净环境启动 |
| #240 | P2 | probe 两处稳定性红（datepicker/果冻） | 修已在 c0ff79c1；完整档连续两次 probe 全绿 |
| #205 | P3 | build-test-run.sh 加 `--probes-on` | 核对开关 + 守卫链仍过 |
| #237 | P3 | EUI `FT_Property_Set` 返回值检查 | 改的是 submodule，核对 EUI 单文件 + 跨仓推送顺序 |
| #238 | P3 | THIRD-PARTY-NOTICES 补 4 组件 | 逐条对仓库证据，格式与既有 5 条一致 |
| #241 | P3 | gguf 模型内置随包 | 打包后 `assets/models/` 有 gguf + 日志定位源=pkg（Windows 侧也须验） |
| #235 | P2 | need219 规范三处漏收编 | 前两处纯字重改值，第三处**需先裁决口径**（玻璃三件套） |
| #236 | P2 | claude_proxy 两处残留（徽标压字/env 行卡偏上） | S 级几何，亮暗两态看图 |
| #243 | P2 | DMG 图标 + 排版 | 打包后开 DMG 目检（**你的手测项**） |
| #248 | P2 | 终端中文 NUL bug | 单测 `terminalRowSliceTextWideSpacer` 绿 |

**A2 需要真机/老板在场（观感与手测）**

| 池号 | P | 内容 | 谁来 |
|---|---|---|---|
| #156 | P1 | 全站弹窗样式统一（批B 完成） | 目检全站弹窗 |
| #159 | P1 | 记忆模块全链路 E2E 实测 | 需真引擎+真实会话素材 |
| #173 | P1 | 定时任务模块整体重做 | L 级，多轮验收 |
| #155 | P2 | Claude 代理卡片重设计 | 目检 |
| #165 | P2 | MD 弹窗背景变灰（遮罩预期？） | 目检定性 |
| #181 | P2 | 记忆三 TAB 信息分层 | 目检 |
| #183 | P2 | 记忆审核 UI 长文本 | 目检 |
| #192 | P2 | 上线任务编辑详情页改造 | 目检+拖拽手感 |
| #174/#175/#176 | P3 | Profiles 三项 UI | 目检 |

> 手测清单在 `docs/superpowers/plans/qa-20261107-batch-manual.md`（X-1~X-8 段）。

### B. 在池 7 条 —— 可直接开工的活

| 池号 | P | 内容 | 说明 |
|---|---|---|---|
| #211 | P2 | CI Windows 通知用例红 | **需先裁决产品口径**（改产品重试 or 接受） |
| #228 | P3 | Windows 流质泵无 occlusion 检测 | 平台不对称；候选 DWM `DWMWA_CLOAKED`，需定优先级 |
| #231 | P3 | Nacos 查询能力缺口 | 需定方向：接 Nacos API or 降级静态清单 |
| #250 | P3 | need244 审计残余批 P2×9+P3×1 | 清单卡 `docs/superpowers/plans/need244-claude-install-p2-residual.md`；下次碰 module_mcp 顺带清 |
| #51 | P3 | agent_ui DB 用例下沉 service 层 | 缓行，待大改 agent UI 时顺带 |
| #121 | P3 | CI msys2 UCRT64 迁移 | 重开条件：查清 setup-msys2 PATH 注入机制 |
| #209 | P3 | 本地 bundle 残留死字体 | 重打包自愈，登记备查不派工 |

### C. 挂起 13 条 —— 等条件/等环境

| 池号 | P | 内容 | 卡在哪 |
|---|---|---|---|
| **#140** | **P1** | **macOS 标题栏 40px：路线 A/B 待复核** | **等你拍板**（你原拍 B 自绘红绿灯；调研推荐 A 保留原生）——**最高优先级决策项** |
| #61 | P2 | Windows 实测 ui-probe-shot 路径 A | 需 Windows 机（新机正好做） |
| #117 | P2 | clean Windows CJK 度量 badge 超宽 | 同上，Windows 真机复核 |
| #16 | P2 | 壁纸线 terminate 尾巴 | 修复码在另一台 Windows 机未上行——**若新机就是那台，顺手上行** |
| #188 | P2 | Token 额度上限提高 | 待你指认出处（代码里查无此系统） |
| #213 | P2 | EUI retained layer 缩放+降频 | 执行已转 need#217（#217 已待验收，收口后本条随关） |
| #14 | P2 | agent_e2e 真网探针 flake | 登记不派工 |
| #34/#35 | P3 | 向量模型升级档 / 全文通道升级 | 条件未触发 |
| #46/#47 | P3 | todo 自动路径守卫缺口 / anim_seam flake | 登记不派工 |
| #48 | P3 | 记忆解析层两条 QA 备注 | 碰记忆解析层时顺带 |
| #56 | P3 | 发布任务全屏编辑层 | 等池 53 落地 |

---

## 三、有裁决权/待拍板的事（AI 别自己动）

1. **#140 macOS 标题栏路线 A/B** —— 老板原拍 B（自绘、放弃原生红绿灯），调研推荐 A。复核前不动工。
2. **#235 第三处**（mcp 卡玻璃三件套）—— 规范 §5.2-1 与 §6-C 口径矛盾，需先裁决。
3. **#211** CI Windows 通知用例 —— 改产品重试（行为变更）还是接受现状。
4. **#228 / #231** —— 优先级与方向（Windows occlusion；Nacos 查询）。
5. **#188** —— Token 额度上限到底指哪里。
6. **#250 残余 9 条** —— 逐条「修 or 明确不做」。

---

## 四、跨平台未验证面（Windows 机开工须知）

本批所有改动**只在 macOS arm64 实测**。Windows 上需重点复核：

| 项 | Windows 关注点 |
|---|---|
| need244 MCP 安装钮 | `paths_win.cpp` 的 `claudeConfigFile()` 走 `USERPROFILE`；`~/.claude.json` 写入路径与 UTF-8 编码；真机点一次安装/卸载 |
| bug245 拖选 | 指针事件 `PointerSpace` 契约 Windows 侧同源，但 DPI scale 分档不同——真机拖选复验行号对齐 |
| bug246/249 模块自愈 | core 层无平台代码，交叉编译已过；真机验证启动日志「已自校验补回」 |
| need242 driver | 终端 PTY 在 Windows 是 ConPTY，注入链可能不同——跑 `ui_driver_terminal-input` 场景 |
| #241 gguf 随包 | `package-windows.sh` 是否也拷了模型（本批只验了 mac 打包段） |
| #239 mac 包 openssl | mac 专项，Windows 无关 |

---

## 五、环境与工具提醒（新机）

- **构建**：一律 `bash scripts/build-test-run.sh`，禁止手敲 `cmake -B/--build`（有门禁硬阻断）。
- **平台**：Windows 用 MSYS2 MinGW64（`MINGW64`，不要 UCRT64——见 #121）；静态链 sqlite3/curl/ssl/crypto。
- **数据根**：Windows `%USERPROFILE%\DevDesk`，`DEVDESK_DATA_ROOT` 可覆盖。
- **测试**：`bash scripts/build-test-run.sh --dev --filter <词>`；`tests/doctest.h` 是自研 shim（无 `REQUIRE_FALSE`/`MESSAGE`，仅报首个失败）。
- **ui_driver**：`cd build/tests && ./devdesk_ui_driver --script ../../tests/ui_driver/scenes/<名>.json`（离屏窗，不抢焦点）；`.exe` 补全在 Windows 上需注意（见 #61）。
- **跨仓推送顺序**：先推 `EUI-NEO` → fork，再推 DevDesk → origin（顺序反了 CI 红）。
- **提交前**：`git status` 确认不含他人未提交改动；长任务用 `wip:` 检查点，收尾 squash。
- **汇报纪律**：做完再一次性汇报（编号+一句话结论），细节写清单卡文件给路径。

---

## 六、本卡未覆盖的（自己查）

- 池内已关闭的 195 条历史与证据 → `pool_trace <id>`
- 各需求 PRD/设计案 → `docs/superpowers/specs/`、`docs/design/`
- 架构/构建/平台约束 → 仓库根 `AGENTS.md`、`CLAUDE.md`
- 最近一次夜间交接（另一份参考）→ `docs/handoff/2026-11-07-nightshift-handoff.md`
