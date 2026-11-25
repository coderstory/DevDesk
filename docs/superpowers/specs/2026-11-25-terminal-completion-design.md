# 终端能力完善设计（2026-11-25）

> 输入：《终端功能审计报告-2026-11-25》《DevDesk-终端功能完善分析报告-2026-11-25》（均为 AI 产出，未经人工审计）
> 本设计只覆盖**桌面 app + PC 网页端 + 手机网页端**三面的终端能力补全，不含终端内核替换。
> 结论口径：本文件里的事实断言分「已核（勘察证据）」「待核（进核验卡）」两类，未标注的不得当作事实使用。

---

## 一、意图与目标

终端当前「能开、能用」，但离「桌面主力」和「手机应急跑 Claude Code」两个定位都差一层：仿真缺口（鼠标上报、OSC、bell）、交互效率（选择、快捷键、右键）、生命周期兜底（退出即杀）、网页端交互短板（键带）。

**本设计的目标**：把两份审计里所有 ROI 达标的项，按三端分别排成可执行的需求序列，并明确每一端做什么、不做什么、怎么验。

**成功判据**
1. `vim` / `tmux` / `htop` / `claude` 在终端里鼠标可点可滚，配色正确；
2. 不记快捷键也能完成复制、粘贴、搜索、调字号；
3. 多项目可一键选目录开标签，桌面标签可改名、可排序；
4. 误关窗口/误关运行中标签有拦截；
5. 手机通过网页键带能跑完一轮 Claude Code 交互（输入、Ctrl+C、历史、Tab 补全、复制输出）；
6. 池台账无「代码已实现仍在池」的项。

---

## 二、非目标与反目标

| 类别 | 项 | 理由 |
|---|---|---|
| 不做·产品 | 分屏（横/纵） | 老板裁决：不做 |
| 不做（长期） | 会话恢复、shell integration、图片协议（sixel/kitty）、连字、读屏（app 端） | 成本 L 且与网格模型/框架冲突 |
| 不做（三端） | urxvt/1015 老鼠标协议、中键粘贴 | 收益低、mac 无法真机验 |
| 不做（按端） | 见第四节矩阵，不再重复 | — |
| 反目标 | 「为对齐而把网页做成 app 复刻」 | 网页端是同一会话的轻量入口 |
| 反目标 | 「文档说缺就开做」 | 已发现文档漂移（#450/#413 代码已有），一律先核验 |

---

## 三、已核事实（勘察证据，2026-11-25）

1. **libvterm 已内置鼠标上报**：`third_party/libvterm` v0.3.3-14（`934bc2f`）；`vterm_mouse_move/button` 声明 `include/vterm.h:374-375`，SGR 编码 `src/mouse.c:39-42`，DECSET 1000/1002/1003 解析 `src/state.c:821-829`、1006 `:840-842`。**缺的是封装层入口与模式状态，不是底层能力** → 鼠标上报成本由 L 降为 M。
2. **VtermCore 无鼠标入口**：`terminal_vterm_core.h` 仅 `feed/resize/snapshot/scrollback/selectionText/bracketedPasteEnabled`，无 mouse 方法；后端接口 `terminal_backend.h:46` 只有字节写。
3. **备屏状态不可查**：`altscreen` 仅内部字段 `terminal_vterm_core.cpp:109`，无 getter、无其他读取点；滚轮路由需要补只读访问器。
4. **bell 未接**：回调表 `/* bell */ nullptr` `terminal_vterm_core.cpp:147`，且未注册 `VTermStateCallbacks`（`:137-152`）。
5. **滚轮只滚本地回看**：`terminal_page.cpp:555-570`，只改 `Session.scrollOffset`，不发给 PTY；路由设计注释 `terminal_session_controller.h:114-115`。
6. **选区仅拖选**：按下/拖动/释放 `terminal_page.cpp:528-554`，状态字段 `terminal_page.h:107-109`；无双击选词/三击选行/Shift 扩选/Shift+PageUp。
7. **无右键菜单**：`terminal_grid_widget.cpp` 无任何 `onContextMenu` 等事件处理（纯渲染器）。
8. **终端零键盘快捷键**：新建/关闭/全屏/搜索只有按钮入口；模块 `onKeyEvent` 仅 Esc/Cmd+C（`module_terminal.cpp:150-167`）；notepad 有 `shortcut()` 先例（`notepad_window.cpp:1999-2066`）。
9. **退出无保护**：`onStop` 直接 `closeSession(0)` 循环（`module_terminal.cpp:134-146`），`closeSession` 无确认（`terminal_session_controller.cpp:193-209`）；`app::setQuitGuard` 只挂 notepad（`app_assembly.cpp:97-100`），三选弹窗可复用（`notepad_window.cpp:1444-1528`）。
10. **网页端**：canvas 自绘 + 透明文本层供原生选区（`web/www/render.js:3,173`）；键带仅 Esc/Tab（`www/index.html:113-117`，处理器 `app.js:675-678`）；已经有标签双击改名（`app.js:465`）；传输为 fetch 流而非 EventSource（`protocol.js:711-751`）。
11. **#450/#413 漂移成立**：滚动条覆盖层拖拽已实现（`terminal_page.cpp:697-730`），搜索条与前向搜索已实现（`terminal_page.cpp:1143`、`terminal_search.cpp`）。
12. **页面规模**：`terminal_page.cpp` 1537 行，compose 约 1345 行单函数（`wc -l` 实测）。

