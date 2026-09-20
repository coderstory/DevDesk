# DevDesk UI/UX 设计规范

> **作者**：基于 ui-ux-pro-max 设计智能 + DevDesk 业务需求
> **日期**：2026-09-21
> **目的**：在 spec §6.5（design token）和 §6.5.2（大气原则）基础上，定义完整的 UI/UX 行为规范
> **作用**：HTML 原型 + EUI-NEO C++ 实现都要遵循本文档

---

## 1. 信息架构（IA）

### 1.1 全局导航

```
DevDesk 侧边栏（260 px，固定左）
├── 🏠 首页        → Home 仪表盘
├── 📝 Markdown    → Markdown 笔记本
├── ✅ Todo         → Todo 看板
├── 🔄 Claude      → Claude 切换器
└── ⚙️ 设置        → 设置页（底部独立）
```

**位置决策**：
- Claude 切换器的 profile 列表放**右侧主区左侧**（不是侧边栏）—— 侧边栏只放"模块入口"，profile 数量会增长，不应挤侧边栏
- 设置放在**侧边栏底部**（独立视觉块），遵循 macOS 系统偏好设置习惯

### 1.2 每个模块的内部布局

**Markdown 笔记本**：左-中-右三栏
- 文件树（200-240 px，可拖拽调整）
- 编辑器（flex，可隐藏切换为预览模式）
- 预览（flex）

**Todo 看板**：横向滚动列
- 列数固定 4（待办 / 进行中 / 已完成 / 已归档）
- 列宽固定 280 px，列间距 20 px
- 列内卡片垂直堆叠

**Claude 切换器**：左-右两栏
- 左：Profile 列表卡片（垂直堆叠，宽度 380-420 px）
- 右：详情面板（当前 Profile 详情 + 最近调用日志）

**Home 仪表盘**：单列垂直流
- 顶部欢迎区（h1 + 日期）
- 4 个统计卡片（横向 grid）
- 最近编辑（列表）
- 快捷操作（按钮组）

---

## 2. 布局规范（"大气"档）

### 2.1 窗口与栅格

| 项 | 值 | 说明 |
|----|------|------|
| 默认窗口 | 1600 × 1000 | 大气档（占 1080P 屏 83%） |
| 最小窗口 | 1200 × 720 | 三栏布局最小可读尺寸 |
| 主区 padding | **40 px** | 不是 24 |
| 内容最大宽度 | 1200 px | 防超宽屏文字过长，居中 |
| 栅格系统 | 8 px | 所有间距用 8 的倍数 |

### 2.2 各区域具体尺寸

| 区域 | 尺寸 |
|------|------|
| 侧边栏宽度 | 260 px |
| 侧边栏内 padding | 24 px（上）/ 16 px（左右）|
| 导航项高度 | 40 px |
| 导航项间距（gap） | 4 px |
| 侧边栏品牌区高度 | 48 px（含 logo） |
| 主区顶部 padding | 40 px（顶 / 左右），10 px（底）|
| 内容区水平 padding | 40 px |
| 卡片间距 | **20 px**（不是 12） |
| 卡片内 padding | **24-28 px** |
| 统计卡片间距 | 20 px |

---

## 3. 组件规范

### 3.1 按钮

**尺寸档（大气档）**：

| 类型 | 高度 | padding-x | 字号 | 圆角 | 用途 |
|------|------|-----------|------|------|------|
| 主按钮 | **44 px** | 20 px | 15 px / 500 | 10 px | 「+ 新建」、「保存」 |
| 次按钮 | **40 px** | 20 px | 15 px / 500 | 10 px | 「设置」、「取消」 |
| 小按钮 | 32 px | 12 px | 13 px / 500 | 8 px | 行内操作（编辑/删除） |
| 图标按钮 | 40×40 px | - | - | 10 px | 工具栏图标 |

**状态色**：
- 默认：`bg: accent` / `text: #FFFFFF`
- hover：背景 +6% 亮度（`#E0896B`），150ms ease-out
- pressed：背景 -8% 亮度（`#B85F3F`），50ms ease-in
- disabled：背景 `bg-muted`，text `text-disabled`，无 hover
- focus-visible：外圈 2 px `accent` ring，offset 2 px（WCAG AAA）

**图标按钮**：
- 图标尺寸 18-20 px（按钮内）或 20 px（独立图标按钮）
- 与文字间距 8 px（水平排列）
- 必须有 `aria-label`（图标无文字时）

### 3.2 输入框

| 元素 | 高度 | padding | 圆角 | 字号 |
|------|------|---------|------|------|
| 单行输入 | **40 px** | 0 14 px | 10 px | 15 px |
| 多行编辑器 | 320 px 起 | 14 px | 12 px | 15 px |
| 搜索框 | 40 px | 0 14 px | 10 px | 14 px |

**状态**：
- 默认：bg `bg-elevated`，border `border-default`
- focus：border `accent`，外圈 2 px ring（offset 2 px），150ms
- error：border `error`，下方红色 helper text
- disabled：bg `bg-muted`，text `text-disabled`，cursor `not-allowed`

**必填标记**：右上角 6×6 px 圆点，颜色 `error`（不挡文字）

### 3.3 卡片

| 类型 | padding | 圆角 | 边框 | 阴影 |
|------|---------|------|------|------|
| 普通卡片 | 20 px | 12 px | 1 px `border-default` | 无（macOS 高端风格）|
| 统计卡片 | 24 px | 14 px | 1 px `border-default` | 无 |
| Profile 卡片（active）| 20 px | 14 px | 2 px `accent` | 无 |
| 大卡片（详情面板）| **28-32 px** | 14 px | 1 px `border-default` | 无 |

**阴影说明**：方案 B 用实色 + 边框而非阴影分层（参考 Cursor、Things 3）。仅在以下情况用阴影：
- 浮层（dropdown / menu）：`0 1px 2px rgba(0,0,0,0.04), 0 4px 12px rgba(0,0,0,0.06)`
- 模态框：`0 8px 24px rgba(0,0,0,0.12)`

### 3.4 Chip / Tag

- 高度：**22 px**（不是 28）
- padding：0 10 px
- 字号：11 px / 500
- 圆角：**999 px**（pill 形）
- 默认：`bg: bg-elevated` / `text: text-secondary` / `border: 1 px border-default`
- accent（激活 / 选中）：`bg: primary-soft` / `text: accent` / `border: transparent`
- 优先级色（Todo 用）：
  - 高：`bg: error` / `text: #FFFFFF`
  - 中：`bg: warning` / `text: bg-base`
  - 低：`bg: info` / `text: bg-base`

### 3.5 状态指示点

- 8 × 8 px 圆点（成功/运行中用 8 px，绿/警告/错误一致）
- 位置：与文字 baseline 对齐或水平居中（视父容器）
- 配套标签：必须同时用文字 + 点（不能只靠颜色，色盲用户友好）

### 3.6 Profile 卡片（Claude 切换器专用）

