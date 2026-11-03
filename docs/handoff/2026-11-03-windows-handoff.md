# 交接文档：mac → Windows 开发机（2026-11-03 夜间批次）

> mac 侧（本会话）收工快照。Windows 机接手第一步：读本文档 + 按提示词开工。

## 1. 同步基线（先做）

```sh
git fetch --all --prune && git pull --ff-only     # dev → 23da514b 或更新
git submodule update --init                        # EUI-NEO → e3a3df3（本轮有 fork 新提交，必拉）
bash scripts/build-test-run.sh                     # 全量，预期 2116+ 例 0 红
```

- CI 状态：mac 侧收工时终轮在飞（head=23da514b），GitHub Actions 双平台；红绿用 `gh run list --branch dev` 查。此前已知绿证：run 41566653833（双平台 success 含 ASan 档）。
- EUI-NEO 跨仓纪律不变：先推 fork（本仓 origin 即 coderstory/EUI-NEO）再推 DevDesk。

## 2. 今日 mac 侧完成（commit 链，全部已推）

| commit | 内容 |
|---|---|
| 56247146 | need113/114 实测报告（上线+记忆模块） |
| fb8e0d35/ac1018d9/89a2a065 | 记忆批次修复（撤销链 trim/注入 threshold=0.4/pip 钉版/py 文案/按钮置灰）+ 头像口径 + 上线批次 B 五项打磨 |
| 426f5032…5e31eefb（CI 治理 ×5） | Windows CI 自 submodule 迁移起连红根治（EUI_NEO_DIR/GCC16 误报/flake×3/环境分叉×5/ASan 窗口） |
| 19487b35 | 需-3 记忆类型前缀合并（路②清单差分补标+注入分组渲染，POC 双实锤） |
| 09966fd8 | scripts/run-memory-e2e.sh（e2e 可回归） |
| a739e2c0 | 池49/110（todo 动画用例+home 图表留白；注意 test_ui_modern 柱高断言已随新几何更新） |
| 21dc50a3 | 池57/116 release_probe 补账（发布态绿染/空态/拥挤态断言） |
| ca478587 | 池106/107 同步 409/404 错误路径（空仓首传/自动建库，live 8/8+8/8） |
| 692f2aa1 | 池111/112 EUI onHover 消债（EUI e3a3df3 + schedule 接线） |
| f60c412c | 池120 agent_live 编译红修复 |
| c182a9a4 | 池109 字号缩放 198 处清零（A 67→2 / B 132→7，9 豁免见 evidence/2026-11-03-need109-fonscale/） |
| 671e47f0/231b38ec | CI msys2：全局 EUI_NEO_DIR（保留）；UCRT64 迁移两轮翻车已回退 MINGW64+消音（记池 121） |
| 23da514b | 池台账落账 |

## 3. Windows 机待办（优先级序）

1. **验收 need 113/114**（待验收态）：照 `docs/research/2026-11-03-windows-uat-checklist.md` §1/§2 走查，结论回池（过=关闭，或告诉 AI 关）。
2. **复核 bug 117**（挂起）：清单 §3 一条命令——`bash scripts/build-test-run.sh --dev --filter "font-em"`：绿=CI runner 字体栈特有，关；红=真缺陷（badge 超宽），立案修。
3. **need 61**（挂起，Windows 正好能做）：跑 `build/tests/uat62_page_shots_probe --size 1000x640 --out evidence/<日期>/`，核对 exe 直调/PNG≈逻辑尺寸/wallpaper 13 页/图标，回填 evidence 并删 skill 中「Windows 侧差异」待验标注（`.agents/skills/ui-probe-shot/SKILL.md`）。
4. 全量回归已在 §1 跑；若红把 `[FAIL]` 行带回池。
5. 记忆模块 Windows 实机过一遍（py 探测/引擎向导/评测出数——S2 曾验过，本轮改了文案+钉 telemem==1.10.1+threshold）。

## 4. 池状态快照（收工时）

- **待验收 ×2**：113/114（等老板验收）
- **挂起 ×10**：117/61（Windows 可动）+ 34/35/56/121/16（条件触发）+ 14/46/47/48（登记不派工）
- **在池 ×1**：51（成本权衡裁决缓行）
- 其余全部关闭。台账唯一真相源=docs/pool/pool.db（pool-report.html 可看）。

## 5. 注意事项 / 坑

- **PAT 红线**：同步 live 测试的 GitHub PAT 由老板提供（勿落盘/勿入库/勿进日志——测试时只走环境变量）。测试仓 `coderstory/private_notes` 已恢复空仓态可重演 409 场景；夹具仓 devdesk-sync-test 被误重建为 private（reset-main 可重置，老板已知）。
- **UCRT64**（池 121）：别直接切——PATH 传导机制未查清，两轮翻车记录在池。
- **过滤词盲区**：改 UI 几何后定向过滤要按「被改元素 id」反查全测试域（grep 元素 id tests/），防 test_ui_modern 类跨文件用例漏网（今日两次踩中，已沉淀 lessons）。
- mac 侧已知平台不对称红 ×3（FileWatcher×2/keychain×1）在 AGENTS 有案，Windows 不涉及。
- 后台代理并发上限是供应商压力动态值（今日实测 2~5 波动），别按固定值设计流程。

## 6. 产物索引

- 实测报告：docs/research/2026-11-03-{上线,记忆}模块实测报告.md
- Windows 验收清单：docs/research/2026-11-03-windows-uat-checklist.md
- CI 分叉扫描：docs/research/2026-11-03-win-ci-divergence-scan.md
- e2e 回归脚本：scripts/run-memory-e2e.sh（--keep 留现场）
- 证据总目录：evidence/2026-11-03-*/（git-ignore，本机可见）

## 7. Windows 侧 codegraph 接线（mac 已装 1.6.2 三件齐，Windows 需同装）

在 Windows 会话里跑 `/codegraph-setup` skill（devflow 插件自带），或手动等价三件：
1. `npm i -g @colbymchenry/codegraph`（取 `command -v codegraph` 绝对路径）；
2. config（`%USERPROFILE%\.zcode\cli\config.json`）→ `mcp.servers.codegraph = {"type":"stdio","command":"<绝对路径>","args":["serve","--mcp"]}`（json 读改写，禁拼接）；
3. prompt-hook：插件 `assets/codegraph-prompt-hook.sh` 填二进制路径落 `~/.zcode/hooks/`（chmod +x），config `hooks.enabled=true` + `UserPromptSubmit` 追加 command 条目（timeoutMs 10000）；
4. 项目索引：Windows 侧首次 `codegraph init`（项目根，.codegraph/ 勿提交）；skill 全流程幂等可重跑。
mac 侧收工前已补齐本机 hook 接线（此前只有 MCP+AGENTS.md），重启会话生效。