## 三之二、待核项核验结论（M1 Task 1，2026-11-25）

| 断言 | 判定 | 证据 / 处置 |
|---|---|---|
| #508 快照上限 200×60、超限丢帧 | **真** | Swift `BridgeLimits` `CortaBridge.swift:41-45`；C++ 镜像 `swift_bridge.cpp:17-18`；超限跳过帧 `:76-78`。按 Task 4 修 |
| #509 `ansiIndexedColor` 仅 0–15 | **真** | `terminal_grid_widget.cpp:234-252`，`index > 15` 回落 index 7。按 Task 2 修 |
| #465 进页同步建 ConPTY 14.4ms | **假（已过时）** | `onPageEnter → ensureBackend → startSessionAsync`（`module_terminal.cpp:41-45,130`），已是异步建会话；无需处理，核验后关闭 |
| #499 CSP 双策略交集 | **假（已修）** | `web_routes.cpp:14-27` 为唯一 CSP 响应头源，`index.html` 显式不放 meta（注明 bug499），favicon `data:` 抑制 404。Task 5 只做 E2E 验证 |
| #436/#449 IME 候选窗位置 | **待真机** | 代码有 `ime_bridge` 与 `ime-trace.log` 三段落点（`terminal_page.cpp:18`），静态无法判定候选窗位置 |
| OSC 0/2 标题 / OSC 52 远程复制未消费 | **真** | `settermpropCbImpl` 只处理 `VTERM_PROP_ALTSCREEN`（`terminal_vterm_core.cpp:271-273`）；全仓无 OSC 52 处理 |
| 中文间字距「宽字不读实际字形宽」 | **部分（按设计）** | `cellXOf` 前缀宽和恒按 `width × cellW`，注释明确「不读字形 advance，只影响字形间隙」（`terminal_grid_widget.cpp:366-372`，bug244/clue103）。属设计取舍，不是缺陷 → 移出缺陷清单 |
| #383 启动目录单值 | **真** | `terminal_settings.h:28-29`：C-4 已砍相关 UI，仅 `workDir` 单值 + shell 可配 |
| need490 未完成项清单 | **部分** | 池卡 detail 列 7 项开放决策（QR 编码器、TOTP HMAC、密钥存储、连接指示、关 tab 语义、重连全量刷新、cookie 过期）；代码侧 E2E 已实跑（见下） |

**网页 E2E 实测（2026-11-25，M1 Task 5）**：`DEVDESK_E2E_SKIP_INSTALL=1 bash tests/e2e/run-e2e.sh` → `E2E_OK server-up -> assets-embedded -> handshake+loopback-login -> grid-frame -> echo-hi -> auto-reconnect -> resync-frame`；Edge/Chromium 手机视口（390×844，isMobile+hasTouch）；截图落 `evidence/need490-w7/`。→ **#497 回显判定为已修，可关闭；#499 CSP 单一源已落地。**

---

## 四、三端能力矩阵（定稿）

图例：✅ 做 ｜ ⏳ 长期 ｜ — 不做