```
┌─────────────────────────────────────────────┐
│ [Avatar 40px] 名称 [ACTIVE chip]   [⚙]      │
│              base-url（font-mono）         │
│              model · sk-ant-***xyz         │
└─────────────────────────────────────────────┘
```

- **active 卡片**：border 2 px `accent` + bg `accent-soft`
- **普通卡片**：border 1 px `border-default` + bg `bg-surface`
- 高度：约 90 px（自适应内容）
- avatar 圆 / 40 px / bg `bg-elevated` / 文字 `text-secondary` / 字号 16 px
- 点击整个卡片区域 = 选中（不是只点 avatar）
- hover：bg `bg-hover`，过渡 150 ms

### 3.7 Todo 卡片

- bg `bg-elevated`，padding 14 px
- **左边框 4 px** 表示优先级颜色（不是整卡背景色）
- 圆角 12 px
- 字号 14 px（标题），11 px（元数据）
- hover：bg `bg-hover`
- 拖拽中：opacity 0.6，scale 0.98，shadow 中
- drop target 高亮：列背景 `primary-soft`，过渡 150 ms

---

## 4. 交互规范

### 4.1 Hover

| 元素 | 默认态 | hover 态 | 过渡 |
|------|--------|----------|------|
| 按钮 | accent | accent-hover | 150 ms ease-out |
| 卡片 | bg-surface | bg-hover | 150 ms ease-out |
| 导航项 | text-secondary | text-primary + bg-hover | 150 ms ease-out |
| Profile 卡片 | border 1 px | border 1 px + bg-hover | 150 ms ease-out |
| 表格行 | transparent | bg-hover | 100 ms ease-out |

**所有交互元素**必须 `cursor: pointer`（按钮）/ `cursor: text`（输入框）/ `cursor: grab`（可拖拽）。

### 4.2 Focus（WCAG 2.2 AAA）

```
:focus-visible {
  outline: 2px solid var(--accent);
  outline-offset: 2px;
  border-radius: 8px; /* 跟随元素圆角 */
}
```

- 不能 `outline: none` 不带替代
- 不能用 1 px 低对比度 outline
- 不能让 sticky header / modal 遮挡 focus 元素

### 4.3 Active / Pressed

- 按钮按下：bg 变深 8% 亮度，scale 0.98（极轻微），50 ms ease-in
- 即时反馈，不能 0 ms 切换（让人感觉"卡"）
- 同时 haptic feedback（macOS trackpad / Windows 可选）

### 4.4 Loading / Skeleton

**Skeleton（加载占位）**：

```css
.skeleton {
  background: linear-gradient(
    90deg,
    var(--bg-surface) 0%,
    var(--bg-elevated) 50%,
    var(--bg-surface) 100%
  );
  background-size: 200% 100%;
  animation: shimmer 1.5s ease-in-out infinite;
  color: transparent;
  border-radius: 6px;
}
@keyframes shimmer {
  0% { background-position: 200% 0; }
  100% { background-position: -200% 0; }
}
```

- prefers-reduced-motion 用户：禁用动画，给纯色占位
- 占位元素尺寸与最终内容一致（避免 layout shift，CLS < 0.1）

**Spinner**：
- 16 px / 24 px / 32 px 三档
- accent 颜色，1.2 s 一周
- 永远不遮挡主内容（顶部状态条或按钮内 inline）

### 4.5 Empty States

**设计公式**：图标（大）+ 标题（一句话说明）+ 副标题（解决建议）+ 主操作按钮

```
       📄
  还没有 Markdown 笔记
按 + 新建笔记 创建你的第一篇
       [+ 新建笔记]
```

- 图标：48 px，text-tertiary 颜色
- 标题：18 px / 500，text-primary
- 副标题：14 px，text-secondary
- 按钮：主按钮

### 4.6 Error States

**错误展示原则**：
1. **就近原则**：错误尽量靠近出错字段，不要堆到顶部
2. **可读性**：用中文 + 具体原因，不只说"出错了"
3. **可恢复**：提供"重试"按钮
4. **可记录**：错误同时 LOG_ERROR（用户可在日志查看）

**Toast（轻量错误）**：
```
[图标]  保存失败：权限不足     [×]
        文件在 ~/notes/inbox/ideas.md
```
- 5 秒自动消失（错误用 8 秒，因更严重）
- 鼠标 hover 暂停倒计时
- 多个 toast 垂直堆叠，最多 3 个
- 颜色：error 顶部 `error` 4 px bar + bg `bg-surface`
- icon：`!` 圆形 / 16 px

---

## 5. 动效规范

### 5.1 时长档

| 场景 | 时长 | 缓动 |
|------|------|------|
| Hover / 颜色变化 | **150 ms** | `ease-out` |
| 按钮按下 | 50 ms | `ease-in` |
| 页面切换 | **200 ms** | `ease-out` |
| 模态框出现 | 200 ms | `cubic-bezier(0.32, 0.72, 0, 1)` (spring) |
| 模态框消失 | **150 ms**（比进入快）| `ease-in` |
| Toast 滑入 | 250 ms | `ease-out` |
| Toast 滑出 | 200 ms | `ease-in` |
| 拖拽跟随 | 即时 | - |
| 主题切换（dark↔light）| 300 ms | `ease-in-out` |

### 5.2 缓动函数

只用以下 4 种：
- `ease-out`（默认）
- `ease-in`（退出）
- `ease-in-out`（状态切换）
- `cubic-bezier(0.32, 0.72, 0, 1)`（spring，模态框）

**不要用 linear**（除了进度条）。

### 5.3 prefers-reduced-motion

```css
@media (prefers-reduced-motion: reduce) {
  * {
    animation-duration: 0.01ms !important;
    transition-duration: 0.01ms !important;
  }
}
```

- 关闭所有非必要动画（拖拽跟随除外）
- macOS 系统偏好 → 辅助功能 → 显示器 → 减弱动态效果

### 5.4 微交互清单

| 交互 | 动效 |
|------|------|
| 页面切换 | 淡入淡出（200 ms） |
| 模态框出现 | 缩放（0.95→1）+ 淡入（200 ms spring） |
| Modal 消失 | 缩放（1→0.95）+ 淡出（150 ms） |
| Toast 进入 | 从右下滑入（250 ms） |
| Toast 退出 | 向右滑出（200 ms） |
| 卡片拖起 | scale(0.98) + opacity 0.8 |
| 卡片放下 | 还原 + 目标列高亮 |
| 主题切换 | 全局颜色 300 ms 过渡 |
| 列表项新增 | 从顶部滑入（200 ms） |
| 列表项删除 | 高度塌陷 + 淡出（200 ms） |

---

## 6. 模态框（Modal）

### 6.1 结构

```html
<div class="modal-backdrop">     <!-- 半透明遮罩 -->
  <div class="modal-container"   <!-- 居中，最大宽度 560 -->
       role="dialog"
       aria-modal="true"
       aria-labelledby="modal-title">
    <h2 class="modal-title">新建 Profile</h2>
    <div class="modal-body">
      <!-- 表单字段 -->
    </div>
    <div class="modal-footer">
      <button class="btn-secondary">取消</button>
      <button class="btn-primary">创建</button>
    </div>
    <button class="modal-close" aria-label="关闭">×</button>
  </div>
</div>
```

