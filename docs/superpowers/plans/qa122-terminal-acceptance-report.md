# QA 验收报告：终端模块 v1（need:122）

> 执行人：qa-tester · 日期：2026-11-04 · 预算 45 次工具调用（实耗 45）
> 测试清单卡（F-ID / 流程 / 证据物 / 结果）。证据暂存 `/tmp/qa122-out/`（**预算耗尽未及归档 evidence/，建议主会话收尾时 cp 到 `evidence/qa122-terminal-v1/`**）；QA 场景文件 `tests/ui_driver/scenes-dev/qa122-*.json`（6 份）；QA 基建小修 `tests/ui_driver/ui_driver_main.cpp`（全屏桥装配对齐真机 main.cpp:654，含收尾置空）。

## 总判定：**半成品→待手测补证**（探针侧主体绿；F-05 侧栏折叠 1 个待判定缺陷 + 键盘/拖选/IME 三项探针不可达归手测）

## 一、收口档全量（阶段档）

| 项 | 结果 | 证据 |
|---|---|---|
| doctest 全量 | **Total 2165 / Passed 2159 / Failed 0**（6 skipped），全绿 | `/tmp/qa122-fullrun.log:18` |
| UI probe | 29 项：唯一红 schedule_ui_probe（**bug:125 已知**，非本任务）；wallpaper 4 项本机跳过（平台能力不支持） | 同上 :22-54 |
| 守卫链 | 发布形态 darwin 降级警告（契约仅 Windows）；DEVDESK_STATIC MinGW 专有跳过——符合预期 | 同上 :16 |
| bundle | build/DevDesk.app 与构建同步（含终端模块） | 同上 :12 |

## 二、逐项验收

| 项 | 动作 | 结论 | 证据 |
|---|---|---|---|
| F-01 入口 | ui_driver 真点侧边栏「终端」 | **通过**：条目在场（interactive）+真点导航命中回读 | qa122-recon/exit dump：`app.sidebar.item.Terminal.hit 16,410 188x40`；多次场景 [1/…] PASS |
| F-02 会话/渲染 | 进页自动起 zsh；提示符上屏；**reflow：800 窗 grid 行宽 594px vs 1000 窗 1004px** | **渲染/输出半边通过**；**键盘输入半边探针未证**（driver type/key 均进不了 PTY，见 BUG-2）→ 命令交互归手测 | fail-5.png（提示符 coderstory@coderstory-air tests %）；qa122-reflow-dump.txt 行宽对比 |
| F-03 亮暗主题 | 亮态（默认主题）+暗态（seed themeMode=dark）各截图 | **通过（截图证据落盘）**：亮 qa122-term-light.png / 暗 qa122-term-dark.png（暗态场景 5/5 全绿退出码 0）。配色逐像素对照设计 §3.3 归视觉复核（截图已齐） | /tmp/qa122-out/qa122-term-{light,dark}.png |
| F-04 拖选复制 | drag row.0→row.2（物理隔帧链路） | **探针不可达**：drag 步骤死锁（driver 物理拖选与终端 pointer 捕获层不兼容）→ **归老板手测**（步骤见 §四） | C 场景 [3/7] PASS 后超时；pbpaste 空 |
| F-05 全屏 | 进全屏→悬浮钮→还原双向 | **主体通过，1 缺陷**：工具栏隐藏✓、悬浮退出钮在场可点✓（`app.term.exit.fab 28x28`）、还原后全屏钮+徽标回场✓（10/10 全绿双向可逆）；**侧栏未折叠**（桥 handler 已装配+module_terminal.cpp:73 调用点存在，探针侧 sidebar rect 全程 `0,30 220x970`）→ BUG-1 待真机判定 | qa122-fs.png / qa122-unfs.png / fs-report.json 10 PASS / qa122-fs-dump.txt |
| F-06 Windows 占位 | 本机 macOS 无法构建 | **归属收口**（Windows 机项） | — |
| F-07 exit/失败/残留 | exit 结束卡+新建；失败注入；pgrep | **失败态通过**：DEVDESK_TERMINAL_SHELL=/nonexistent → 失败覆盖卡（overlay.title/detail/retry 320x140）+「重试」文案在场，重试后不崩；**exit 结束态探针未证**（键盘半边 BUG-2 阻断，key 逐链执行但字符未达 PTY）→ 归手测；**无残留通过**：ui_driver 实例（含终端会话）正常退出后 `pgrep corta-exec` 零命中（三轮复验） | qa122-spawnfail.png / qa122-spawnfail-retry.png / spawnfail-dump.txt；三轮 pgrep 输出（见终报） |
| F-07 切页保活 | 未单独构造（预算约束） | **未验证**（长任务 sleep 10 切页往返）→ 归手测清单 | — |
| POC-4 GL 实帧 | 离屏探针无逐帧 instrumentation | **测不了，如实标注归收口说明**（CPU 侧 281.2µs/帧已证，技术卡 §10） | — |
| imeRect | 元素树实证 `app.term.ime.anchor` 在场（锚定矩形上报链路通） | 候选窗真实定位**归老板手测** | qa122-fs-dump.txt |

## 三、缺陷清单（挂池跟踪）

- **BUG-1（F-05，P1 待判定）**：全屏态侧栏未折叠。探针侧桥 handler 已装配（QA 小修对齐 main.cpp:654）、模块调用点在（module_terminal.cpp:73），sidebar rect 全程 220px 不变。根因二选一：探针宿主差异 / 产品缺陷——真机手测一步判定。
- **BUG-2（QA 基建债，非产品 bug）**：ui_driver 注入面与终端页不兼容——type 文本卡死（app.term.input 捕获层）、keyDown 链不达 PTY、drag 物理拖选死锁。键盘/拖选类验收在终端页探针不可达。建议 developer 侧补 driver 终端兼容（后续债务，不阻塞本收口的手测路线）。

## 四、归手测清单（老板真机 5 分钟）

1. **键盘命令**（F-02/F-04）：终端页敲 `echo hello`（回显 hello）、`ls --color`（彩色）、vim/less 方向键、中文文件名；**进页后不点终端区直接打字**顺带验证焦点易用性（PRD 流程第 2 步语义）。
2. **IME**（F-04）：切中文输入法，候选窗位置跟随光标（`app.term.ime.anchor` 链路已证在场），组合期不上屏不发字。
3. **拖选复制**（F-04）：拖选一段 `ls` 输出 → Cmd+C → 粘贴到别处对照一致。
4. **F-05 BUG-1 一锤定音**：真机点全屏——侧栏折叠是否发生。折叠不发生=BUG-1 实锤转 developer。
5. **exit 生命周期**（F-07）：敲 exit → 「会话已结束（退出码 N）」→ 新建会话可用；长任务 `sleep 10 && echo done` 切 Markdown 再切回验证保活不丢输出。
6. 退出 DevDesk 后终端复跑一次 `pgrep -f corta-exec`（探针侧已三轮零残留，真机补一次最终确认）。

## 五、结论

**完成度**：收口档全量绿（唯一红=已知 bug:125）；F-01/F-03/F-05（主体）/F-07（失败态+无残留）探针通过；F-02 渲染半边通过。**v1 达成判定待**：BUG-1 真机判定 + §四手测项补证（键盘半边与 IME 本就依赖真机输入法）。F-06 归 Windows 机收口。