| 能力 | App 桌面 | PC 网页 | 手机网页 | 备注 |
|---|---|---|---|---|
| 选即复制 | ✅ | ✅ | — | 均为已有选区再挂一次；手机长按交给系统 |
| 链接识别点击 | ✅ | ✅ | — | 共用识别规则；触屏误触率高 |
| 鼠标上报 | ✅ | ✅ | — | 编码逻辑共用；手机无鼠标 |
| 拖文件插入路径 | — | ✅ | — | app 缺需跨仓给 EUI-NEO 加文件拖放（L），不做 |
| 中键粘贴 | — | — | — | 收益低、mac 无法验 |
| 连字 / 图片协议 | — | — | — | 与网格模型冲突 / 无基础 |
| 读屏·无障碍 | — | ✅ | ✅ | 网页文本层已有语义，补 aria 即可；app 自绘 L |
| 页内搜索 | ✅ | — | — | 网页用浏览器原生 Ctrl+F 即可（文本层可搜） |
| 字号快捷键 | ✅ | ✅ | — | 手机设置面板够用 |
| 右键菜单 | ✅ | — | — | 网页自绘会劫持浏览器菜单，不做 |
| 标签改名 | ✅ | ✅ 已有 | — | 桌面补平；网页已具备 |
| 标签拖拽排序 / 关闭其他 | ✅ | ✅ | — | 手机保持点击切换 |
| 最小键带（Ctrl/↑/↓） | n/a | — | ✅ | 并入 need490 收口 |
| 退出保护 | ✅ | n/a | n/a | 网页关页即断，不拦 |
| 分屏 | — | — | — | 已裁决 |
| 会话恢复 | ⏳ | ⏳ | ⏳ | 长期 |

---

## 五、需求切分原则

1. **一条需求走完整流程**（分析→调研→方案→开发→测试→审计），不攒批、不并行。
2. **核验先行**：任何需求开工前，其依赖的文档断言必须先落在核验表的「真」栏。
3. **拆缝先行**：鼠标上报与交互补齐都要动 `terminal_page`，先把「鼠标/选区/滚动」这条输入路由抽成独立组件（只抽这条线，不重写页面），后续功能长在组件上。
4. **ROI 分诊**：能感知 + 低成本 → 先做；能感知 + 高成本 → 排后；感知不到 → 长期不做。
5. **规模标注**：S ≤ 半天、M ≤ 2 天、L > 2 天（相对本机 mac 开发节奏）。

---

## 六、验收口径

| 面 | 验收方式 |
|---|---|
| App · mac | 每条需求真机实操 + 定向 doctest/probe；UI 类必须真窗口 probe 几何断言 + 截图 |
| App · Windows | GitHub CI（`windows-2025`）：Release 构建 + doctest 全量 + headless 探针 + 网页 E2E；**GUI 真机实测不在本计划内，后续另排** |
| PC 网页 | 本机浏览器（Chrome/Safari）+ 现有 `tests/e2e/run-e2e.sh` 覆盖核心链路；交互类补 DOM probe |
| 手机网页 | 真机浏览器验证最小键带与回显；不做自动化 |

测试成本纪律：研发阶段只跑受影响域定向用例（`--dev` + `--filter`）；全量 doctest + probe + 守卫链只在里程碑收口跑一次。

---

## 七、主要风险

| 风险 | 影响 | 处置 |
|---|---|---|
| 文档断言为假（已见 2 例漂移） | 白做 | M1 核验卡；假的项关池卡 |
| 拆缝动到选区/滚动，回归面大 | 选中偏移、滚轮失效 | 拆缝单独一条需求，先红后绿：把 existing 选区/滚动行为写成测试再搬代码 |
| 鼠标上报与本地选区冲突 | `htop` 里无法复制 | Shift 覆盖约定；先实现「Shift 强制本地」再实现上报 |
| 网页 canvas 与文本层对齐漂移 | 链接命中错位 | 命中测试以文本层 DOM 为准，不以 canvas 像素为准 |
| Windows CI 无 GUI 会话 | 平台差异在真机上才暴露 | 明确登记为已知缺口，不在本计划宣称「Windows 已验收」 |

---

## 八、待办边界（不写进本设计）

池卡入池编号、每条需求的详细技术方案卡、测试清单卡由执行阶段按 devflow 流程产出，本设计只定范围与验收。