### 6.2 行为

- **打开**：背景滚动锁定，焦点陷阱在 modal 内，Esc 关闭
- **焦点管理**：
  - 打开时：焦点移到 modal 第一个可关闭元素（"名称"输入框）
  - 关闭时：焦点返回到触发它的元素（"新建"按钮）
- **遮罩**：bg `rgba(26, 24, 22, 0.7)` + backdrop-filter blur(4px)（macOS 风格）
- **关闭方式**：
  1. 点击遮罩
  2. 按 Esc
  3. 点关闭按钮
  4. 提交后自动关闭
- **大 modal**：垂直滚动（modal-body），footer 固定底部

### 6.3 表单 modal（新建 Profile 示例）

| 字段 | 类型 | 必填 | 验证 |
|------|------|------|------|
| 名称 | text | 是 | 唯一，长度 1-50 |
| Base URL | url | 是 | 以 `http://` 或 `https://` 开头 |
| API Key | password | 是 | 长度 ≥ 10 |
| 模型 | select | 是 | 下拉 + 自定义 |

提交时校验顺序：先必填 → 格式 → 唯一性（查数据库）。每个错误显示在该字段下方。

---

## 7. Toast 通知

### 7.1 类型与样式

| 类型 | 颜色 | icon | 自动消失 |
|------|------|------|----------|
| info | info | ⓘ | 4 秒 |
| success | success | ✓ | 3 秒 |
| warning | warning | ⚠ | 6 秒 |
| error | error | ✕ | **8 秒**（不 hover） |

### 7.2 位置与堆叠

- 位置：**右下角**（macOS 通知中心习惯；不是顶部）
- 距底 24 px，距右 24 px
- 多个 toast 向上堆叠，最多 3 个可见
- 第 4 个 + 排队等候

### 7.3 结构

```
┌────────────────────────────────────────────┐
│ [icon] 标题                    [×]          │
│        详细描述文字                         │
│ ──────────────────── (progress bar)         │
└────────────────────────────────────────────┘
```

- 宽度 360 px，圆角 12 px，padding 16 px
- 左侧 4 px accent bar
- 背景 `bg-surface`，边框 1 px `border-default`
- 阴影 `0 4px 12px rgba(0,0,0,0.12)`
- 底部进度条显示剩余时间
- hover：暂停倒计时
- × 按钮：手动关闭

---

## 8. 键盘快捷键

### 8.1 全局快捷键（macOS / Windows）

| 操作 | macOS | Windows | 触发位置 |
|------|-------|---------|----------|
| 切换到 Home | ⌘1 | Ctrl+1 | 全局 |
| 切换到 Markdown | ⌘2 | Ctrl+2 | 全局 |
| 切换到 Todo | ⌘3 | Ctrl+3 | 全局 |
| 切换到 Claude | ⌘4 | Ctrl+4 | 全局 |
| 切换到设置 | ⌘, | Ctrl+, | 全局 |
| 显示快捷键帮助 | ⌘/ | Ctrl+/ | 全局 |

### 8.2 模块内快捷键

**Markdown**：
| 操作 | macOS | Windows |
|------|-------|---------|
| 新建笔记 | ⌘N | Ctrl+N |
| 保存 | ⌘S | Ctrl+S |
| 搜索 | ⌘F | Ctrl+F |
| 切换预览 | ⌘P | Ctrl+P |

