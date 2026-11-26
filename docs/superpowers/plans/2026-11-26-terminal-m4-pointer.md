# 终端 M4（指针交互三端）实施计划

> 需求卡：`docs/superpowers/plans/2026-11-26-terminal-pointer-interaction.md`
> 本计划把「opencode 里选文复制」与「TAB 单击」钉成可回归的行为，三端分别落地。

**Goal:** 三端（App / PC 网页 / 手机网页）点击与触摸的选中/复制行为一致、可靠；TUI（opencode/vim/tmux）开启鼠标上报后仍能选文复制。

**Architecture:** App 侧 = 页面输入路由（左键选区、右键复制、Shift 覆盖上报）+ 平台鼠标上报；网页侧 = 文本层点击/长按与选区状态机（前端纯 JS）。测试：App 用离屏真窗口探针，网页用 Playwright。

**Spec:** `docs/superpowers/specs/2026-11-25-terminal-completion-design.md`、需求卡见上。

## Global Constraints

- 选区只能由**左键/触摸长按拖动**产生与修改；右键/滚动不得改动选区状态。
- 有选区时右键 = 直接复制，不弹菜单；无选区 = 弹菜单。
- 应用请求鼠标（DECSET 1000/1002/1003）时：Shift+拖选走本地；未按 Shift 的拖动/滚轮走上报。
- 拖拽排序阈值 ≥ 6px，且一次拖动只换位一次。
- 三端都有回归断言；真机验收项单独列。

## Review Focus

- 手指长按 500ms 后才进入选择态（避免起手就选）；期间滚动不触发选择。
- 触屏拖动标签不得换位（手机端不启用拖拽）。
- Shift 覆盖在「应用请求鼠标但用户只想选文」时必须是默认可发现路径。
- 复制后剪贴板内容与选中文本逐字节一致（含中文与换行）。

---

### Task 1: TAB 单击可靠 + 拖拽阈值（App）

**Files:** `src/module_terminal/terminal_page.cpp/.h`
**Interfaces:** 无新增公开面；`reorderTabIndex` 语义保留。

- [ ] 探针 `terminal_pointer_probe` 断言 1：点击 `term.tab.1.bg` 中心 → `activeIndex` 变 1（红→绿）
- [ ] 拖拽 `< 6px` 不换位；`> 6px` 且越过中线才换位一次

### Task 2: 右键复制（App）

**Files:** 同上
- [ ] 有选区 + 右键松开 → 写入剪贴板，不弹菜单，选区保持
- [ ] 无选区 + 右键 → 弹菜单（复制/清屏/搜索）
- [ ] 探针断言 2/3：拖选后复制回调非空；右键后 `term.sel.0` 仍在

### Task 3: 鼠标上报与 Shift 覆盖（App）

**Files:** `terminal_page.*` + `terminal_session_controller.*`（透传）+ 平台层（并行子任务已完成）
- [ ] 应用请求鼠标时：未按 Shift 的拖动 → 上报字节，不产生本地选区；滚轮 → 上报
- [ ] Shift+拖动 → 本地选区；Shift+滚轮 → 本地回看
- [ ] 单测：vterm 层上报编码（`test_terminal_vterm_mouse.cpp`）

### Task 4: 网页端右键/长按复制与标签点击（PC + 手机）

**Files:** `web/www/app.js`、`web/www/render.js`、`web/www/index.html`、`web/www/app.css`、`tests/e2e/web_terminal_e2e.mjs`
- [ ] 桌面：有选中文本时右键 → 直接 `navigator.clipboard.writeText`（失败回退 `document.execCommand('copy')`）
- [ ] 手机：长按 500ms 进入选择态，松手后浮出「复制」按钮
- [ ] 标签单击切换稳定；触屏不启用拖拽排序
- [ ] e2e：`page.mouse.click(button:'right')` 后断言剪贴板钩子收到文本；手机视口长按断言复制按钮出现

### Task 5: 收口与回归

- [ ] App：`terminal_pointer_probe` 三断言 + `ui_driver_terminal-input` 全绿
- [ ] 网页：`tests/e2e/run-e2e.sh` 全绿
- [ ] 真机（不可代替）：终端里跑 opencode → Shift+拖选 → 右键 → 粘贴校验；手机浏览器跑一轮

## 依赖与顺序

Task 1/2/3 同属 `terminal_page.*`，串行；Task 4 独立（网页）；Task 5 最后。
