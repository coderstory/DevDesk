# 终端 M3（网页端轻量项）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans（并行子任务版：按 Task 分派，文件归属互斥）。

**Goal:** 网页端补齐到「手机能跑一轮 Claude Code、桌面浏览器顺手」：最小键带、链接可点、拖文件插入路径、无障碍语义、标签拖拽排序。

**Architecture:** 全部落在 `src/module_terminal/web/www/*`（纯前端，无构建步骤，资源经 `web_assets.h` 嵌入）与 `tests/e2e/*.mjs`（Playwright）。后端协议零改动：键带复用 `RealClient.sendInput`，链接/拖文件只影响本地输入路径，无障碍只加 DOM 语义。

**Tech Stack:** 原生 JS（无框架）、CSS、Playwright（chromium/webkit，手机视口 390×844）、`bash tests/e2e/run-e2e.sh`。

**Spec:** `docs/superpowers/specs/2026-11-25-terminal-completion-design.md`（三端矩阵：手机端只做键带；PC 网页做链接/拖文件/无障碍/标签排序）

## Global Constraints

- 不改 `web_routes.cpp` 的 CSP（唯一响应头源）；新增内联脚本/样式一律走外部文件。
- 不新增依赖、不引入框架；资源体积增量 ≤ 20 KB。
- 触屏（`hasTouch` 或 `innerWidth ≤ 768`）不启用拖拽类交互（误触）。
- e2e 每条需求都要带断言；测试与代码同批写，全量只在批次收口跑一次。
- 禁止手敲 cmake/ctest；网页侧验证入口 `bash tests/e2e/run-e2e.sh`。

## Review Focus

- **Ctrl 锁存**：点一次 Ctrl 后按字母必须发 `\x03` 之类控制码，再点一次解除；锁存态可见。
- **链接点击**：长按/拖动不得误触发打开；URL 尾部标点不吞进链接。
- **拖文件**：多文件、含空格路径必须正确转义；拖入非输入区不插入。
- **无障碍**：文本层不得变成逐字符节点（保持整体可读、可复制）。
- **标签拖拽**：触屏不启用；桌面拖拽后活动标签不变。

---

### Task M3-1: 最小键带（Ctrl 锁存 / ↑ / ↓）

**Files:** `www/index.html`（#keybar 追加三键）、`www/app.css`（锁存态）、`www/app.js`（`syncKeybar` 区）、`tests/e2e/web_terminal_e2e.mjs`

**Interfaces:**
- Produces: `window.__wt.client.sendInput('\x03')` 走既有通道；DOM id：`#key-ctrl`、`#key-up`、`#key-down`；`#key-ctrl` 加 `.latched` 类表示锁存。

- [ ] 点击 Ctrl → `.latched`；再点解除；锁存时点字母键带发 `\x03`（Ctrl+C 等价）
- [ ] ↑/↓ 发 `\x1b[A` / `\x1b[B`
- [ ] e2e：断言 `#key-ctrl` 点击后 `.latched` 存在、`sendInput` 收到 `\x03`（用 `window.__wt` 钩子记录）

### Task M3-2: 链接识别点击（网页）

**Files:** `www/render.js`（文本层点击命中）、`www/app.js`（点击分派）、`www/app.css`（悬停下划线）、`tests/e2e/web_terminal_e2e.mjs`

- [ ] 点击文本层中 URL → `window.open(url, '_blank', 'noopener')`；拖选中的 pointerup 不触发
- [ ] 与服务端同一识别口径（行内 http/https 与绝对路径；尾部标点剥除）
- [ ] e2e：构造含 URL 的输出行，点击对应坐标后断言 `window.__lastOpen`（测试钩子记录目标）

### Task M3-3: 拖文件插入路径

**Files:** `www/app.js`（dragover/drop）、`www/index.html`（drop 遮罩可选）、`tests/e2e/web_terminal_e2e.mjs`

- [ ] 拖入 1..n 个文件 → 依次插入「转义后路径 + 空格」，经 bracketed paste 发送
- [ ] 含空格路径用单引号包裹；拖到非终端区不插入
- [ ] e2e：用 Playwright 构造 `DataTransfer` 触发 drop，断言 `sendInput` 收到转义串

### Task M3-4: 无障碍语义

**Files:** `www/index.html`（#text-layer role/aria）、`www/app.js`（状态播报）、`tests/e2e/web_terminal_e2e.mjs`

- [ ] 文本层 `role="log"`、`aria-live="polite"`、`aria-label`；连接状态变化播报（`aria-live` 区域）
- [ ] 键带与粘贴钮可 Tab 聚焦、有 `aria-label`
- [ ] e2e：断言 role/aria 属性存在；Tab 键可从输入到键带

### Task M3-5: 标签拖拽排序 + 关闭其他（桌面浏览器）

**Files:** `www/app.js`（tabs 渲染区）、`www/app.css`（拖拽态）、`tests/e2e/web_terminal_e2e.mjs`

- [ ] 桌面（非触屏）拖拽标签换位，调 `client` 的既有 tab 操作；触屏不启用
- [ ] 右键标签 → 关闭其他（触屏长按不启用）
- [ ] e2e：桌面视口下拖动两个标签，断言顺序变化；手机视口断言无拖拽柄

---

## M3 收口

- [ ] `bash tests/e2e/run-e2e.sh`（全链路）一次
- [ ] 手机真机手验：键带跑一轮 Claude Code（输入/Ctrl+C/历史/Tab/粘贴输出）
- [ ] 文档回填 spec 三端矩阵状态