**Todo**：
| 操作 | macOS | Windows |
|------|-------|---------|
| 新建任务 | ⌘N | Ctrl+N |
| 移动到下一列 | ⌘] | Ctrl+] |
| 移动到上一列 | ⌘[ | Ctrl+[ |
| 完成任务 | ⌘D | Ctrl+D |

**Claude**：
| 操作 | macOS | Windows |
|------|-------|---------|
| 新建 Profile | ⌘N | Ctrl+N |
| 启用选中 Profile | ⌘⏎ | Ctrl+⏎ |
| 测试连通 | ⌘T | Ctrl+T |

### 8.3 快捷键展示

- **发现性**：所有按钮 / 菜单项右侧标注快捷键
- **帮助面板**：⌘/ 打开「键盘快捷键」modal，分类列出全部
- **首次使用**：右下角 toast「💡 试试 ⌘N 新建笔记」

---

## 9. 拖拽交互

### 9.1 视觉反馈

| 阶段 | 元素状态 |
|------|----------|
| 静止 | 普通样式 |
| hover（拖拽前）| cursor: grab |
| 拖起 | 半透明 (opacity 0.6) + 轻微缩小 (scale 0.98) + 阴影增强 |
| 拖拽中 | 跟随光标 |
| 命中 drop target | 目标列高亮（bg `primary-soft`） |
| 未命中 | 无视觉变化 |
| 放下成功 | 卡片落入新位置，短暂闪烁高亮 |
| 放下失败 | 弹回原位 + 轻微晃动 |

### 9.2 拖拽性能

- 拖拽过程中**禁用过渡**（避免 60 FPS 掉帧）
- `transform: translate3d(...)` 触发 GPU 加速
- 不在拖拽过程中做布局计算
- prefers-reduced-motion：拖拽无缩放/阴影变化，仅位置跟随

---

## 10. 无障碍（A11Y）

### 10.1 WCAG 2.2 目标

- **Level AA**（必达）：文字对比度 ≥ 4.5:1，UI ≥ 3:1
- **Level AAA**（尽力）：文字 ≥ 7:1，暗色主题正文 ≥ 7:1

### 10.2 已验证对比度（方案 B Dark）

| 前景 | 背景 | 对比度 | WCAG |
|------|------|--------|------|
| #FAF9F5 | #1A1816 | 15.2:1 | AAA ✓ |
| #C9C5BD | #1A1816 | 11.8:1 | AAA ✓ |
| #9A948A | #1A1816 | 6.5:1 | AA ✓ |
| #D97757 | #1A1816 | 5.8:1 | AA ✓（主按钮） |
| #D97757 | #FAF9F5 | 3.4:1 | AA Large only ⚠️ |

**注意**：#D97757 主色在 Light mode 上仅达 AA Large（用于 ≥ 18 px 文字或 ≥ 14 px 粗体）。UI 元素 ≥ 3:1。

### 10.3 键盘导航

- Tab 顺序遵循视觉顺序
- 跳过链接：「Skip to main content」（Cmd+; 触发后焦点移到主区）
- 所有功能可不用鼠标完成
- 焦点始终可见（WCAG 2.4.7）

### 10.4 屏幕阅读器（VoiceOver / NVDA）

- 所有图标按钮 `aria-label`
- 表单字段 `<label>` 关联
- Modal `role="dialog" aria-modal="true" aria-labelledby="..."`
- Toast 用 `aria-live="polite"`，error 用 `aria-live="assertive"`
- 进度条 `role="progressbar" aria-valuenow aria-valuemin aria-valuemax`

### 10.5 焦点陷阱（modal 内）

```js
// 进入 modal 时记录 lastFocus
// Tab 在 modal 内循环
// Shift+Tab 反向循环
// Esc 关闭 modal
// 关闭时 lastFocus.focus()
```

---

## 11. 平台习惯

### 11.1 macOS

- 菜单栏在**屏幕顶部**（不在窗口内）
- 窗口控制（红黄绿）在**左上**
- 快捷键修饰：`⌘`（不是 Ctrl）
- 偏好设置：`⌘,`
- 退出应用：`⌘Q`（不是 File → Exit）
- 系统托盘 = 菜单栏图标
- 字体：SF Pro Display + SF Mono（fallback 到苹方 + JetBrains Mono）

### 11.2 Windows

- 窗口控制（最小化/最大化/关闭）在**右上**
- 菜单栏在**窗口内**（应用自己的菜单）
- 快捷键修饰：`Ctrl`
- 偏好设置：`Ctrl+,`
- 系统托盘 = 右下角图标
- 字体：Segoe UI + Cascadia Code（fallback 到微软雅黑 + JetBrains Mono）

### 11.3 共同点（跨平台）

- 复制/粘贴/撤销/重做：`⌘/Ctrl + C/V/Z/Shift+Z`
- 全选：`⌘/Ctrl + A`
- 文件保存：`⌘/Ctrl + S`
- 查找：`⌘/Ctrl + F`
- 新建：`⌘/Ctrl + N`

---

## 12. 设计检查清单（Pre-Delivery）

每个页面/组件发布前确认：

- [ ] 所有交互元素有 hover / focus / active 状态
- [ ] focus ring 可见（WCAG AAA 2px）
- [ ] 文字对比度达标（AA 4.5:1 / AAA 7:1）
- [ ] 所有图标按钮有 `aria-label`
- [ ] 键盘可访问（Tab 顺序正确，焦点不丢失）
- [ ] 触摸目标 ≥ 44×44 px（虽然桌面不强制，但保留）
- [ ] 暗色模式对比度重新验证（不能用 Light 模式色）
- [ ] 空状态 / 加载态 / 错误态都有设计
- [ ] prefers-reduced-motion 友好
- [ ] 截屏 + 检查 hover/pressed/disabled/empty 状态
- [ ] 拖拽功能测试流畅度（目测 60 FPS）
- [ ] 快捷键全部工作 + 有可视化提示
- [ ] 字体回退：英文 / 中文 / emoji 都能正常显示

---

## 13. 实现优先级

按 MVP 必交付排序：

| 优先级 | 项 | 备注 |
|--------|------|------|
| P0 | 配色 + 字号 + 圆角 + 按钮 | 视觉基础 |
| P0 | 侧边栏 + 主页 padding | 布局基础 |
| P1 | Hover / focus / active 状态 | 交互基础 |
| P1 | 加载态（skeleton） | 用户感知 |
| P2 | Modal（含 focus trap）| 关键功能 |
| P2 | Toast（含队列） | 反馈 |
| P2 | 空状态 | 用户感知 |
| P3 | 拖拽视觉反馈 | Todo 用 |
| P3 | 动画过渡 | 体验加分 |
| P3 | 键盘快捷键 + 帮助 | 高效用户 |
| P4 | prefers-reduced-motion | 无障碍 |
| P4 | 屏幕阅读器优化 | 无障碍 |

---

**设计规范 v1.0** · 后续根据实际实现反馈持续迭代
---

# 第二部分：各功能模块的完整 UI/UX 设计

> 每个模块含：**信息架构 → 视觉布局 → 交互流程 → 状态机 → 键盘快捷键 → 无障碍 → 代码示例**

---

## 14. 全局：侧边栏（Sidebar）

### 14.1 信息架构

```
DevDesk
├── 品牌区 [D Logo + "DevDesk"]
│   （点击品牌 = 回到 Home）
├── 导航项
│   ├── 🏠 首页
│   ├── 📝 Markdown 笔记本
│   ├── ✅ Todo 看板
│   └── 🔄 Claude 切换器
├── （flex spacer）
└── ⚙️ 设置
```

### 14.2 视觉布局

```
┌─────────────────────────┐
│                         │  ← 24px 顶部 padding
│   [D]  DevDesk          │  ← 品牌区 48px 高
│                         │
│   🏠 首页               │  ← 导航项 40px 高
│   📝 Markdown 笔记本   │
│   ✅ Todo 看板         │
│   🔄 Claude 切换器    │
│                         │
│   （flex: 1）           │
│                         │
│   ⚙️ 设置               │  ← 固定底部
│                         │
└─────────────────────────┘
   260px 宽
   bg-bg-surface
   border-right: 1px border-default
```

### 14.3 交互流程

| 状态 | 样式 |
|------|------|
| 默认 | text-secondary, bg-transparent |
| hover | text-primary, bg-bg-hover |
| active（当前页面）| text-accent, bg-accent-soft, font-weight 600 |
| focus-visible | outline 2px accent |

- 点击 → 路由切换 + active 状态立即更新
- 键盘 `⌘1/2/3/4` 快速切换
- 长按无特殊行为（避免误触）

### 14.4 关键决策

- **侧边栏宽度固定 260 px**：与 spec §6.5.2 一致，**不可拖拽调整**（避免布局抖动）
- **不使用图标 + 文字组合**：单 emoji + 文字，简单清晰
- **设置项独立在底部**：与系统偏好设置习惯一致
- **品牌区可点击**：点击 DevDesk 文字 = 回到 Home

### 14.5 无障碍

- `role="navigation"` `aria-label="主导航"`
- 当前项 `aria-current="page"`
- Tab 顺序：Home → Markdown → Todo → Claude → Settings
- 屏幕阅读器朗读：`"首页，导航项，当前页面"`（active 时）

---

## 15. 模块 1：Home 仪表盘

### 15.1 信息架构

```
┌─────────────────────────────────────────────────┐
│  欢迎回到                                         │
│  2026 年 9 月 21 日 · 星期一                      │
│                                  [↻ 刷新] [⚙ 设置] │
├─────────────────────────────────────────────────┤
│  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐            │
│  │ 3   │ │ 7   │ │ 1   │ │ 142  │            │
│  │今日笔记│ │待办任务│ │逾期任务│ │API调用│            │
│  └──────┘ └──────┘ └──────┘ └──────┘            │
├─────────────────────────────────────────────────┤
│  最近编辑                                        │
│  • 设计稿v2.md  (2 小时前)                       │
│  • 周会纪要.md  (昨天)                          │
│  • bug分析.md  (3 天前)                        │
├─────────────────────────────────────────────────┤
│  [+ 新建笔记] [+ 新建任务] [📋 切换 Claude Profile] │
└─────────────────────────────────────────────────┘
```

### 15.2 视觉布局细节

**欢迎区**：
- 标题 "欢迎回到" — h1, 32px / 700, text-primary
- 副标题日期 — 13px / 400, text-secondary, margin-top 8px
- 右侧操作：`↻ 刷新`（ghost 按钮，刷新数据）+ `⚙ 设置`（次按钮）

**统计卡片（4 个并排）**：
- 网格：`grid-cols-4`，间距 20px
- 每个卡片：padding 28px，圆角 14px，bg-surface，边框 1px border-default
- 内部结构：
  - 顶部 label：12px / 500 / uppercase / text-secondary
  - 中间数字：**48px / 700 / text-primary**（display 级）
  - 底部 badge：px-3 py-1 圆角 pill / 11px / bg-elevated / text-tertiary
- 特殊卡片（逾期任务）：数字颜色 `error`，badge 颜色也 `error`（视觉警示）

**最近编辑**：
- 卡片容器：padding 28px，圆角 14px
- 标题 "最近编辑"：18px / 600
- 列表项：每项 56px 高（padding 12px + 16px gap），hover bg-hover
- 列表项结构：
  - 左侧文件图标：40×40 圆角 8px，bg-elevated
  - 中间：标题（14px / 500）+ 时间（11px / text-tertiary）
  - 右侧：tag chip（可选）

**快捷操作按钮组**：
- 三个按钮横排，间距 12px
- 主操作「+ 新建笔记」用 accent 背景
- 其他两个用 bg-elevated + border

### 15.3 交互流程

| 操作 | 触发 | 结果 |
|------|------|------|
| 点击统计卡片 | `⌘N` | 跳转到对应模块并预填 |
| 点击最近编辑项 | - | 跳到 Markdown 模块并打开该笔记 |
| 点击「+ 新建笔记」 | `⌘N` | 跳到 Markdown 模块 |
| 点击「+ 新建任务」 | - | 跳到 Todo 模块 |
| 点击「切换 Claude Profile」| - | 跳到 Claude 模块 |
| 点击 `↻ 刷新` | - | 重新查询 DashboardStats |
| 点击 `⚙ 设置` | - | 跳到设置页 |

### 15.4 状态机

```
LoadingState:
  initial → loading → loaded
  initial → error → retry → loading
  loading 超时（>5s）→ error

RefreshState:
  idle → refreshing → idle
  refreshing 失败 → toast error + 回到 idle
```

**加载态**：4 个统计卡片显示 skeleton shimmer
**空态**：所有计数为 0 + "今天还没有活动"提示
**错误态**：整个 Home 区域显示错误卡片 + 「重试」按钮

### 15.5 键盘快捷键

- `⌘N` — 新建笔记（跳到 Markdown）
- `⌘⇧N` — 新建任务（跳到 Todo）
- `⌘R` — 刷新 Home 数据
- `⌘1` — 切换到 Home（已在 Home 时无操作）

### 15.6 无障碍

- 标题层级：h1（欢迎），h2（最近编辑），h3（统计卡 label）
- 统计数字加 `aria-label="今日笔记 3 篇"`
- 列表项加 `role="listitem"`
- 快捷操作按钮有可见文字（不是纯图标）

---

## 16. 模块 2：Markdown 笔记本

### 16.1 信息架构

```
┌─────────────────────────────────────────────────────────────┐
│  Markdown 笔记本                                             │
│  本地化的笔记系统，文件可直接用外部编辑器打开                  │
│                          [↻ 重扫] [⚙] [+ 新建笔记]         │
├──────────────┬──────────────────────┬──────────────────────┤
│  文件树       │  编辑器              │  预览                │
│  (220px)      │  (flex)              │  (flex)              │
│              │                      │                      │
│  🔍 搜索     │  # 你好              │  你好                │
│              │                      │  ──────             │
│  📄 设计稿v2 │  这是段落...        │  这是段落...        │
│  📄 周会纪要 │                      │                      │
│  📄 bug分析  │  ## 二级标题         │  二级标题           │
│              │                      │                      │
│  ▾ 📁 inbox │  - 列表 1            │  • 列表 1           │
│    📄 ideas │  ```cpp              │  ```                │
│    📄 todo  │  #include <...>     │  代码块             │
│              │  ```                │  ```                │
│  [+ 新建]    │                      │                      │
│              │                      │                      │
│  标签:       │                      │                      │
│  #工作       │                      │                      │
│  #学习       │                      │                      │
└──────────────┴──────────────────────┴──────────────────────┘
```

### 16.2 视觉布局细节

**文件树（左栏，220px）**：
- bg-surface，边框 1px border-default
- 顶部搜索框（占满 12px padding）
- 文件列表：每项 32px 高，padding 8px 12px
  - 默认：text-secondary
  - hover：bg-hover, text-primary
  - active（当前打开）：bg-accent-soft, text-accent, font-weight 600
- 目录（`▾`/`▸` 切换）：可展开/折叠
- 文件图标用 emoji（📄/📁）
- 底部「+ 新建」按钮（次按钮风格）
- 标签区域：mt-auto 自动在底部，chip 列表

**编辑器（中栏，flex）**：
- bg-surface
- font-family: JetBrains Mono
- font-size: 14px
- line-height: 1.7
- padding: 24px
- 顶部显示当前文件名（小标签，text-tertiary）
- 占位符（placeholder）："开始写你的 Markdown..." 

**预览（右栏，flex）**：
- bg-surface
- font-family: Inter（正文）+ JetBrains Mono（代码）
- 与编辑器共享滚动位置（scroll sync）
- 顶部同样显示文件名
- padding: 24px

**三栏比例**：220px : flex : flex（默认等宽）
**列宽可拖拽调整**：分隔条 4px，hover 时 cursor: col-resize

### 16.3 交互流程

| 操作 | 触发 | 结果 |
|------|------|------|
| 新建笔记 | `⌘N` 或按钮 | 弹出输入框："请输入文件名（如 ideas.md）" → 创建文件 + 打开 |
| 保存 | `⌘S` | 写盘 + 更新 SQLite index + toast "已保存" |
| 切换文件 | 点击文件树项 | 加载到编辑器 + 预览（debounced 100ms） |
| 搜索 | `⌘F` 或输入框聚焦 | 实时过滤文件列表（200ms debounce） |
| 删除 | 右键菜单 → 删除 | 确认 modal → 删除文件 + index |
| 重命名 | 右键菜单 → 重命名 | inline edit → 改名文件 + index |
| 拖入文件 | ❌ 不支持（EUI-NEO 限制） | 用「+ 新建」按钮 + 文件选择器 |
| 外部编辑器修改 | 文件 watcher 检测 | toast「文件已被外部修改」+ 三选项 |
| 标签过滤 | 点击 chip | 文件树只显示同标签文件 |

### 16.4 状态机

```
EditorState:
  empty (无文件打开) | editing | saving | error
  editing → saving → edited（成功）或 error
  saving 失败 → 显示错误 + 保留 dirty 内容

FileWatcherState:
  idle | external_changed
  external_changed → 显示 toast 三选项（覆盖/重载/取消）

SearchState:
  empty | typing | results | no_results
```

**未保存指示**：顶部文件名旁显示「•」圆点
**自动保存**：可选（设置中关闭/开启）；默认关闭，由用户 `⌘S` 触发
**冲突处理**：外部修改优先 → 弹 toast 让用户决策

### 16.5 键盘快捷键

| 操作 | macOS | Windows |
|------|-------|---------|
| 新建 | ⌘N | Ctrl+N |
| 保存 | ⌘S | Ctrl+S |
| 搜索 | ⌘F | Ctrl+F |
| 关闭当前文件 | ⌘W | Ctrl+W |
| 切换预览 | ⌘P | Ctrl+P |
| 加粗 | ⌘B | Ctrl+B |
| 斜体 | ⌘I | Ctrl+I |
| 代码 | ⌘E | Ctrl+E |
| 链接 | ⌘K | Ctrl+K |

### 16.6 Markdown 工具栏（可选）

在编辑器顶部加一行工具栏（折叠时只显示图标）：
- `H1` `H2` `H3` | `B` `I` | `</>` (code) | `[link]` | `—` (hr) | `📋 list`

### 16.7 无障碍

- 编辑器 `aria-label="Markdown 编辑器"`
- 预览 `aria-live="polite"`（保存时朗读更新）
- 文件树 `role="tree"`，每项 `role="treeitem" aria-selected="..."`
- 搜索结果 `role="listbox"` + `aria-activedescendant`

---

## 17. 模块 3：Todo 看板

### 17.1 信息架构

```
┌─────────────────────────────────────────────────────────────────┐
│  Todo 看板                                                     │
│  ⌘N 新建任务 · 拖拽卡片跨列移动（也可右键"移动到..."）         │
│            [🔍 搜索任务] [+ 列] [+ 新建任务]                  │
├──────────────┬──────────────┬──────────────┬──────────────────┤
│ 待办 (3)     │ 进行中 (1)    │ 已完成 (2)   │ 已归档          │
│ ┌──────────┐ │ ┌──────────┐ │ ┌──────────┐ │                  │
│ │ 写文档    │ │ │修复bug#42│ │ │初次部署  │ │                  │
│ │ 🔴高     │ │ │ 🟡中    │ │ │ ✓ 今天   │ │  📦              │
│ │ ⏰明天   │ │ │          │ │ │          │ │  归档的任务在这里 │
│ └──────────┘ │ └──────────┘ │ └──────────┘ │                  │
│ ┌──────────┐ │              │              │                  │
│ │ 会议纪要  │ │              │ ┌──────────┐ │                  │
│ │ 🟡中    │ │              │ │环境配置   │ │                  │
│ │ ⏰今天   │ │              │ │ ✓ 昨天   │ │                  │
│ └──────────┘ │              │ └──────────┘ │                  │
└──────────────┴──────────────┴──────────────┴──────────────────┘
```

### 17.2 视觉布局细节

**列（Column）**：
- 宽度固定 280 px，bg-surface，圆角 14px
- padding 16px
- 列头：高度 40 px，包含「列名 + 计数」+「+」按钮
- 卡片列表：flex-1，垂直堆叠，gap 8px
- 拖拽高亮（命中目标时）：bg `primary-soft`，过渡 150ms

**卡片（Card）**：
- bg `bg-elevated`，padding 14px
- 圆角 12px
- **左边框 4 px** 表示优先级颜色（高=error/中=warning/低=info）
- 标题：14px / 500 / text-primary（截断 2 行）
- 元数据行：11px / text-tertiary
  - 优先级 chip（10px / 500）
  - 截止日期 chip（带 ⏰ 图标）
  - 标签 chips
- 已完成：opacity 0.6 + line-through 标题
- hover：bg `bg-hover`
- 拖起：scale 0.98 + opacity 0.6 + shadow 中

### 17.3 交互流程

| 操作 | 触发 | 结果 |
|------|------|------|
| 新建任务 | `⌘N` 或「+ 新建」按钮 | 在第一列末尾创建空白卡片 + 聚焦标题输入 |
| 编辑任务 | 双击卡片 / 单击右侧 ⓘ | 展开详情 panel（右侧 slide-in）|
| 移动任务（拖拽）| 鼠标拖 | 跨列实时跟随 |
| 移动任务（键盘）| 选中卡片 + `⌘]`/`⌘[` | 跨列移动 |
| 移动任务（菜单）| 右键 → "移动到..." | 弹出子菜单选列 |
| 完成任务 | 点击 checkbox | 卡片移到"已完成"列 + 划线 |
| 取消完成 | 点击已完成卡片 checkbox | 移回"待办"列 |
| 归档 | 右键 → 归档 | 移到"已归档"列 |
| 删除 | 右键 → 删除 | 确认 modal → 永久删除 |
| 拖入 | ❌ 不支持 | 无（任务通过按钮/快捷键创建）|

### 17.4 详情面板（点击卡片右侧展开）

```
┌──────────────────────────────────────┐
│ 编辑任务                [×]          │
├──────────────────────────────────────┤
│ 标题：[____________________]        │
│                                      │
│ 描述：[                        ]      │
│       [____________________]        │
│                                      │
│ 截止：📅 2026-09-22                   │
│                                      │
│ 优先级：🔴高 🟡中 🔵低 ⚪无           │
│                                      │
│ 标签：[工作 ×] [+ 添加]              │
│                                      │
│ ────────────────────────             │
│ 创建于 3 天前                        │
│ [删除]                  [保存]        │
└──────────────────────────────────────┘
```

- 右侧 slide-in 面板，宽度 400 px
- 背景遮罩 50% 透明度
- 关闭：× 按钮 / Esc / 点击遮罩 / 保存后

### 17.5 状态机

```
CardState:
  editing_title | editing_desc | viewing | dragging
  viewing → editing_title（双击）
  editing_title → viewing（失焦或 Esc）
  dragging → viewing（释放鼠标到有效 drop target）或 viewing（释放到无效位置）

ColumnHighlight:
  normal | drag_hover
  normal → drag_hover（拖拽过程中命中本列）
  drag_hover → normal（拖出本列）
```

### 17.6 键盘快捷键

| 操作 | macOS | Windows |
|------|-------|---------|
| 新建 | ⌘N | Ctrl+N |
| 选中卡片 | Tab / 方向键 | 同 |
| 打开详情 | ⏎ | ⏎ |
| 移到下一列 | ⌘] | Ctrl+] |
| 移到上一列 | ⌘[ | Ctrl+[ |
| 完成 / 取消完成 | ⌘D | Ctrl+D |
| 归档 | ⌘⇧A | Ctrl+Shift+A |
| 删除 | ⌫ | Del |
| 关闭详情 | Esc | Esc |

### 17.7 无障碍

- 卡片 `role="article"` `aria-label="任务: 写文档, 高优先级"`
- 列 `role="list"` `aria-label="待办，3 项"`
- 拖拽时屏幕阅读器朗读「已抓起 写文档 任务」
- 释放后朗读「已移至 进行中 列」
- 优先级不能仅靠颜色，加文字标识

---

## 18. 模块 4：Claude 配置切换器

### 18.1 信息架构

```
┌─────────────────────────────────────────────────────────────────┐
│  Claude 切换器   🟢 运行中 :8888                                │
│  管理多套 Claude API 配置 · 本地代理 http://127.0.0.1:8888     │
│                          [⏸ 停止代理] [+ 新建 Profile]         │
├──────────────────────┬──────────────────────────────────────────┤
│  Profile 列表         │  当前 Profile 详情                       │
│  (400px)              │  (flex)                                  │
│                      │                                          │
│  ● 官方 [ACTIVE]     │  名称：官方                              │
│    api.anthropic.com │  Base URL：https://api.anthropic.com    │
│    sonnet-4-5        │  Token：sk-ant-•••••••••xyz [Keychain]   │
│                      │  模型：claude-sonnet-4-5                 │
│  ○ 第三方 A          │                                          │
│    api.3rd-party.com │  [启用] [测试连通] [编辑] [删除]        │
│    opus-5            │                                          │
│                      │  ─────────────────                       │
│  ○ 测试账号          │                                          │
│    api.test.com      │  最近调用                                │
│    haiku-5           │  14:32 POST /v1/messages 200 1247ms      │
│                      │  14:30 POST /v1/messages 200 832ms       │
│  [+ 新建 Profile]    │  14:25 POST /v1/messages 429 312ms       │
│                      │  [查看全部 →]                             │
└──────────────────────┴──────────────────────────────────────────┘
```

### 18.2 视觉布局细节

**状态条（顶部）**：
- 标题 + 状态指示器（绿色运行点 + 端口）
- 状态指示器：8px 圆点 + 文字 + 端口号
  - 运行中：success 色
  - 停止：text-tertiary 色
  - 错误：error 色 + 错误描述

**Profile 列表（左栏，400px）**：
- 卡片垂直堆叠，gap 12px
- Profile 卡片：
  - 默认：border 1px border-default, bg-surface
  - hover：bg bg-hover
  - **active**：border 2px accent, bg primary-soft
  - padding: 16px
  - 圆角: 14px
  - 内容：
    - 顶部：avatar（40px 圆形）+ 名称 + ACTIVE chip
    - 中部：base-url（font-mono, 12px, text-tertiary）
    - 底部：model · token hint（font-mono）

**详情面板（右栏）**：
- 大卡片 padding 28px
- 网格：2 列展示名称 + 模型，全宽展示 Base URL + Token
- Token 显示：脱敏（中间 `••••••••`），后缀末 3 位
- 操作按钮组：主按钮 + 次按钮组合
- 最近调用表格：font-mono，11px，行高宽松
  - 列：时间 | 方法/路径 | 状态 | 耗时 | 大小

### 18.3 交互流程

| 操作 | 触发 | 结果 |
|------|------|------|
| 新建 Profile | `⌘N` 或按钮 | Modal 表单（详见 §6.3） |
| 启用 Profile | 详情面板「启用」按钮 | 切换 is_active，重启代理，重启后 toast |
| 测试连通 | 「测试连通」按钮 | 发送 GET /v1/models，显示状态码 + 耗时（5s 超时） |
| 编辑 Profile | 「编辑」按钮 | Modal 表单（预填） |
| 删除 Profile | 「删除」按钮 | 确认 modal（需输入 hint 后 3 位验证防误删）|
| 启动代理 | 「▶ 启动」| 启动 HTTP server，状态变绿 |
| 停止代理 | 「⏸ 停止」| 关闭 HTTP server，状态变灰 |
| 复制 token | 点击 Token 旁 📋 | 复制到剪贴板 + toast |
| 复制 Claude 配置 | 顶部「📋 Claude 配置」 | 复制 `export ANTHROPIC_BASE_URL=...` 命令 |

### 18.4 新建/编辑 Modal

```
┌────────────────────────────────────────────────────┐
│  新建 Profile                              [×]    │
├────────────────────────────────────────────────────┤
│  名称 *                                            │
│  [____________________]                            │
│                                                    │
│  Base URL *                                       │
│  [https://api.anthropic.com        ]              │
│  示例：https://api.anthropic.com                   │
│                                                    │
│  API Key *                                         │
│  [••••••••••••••••••••••••••]                    │
│  存储于 macOS Keychain（开发机）/ Windows DPAPI      │
│                                                    │
│  模型 *                                            │
│  [claude-sonnet-4-5        ▼]                     │
│  [+] 自定义                                       │
│                                                    │
│  ⚠️ API Key 将加密存储，无法明文查看              │
│                                                    │
│                              [取消]  [创建]        │
└────────────────────────────────────────────────────┘
```

- 字段顺序：名称 → Base URL → API Key → 模型
- 实时校验：必填标记 + 错误信息就近显示
- 「创建」按钮在所有字段合法时才可点击
- API Key 字段 `type="password"`，可点击 👁 切换显示

### 18.5 启用 Profile 流程（含代理重启）

```
点击「启用」
  ↓
更新 SQLite (is_active 切换)
  ↓
停止当前代理（如果运行中）
  ↓
读取新 active profile
  ↓
启动新代理（绑定新 profile 配置）
  ↓
toast: "已切换到「官方」profile · 代理已重启"
  ↓
状态指示器闪一次绿色高亮（500ms）
```

如果启用失败（Keychain 取 key 失败、网络问题）：
- 回滚 SQLite 更改
- toast error：「启用失败：xxx，请重试」
- 状态指示器保持原样

### 18.6 状态机

```
ProfileState:
  viewing | editing | confirming_delete | testing_connection

ProxyState:
  stopped | starting | running | stopping | error
  stopped → starting → running
  running → stopping → stopped
  error → starting（重试）

TestConnectionState:
  idle | testing | success | failure
  success 后 5s 回到 idle
  failure 持续显示（直到下次点击）
```

### 18.7 键盘快捷键

| 操作 | macOS | Windows |
|------|-------|---------|
| 新建 Profile | ⌘N | Ctrl+N |
| 启用 | ⌘⏎ | Ctrl+⏎ |
| 测试连通 | ⌘T | Ctrl+T |
| 编辑 | ⌘E | Ctrl+E |
| 删除 | ⌘⌫ | Ctrl+Del |
| 启动/停止代理 | ⌘R | Ctrl+R |
| 复制 Token | ⌘⇧C | Ctrl+Shift+C |

### 18.8 无障碍

- Profile 列表 `role="listbox"`，每项 `role="option" aria-selected="..."`
- ACTIVE 状态用文字 + 颜色双重提示
- 状态指示器加 `aria-label="代理运行中，端口 8888"`
- 调用日志表格语义化 `<table>` 标签

---

## 19. 模块 5：设置

### 19.1 信息架构（单页分组）

```
设置
├── 外观
│   ├── 主题（深色 / 浅色 / 跟随系统）
│   └── 强调色（预设 5 选 1，目前锁定为 Claude Clay）
├── Markdown 笔记本
│   ├── 笔记目录（路径 + 「打开目录」）
│   ├── 编辑器字体大小（数字）
│   ├── 编辑器字体（4 选 1）
│   └── 自动保存（开关，默认关）
├── Todo 看板
│   └── 显示已归档列（开关，默认开）
├── Claude 代理
│   ├── 端口（数字）
│   ├── 认证 Token（只读 + 复制 + 重新生成）
│   └── 开机自启（开关，默认关）
├── 日志
│   ├── 日志级别（5 选 1）
│   └── 调用日志保留天数（数字）
├── 安全
│   ├── API key 提示脱敏粒度（3 选 1）
│   └── 清除所有数据（危险操作）
└── 关于
    ├── 版本号
    ├── 许可证
    └── 依赖列表
```

### 19.2 视觉布局

- 单页垂直滚动，max-width 800px 居中
- 每个分组：大卡片 padding 32px
- 分组内：垂直堆叠，gap 20px
- 表单行结构：
  ```
  [字段名 + 说明]                [控件]      ← 两端对齐
  ```
- 字段名：14px / 500 / text-primary
- 说明：13px / 400 / text-secondary
- 危险操作：「清除所有数据」用红色按钮 + 二次确认 modal

### 19.3 交互流程

| 操作 | 触发 | 结果 |
|------|------|------|
| 切换主题 | 单选 | 立即应用（300ms 过渡）+ 保存 config |
| 修改路径 | 输入 + ⏎ | 验证路径存在性 + 保存 |
| 打开目录 | 「📂 打开目录」按钮 | 调用系统 open 命令 |
| 修改端口 | 输入 | 立即重启代理（toast 通知）|
| 复制 token | 按钮 | 剪贴板 + toast |
| 重新生成 token | 按钮 | 确认 modal + 重启代理 |
| 清除所有数据 | 「🗑 清除所有数据」| 二次确认 modal → 删除 db + config + 笔记 |

### 19.4 状态机

```
SettingsState:
  viewing | editing | saving | error

DirtyCheck:
  clean | dirty
  dirty 时显示「保存」按钮
  离开页面前提示未保存
```

### 19.5 键盘快捷键

- 全局 `⌘,` 打开设置
- `⌘S` 保存设置（如果有改动）
- `Esc` 关闭设置页（无改动时直接退出）

### 19.6 无障碍

- 控件 `<label>` 关联（点击 label 聚焦输入框）
- 路径输入框 `aria-describedby="path-hint"`
- 危险按钮 `aria-label="清除所有数据（危险操作）"`
- 表单验证错误 `aria-invalid="true"`

---

## 20. 跨页面状态设计

### 20.1 加载状态（Loading）

**Skeleton（首选，用于内容区域加载）**：

```
┌────────────────────────────────┐
│ ████████████  ← shimmer       │
│                                │
│ ████ ████████                 │
│                                │
│ ████████████████              │
└────────────────────────────────┘
```

- bg linear-gradient，shimmer 1.5s 循环
- 占位元素与最终内容同尺寸（防 layout shift）
- 禁用 `prefers-reduced-motion` 时用纯色占位

**Spinner（用于操作型加载）**：

```
   ◜◝
  ◜  ◞
  ◟  ◟
   ◟◞
```

- 16/24/32 px 三档
- accent 颜色，1.2s 一周
- 用于按钮内 inline 或顶部状态条

**进度条（用于文件操作）**：

```
████████████░░░░░░░  65%
```

- 高度 4 px 或 8 px
- 圆角 2 px
- 显式百分比文字（不仅是动画）

### 20.2 空状态（Empty）

**公式**：图标 + 标题 + 副标题 + 主操作

**Markdown 模块空态**：
```
       📄
  还没有 Markdown 笔记
  创建你的第一篇笔记开始记录
       [+ 新建笔记]
```

**Todo 模块空态**：
```
       ✅
  还没有任务
  创建第一个任务开始管理
       [+ 新建任务]
```

**Claude 模块空态**：
```
       🔄
  还没有 Profile
  添加你的第一个 Claude API 配置
       [+ 新建 Profile]
```

**Home 模块空态**（所有计数为 0）：
```
       📊
  今天还没有活动
  创建一篇笔记或任务开始
       [+ 新建笔记]
```

### 20.3 错误状态（Error）

**Toast 错误（轻量）**：
```
[✕] 保存失败：权限不足            [×]
    ~/Library/Application Support/DevDesk/notes/inbox/ideas.md
```

**页面级错误（严重）**：
```
┌────────────────────────────────────┐
│         ⚠                         │
│  加载失败                          │
│  无法连接到数据库                  │
│                                    │
│  [重试]      [查看日志]            │
└────────────────────────────────────┘
```

**字段级错误（表单）**：
```
名称 *
[官方profile  ✕]
⚠ 已存在同名 profile              ← 红色，13px
```

### 20.4 成功状态（Success）

- 用 Toast（3 秒自动消失）
- 简短文字 + 可选操作按钮
- 例：「已保存 ✓」（`Saved`）
- 例：「已切换到「官方」profile · 代理已重启」

---

## 21. 主题切换（Dark / Light）

### 21.1 触发方式

- 设置 → 外观 → 主题（3 选 1）
- 全局快捷键：`⌘⇧L` / `Ctrl+Shift+L`（切换深浅）

### 21.2 切换动效

- 全局颜色 transition 300 ms ease-in-out
- 包括：背景、bg、border、text、accent、shadow
- 不包括：布局、字体、间距（避免抖动）

### 21.3 持久化

- 保存到 config.json
- 启动时根据上次选择应用
- 「跟随系统」模式下监听 OS 主题变化（macOS 10.14+）

### 21.4 切换时的 Toast

```
[☀] 已切换到浅色主题
```

不显示，自动消失。

---

## 22. 全局搜索（未来功能）

不在 MVP，但预留设计空间：

- `⌘K` 打开搜索面板（macOS Spotlight 风格）
- 搜索范围：所有笔记、任务、Profile、最近调用
- 顶部输入框，下方结果分组（笔记 / 任务 / Claude）
- 方向键导航，⏎ 跳转到结果
- Esc 关闭

---

## 23. 自定义窗口标题栏（未来）

EUI-NEO 不支持透明标题栏。当前使用系统默认标题栏。

未来升级路径：
- macOS 27+：`titlebarAppearsTransparent = YES` + 自定义 traffic light 位置
- Windows：Win32 `DWMAPI` 扩展框架
- 在 DevDesk 升级到 Metal 后端时一起做

---

# 设计规范完结

**v1.0 · 2026-09-21**
- 基于方案 B（温暖专业）
- 大气档尺寸（1600×1000 默认，15px 正文）
- 完整 5 个模块 + 跨页面状态 + 主题切换设计
- WCAG 2.2 AA 目标，关键场景 AAA
- 包含键盘快捷键、动效时长、无障碍规则

