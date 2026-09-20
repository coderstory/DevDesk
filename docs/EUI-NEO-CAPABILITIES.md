# EUI-NEO UI 框架能力调研报告

> **调研日期**：2026-09-20
> **调研目的**：DevDesk 项目技术选型验证
> **调研方法**：实际打开 `/Users/coderstory/Dev/gui/EUI-NEO/` 源码，grep + 阅读组件实现 + 跑示例
> **结论**：DevDesk 的全部设计**基本可实现**，但有 5 个硬限制 + 5 个隐藏陷阱需要避开

---

## 0. TL;DR — 给开发者看的速查表

### ✅ 可放心使用（80+ 组件 / API）

| 类别 | 能力 |
|------|------|
| **基础容器** | `ui.row / column / flow / stack / loader / rect / polygon / text / image / svg / shadertoy` |
| **表单控件** | `button / input (多行+IME+选区) / checkbox / radio / switch / slider / stepper / progress / segmented / tabs / dropdown / datePicker / timePicker / colorPicker` |
| **导航/容器** | `navbar / sidebar / dialog / toast / tooltip / contextMenu / carousel` |
| **数据展示** | `dataTable / lineChart / barChart / pieChart / markdown / virtualList / virtualMasonry / scroll / scrollView` |
| **实验性** | `workshop::cardSlider / heartSwitch / neumorphicButton / tiltCard` |
| **样式** | 任意 hex 颜色 + 透明度 / 单色边框 + 圆角 / 外阴影 + 内阴影 / 2 色渐变（仅水平/垂直）/ transform 2D+3D |
| **字体** | FreeType 加载 TTF/OTF + DPI 自适应 + 系统字体自动 fallback（macOS苹方/Windows 微软雅黑/Linux Noto CJK）|
| **图标** | FontAwesome codepoint + Unicode Emoji（自动彩色）+ SVG（栅格化）|
| **交互** | onTap / onPress / onRelease / onHover / onDrag / onScroll / onContextMenu / onKeyEvent / onFocusChanged / clipboard / IME（macOS/Windows 完整，Linux no-op）|
| **多窗口** | `app::openWindow`（模态/非模态）|

### ❌ 不可用（必须绕开）

| 限制 | 后果 | 应对 |
|------|------|------|
| **OS 文件拖入** | 文件不能从 Finder/Explorer 拖入窗口 | "+ 新建"按钮 + `openFileDialog` |
| **drop target API** | 卡片跨列拖拽需自实现 | 自实现 `DragController`（~200 行）|
| **macOS 27 Liquid Glass / vibrancy** | 无透明标题栏、无材料层 | 实色 + 阴影替代 |
| **tree 组件** | 文件树需自实现 | 用 `virtualList` + 字符图标 + 状态机 |
| **全局 OS hotkey** | DevDesk 失焦时收不到键盘事件 | 切 profile 需先聚焦窗口 |

### ⚠️ 隐藏陷阱（容易踩坑）

| 陷阱 | 说明 |
|------|------|
| **`fontWeight()` 数值无效** | runtime 只打包 Bold 一份字重，`fontWeight(700)` 不切 face。打包多份 TTF 也不会切，**源码层面就不支持** |
| **托盘菜单只有 Show/Exit** | `tray_bridge` 4 后端都硬编码，不能加自定义菜单项 |
| **Toast 单实例无队列** | 同时显示多条要自管队列 |
| **Tab 焦点循环未实现** | `InputKey::Tab` 枚举存在但不调度，键盘 Tab 导航需自实现 |
| **窗口无 `onResize` / `onMove` 回调** | 自适应布局靠 `DslWindowRequest::compose(ui, screen)` 的 `screen` 参数变化触发 |

---

## 1. 组件清单（39 个 component 头文件全列）

### 基础原子（`core/dsl.h` + `components/text.h`、`image.h`、`panel.h`、`card.h`、`mousearea.h`）

| 组件 | 关键 API |
|------|----------|
| `ui.row(id)` | `.size / .gap / .padding / .alignItems / .justifyContent / .flexGrow` |
| `ui.column(id)` | 同 row |
| `ui.flow(id)` | 自动换行 wrap，`.gap / .lineGap` |
| `ui.stack(id)` | z-堆叠，`.alignItems / .justifyContent` |
| `ui.loader(id)` | 懒加载子树，`.active(bool) / .keepAlive() / .destroyOnHide()` |
| `ui.rect(id)` | 圆角矩形 + 边框 + 阴影 + 渐变 |
| `ui.text(id)` | 文本 + 字体 + 颜色 + 换行 + 对齐 + icon(codepoint) |
| `ui.image(id)` | PNG/JPG/GIF/WebP/SVG，`.source / .tint / .fit(Cover/Contain/Stretch) / .radius / .blur` |
| `ui.svg(id)` | 内嵌 SVG 字符串，nanosvg 栅格化 |
| `ui.polygon(id)` | 自定义多边形 |
| `ui.shadertoy(id)` | GLSL shader quad |
| `components::panel` | 主题面板（圆角 + 阴影）|
| `components::card` | 卡片容器 |
| `components::mouseArea` | 包装 rect，提供高层 pointer 事件：`onTap / onPress / onRelease / onHover / onEnter / onLeave / onMove / onDragStart / onDrag / onDragEnd / onScroll / onContextMenu / .dragThreshold / .suppressClickAfterDrag` |

### 表单控件（`components/*.h`）

| 组件 | 关键 API | 备注 |
|------|---------|------|
| `components::button` | `.text / .icon(codepoint) / .onClick / .onPress / .onRelease / .disabled / .colors(n,h,p) / .radius / .shadow` | 主题化 |
| `components::input` | `.value / .placeholder / .multiline / .scrollbar / .fontFamily / .fontSize / .onChange / .onEnter / .onFocus / .bind(Signal<string>)` | **内置选区 + 撤销重做 + 复制粘贴 + IME 预编辑 + 拖拽选区** |
| `components::checkbox` | `.checked(bool) / .bind(Signal<bool>) / .onChange` | |
| `components::radio` | `.selected / .items / .onChange` | |
| `components::switch` | `.checked / .bind / .onChange / .text` | |
| `components::slider` | `.value / .min / .max / .step / .bind / .onChange` | |
| `components::stepper` | `.value / .min / .max / .step / .base(10/16/2) / .bitWidth / .digits` | DEC/HEX/BIN |
| `components::progress` | `.value(0..1)` | |
| `components::segmented` | `.items / .selected / .bind / .onChange` | |
| `components::tabs` | `.items / .selected / .bind / .onChange` | |
| `components::dropdown` | `.items / .selected / .open / .bind / .bindOpen / .placeholder / .itemHeight / .onChange / .onOpenChange` | |
| `components::datePicker` | `.open / .value(year,month,day) / .onChange / .onOpenChange` | |
| `components::timePicker` | `.value(hour,minute) / .onChange` | |
| `components::colorPicker` | `.value(Color) / .onChange` | RGB，无预设色板 |

### 容器/导航

| 组件 | 作用 |
|------|------|
| `components::navbar` | 侧边/顶部导航栏（brand + items + footer）|
| `components::sidebar` | 抽屉侧栏（drawer）|
| `components::dialog` | 模态弹窗（带 backdrop + 主/次按钮）|
| `components::toast` | Toast（带 icon + title + message + 自动关闭，**单实例，无队列**）|
| `components::tooltip` | 提示框 |
| `components::contextMenu` | 右键菜单（支持嵌套子菜单 + path 选中）|
| `components::carousel` | 卡片轮播（支持 3D tilt）|

### 数据展示

| 组件 | 作用 |
|------|------|
| `components::dataTable` | 表格（columns + rows + 斑马纹 + 表头）|
| `components::lineChart` | 折线图（Linear/Curve/Step）|
| `components::barChart` | 柱状图 |
| `components::pieChart` | 饼图 |
| `components::markdown` | Markdown 渲染（基于 md4c）|
| `components::virtualList` | 虚拟化列表（itemCount + rowHeight + overscan）|
| `components::virtualMasonry` | 瀑布流虚拟化 |
| `components::scrollView` | 滚动容器（bind Signal + scrollbar + step）|

### 实验性（`components/workshop/`）

- `workshop::cardSlider` 3D 倾斜卡片轮播
- `workshop::heartSwitch` 心形开关
- `workshop::neumorphicButton` 新拟态按钮
- `workshop::tiltCard` 鼠标跟踪倾斜卡片

---

## 2. 布局原语

来源：`core/dsl.h`、`core/layout.h`（702 行）

### 容器（10 个 `Ui` 工厂方法）

```cpp
ui.row(id) / column(id) / flow(id) / stack(id) / loader(id) / rect(id) / polygon(id) /
ui.text(id) / image(id) / svg(id) / shadertoy(id)
```

### 位置 + 尺寸（`BuilderBase`）

```cpp
.position(x, y) / .x(float) / .y(float)
.size(w, h) / .width(SizeValue) / .height(SizeValue)
.fill() / .wrapContent()
.minWidth / .maxWidth / .minSize / .maxSize
.flexGrow(v=1) / .flexShrink(v=1) / .flex(grow, shrink=1)
.ignoreLayout(bool=true)   // overlay / 绝对定位
.zIndex(int)
.clip(bool)
```

**SizeValue 三态**：`Fixed(value)` / `WrapContent()` / `Fill()`

### 间距 + 对齐

```cpp
.margin(v) / .margin(h, v) / .margin(l, t, r, b)
.padding(v) / .padding(h, v) / .padding(l, t, r, b)
.gap(value)               // row/column 子项间距
.lineGap(value)           // flow 换行间距
.justifyContent(Align)    // 主轴
.alignItems(Align)        // 交叉轴
.align(Align main, Align cross)
```

### 关键 API 完整示例

```cpp
ui.column("root")
    .size(800, 600)
    .padding(24)
    .gap(12)
    .alignItems(core::Align::CENTER)
    .content([&] {
        ui.row("toolbar").fill().gap(8).content([&] {
            // children
        });
    }).build();
```

### **没有 grid / wrap / 真正的 overlay 容器**

- 没有 CSS Grid；用 `row + flexGrow` 模拟列
- 没有 wrap 元素；用 `flow()` 替代（自动换行）
- "绝对定位"用 `.x(y).ignoreLayout()` 模拟

---

## 3. 样式定制能力

每个 `Element` 通过 `ShapeBuilderBase` 或 `BuilderBase` 链式方法定制：

| 属性 | 方法 | 支持 |
|------|------|------|
| 背景色 | `.color(Color)` | ✅ |
| 渐变 | `.gradient(c1, c2, Vertical/Horizontal)` | ✅ 仅两色 + 仅两方向 |
| 边框 | `.border(width, Color)` | ✅ 单宽度 + 单色 |
| 圆角 | `.radius(float)` | ✅ |
| 阴影 | `.shadow(blur, offsetY, Color)` / `.shadow(blur, x, y, Color)` | ✅ 外阴影 |
| 内阴影 | `.insetShadow(...)` | ✅ |
| 文本颜色 | `.color(Color)` on `TextBuilder` | ✅ |
| 字体大小 | `.fontSize(float)` | ✅ |
| 字体粗细 | `.fontWeight(int 100..900)` | ⚠️ 数值传入但 **runtime 不切换 face** |
| 字体族 | `.fontFamily(string)` | ✅ 字符串别名 |
| 行高 | `.lineHeight(float)` | ✅ 0 = auto |
| 文本对齐 | `.horizontalAlign / .verticalAlign` | ✅ |
| 文本换行 | `.wrap(bool) / .maxWidth(float)` | ✅ |
| 透明度 | `.opacity(0..1)` | ✅ |
| 平移 | `.translate(x,y) / .translate3d / .translateX/Y/Z` | ✅ |
| 缩放 | `.scale(v) / .scale(x,y)` | ✅ |
| 旋转 | `.rotate / .rotateX / .rotateY / .rotateZ / .perspective / .transformOrigin` | ✅ 2D + 3D |
| 模糊 | `.blur(float)` on rect（**仅 rect**）| ⚠️ 矩形 backdrop blur |
| 状态色 | `.states(n,h,p) / .hoverColor / .pressedColor` | ✅ 自动平滑过渡 |
| 过渡动画 | `.transition(Transition) / .animate(AnimProperty bits)` | ✅ |
| 多边形点 | `.points([Vec2...]) / .point(x,y)` | ✅ |
| 图片滤镜 | `.tint / .flipVertically / .fit(Cover/Contain/Stretch)` | ✅ |

**没有的**：box-shadow 多层、stroke-only border、text-stroke、mask/clip-path 复杂形、滤镜（亮度/对比度/饱和度）、SVG 滤镜。

---

## 4. 颜色系统

来源：`core/render/render_types.h:29`

```cpp
struct Color { float r, g, b, a; };  // 线性 [0,1] RGBA
```

### 颜色构造

```cpp
core::Color c1("#D97757");                // #RGB / #RGBA / #RRGGBB / #RRGGBBAA
core::Color c2("#D97757CC");              // 含 alpha
core::Color c3 = core::Color::fromHex(0xD97757);
core::Color c4 = core::Color::fromHexRgba(0xD97757CC);
core::Color c5(0.85f, 0.47f, 0.34f, 1.0f);
core::Color::tryFromHex(hex, out);        // 安全解析
```

### 主题色切换

```cpp
auto tokens = components::theme::dark();
tokens.primary = core::Color("#D97757");
// 然后传给每个 component 的 .theme(tokens)
```

**主题切换是手动模式**：没有全局 `setTheme()`，每次 compose 调用都要重新传 `tokens`。`examples/keyboard.cpp` 用 `bool darkMode` + 局部 lambda `theme()` 实现切换。

### 透明度 + 渐变

- `Color.a` 通道原生支持
- `components::theme::withAlpha(c, a)` 替换 alpha
- 渐变仅 2 色 + 水平/垂直方向

### 没有的颜色能力

- **没有 CSS 变量语义**；token 在 C++ 端硬编码到 `ThemeColorTokens`
- 没有全局颜色注册表 / 命名色 / HSL / sRGB 转换

---

## 5. 字体系统

来源：`core/render/text.h`、`text.cpp`（1978 行）、`include/eui/dsl_app.h`

### 引擎

- **FreeType 2**（自带 `3rd/freetype`）
- `FT_LOAD_COLOR`（支持彩色字体/emoji）+ `FT_LOAD_TARGET_LIGHT`
- 单共享 atlas：2048×2048 灰度页 + 1024×1024 RGBA 彩色页

### 默认字体（bundled assets）

- UI 默认：`JingNanJunJunTi-JinNanJunJunTi-Bold-2.ttf`（中文 display Bold）
- 图标默认：`Font Awesome 7 Free-Solid-900.otf`
- 备用：`YouSheBiaoTiHei-2.ttf`
- **仅打包了一个字重（Bold）**

### 字体加载 API

```cpp
app::dslAppConfig()
    .textFont("fonts/Inter-Regular.ttf")
    .iconFont("assets/Font Awesome 7 Free-Solid-900.otf");

// 或底层
core::TextPrimitive::setDefaultFontFiles(textFontFile, iconFontFile);
```

### 字体族别名

```cpp
ui.text("t").fontFamily("Microsoft YaHei"); // Windows 中文
ui.text("t").fontFamily("PingFang SC");     // macOS 苹方
ui.text("t").fontFamily("monospace");       // 系统等宽
ui.text("t").fontFamily("Icon");            // 图标字体
ui.text("t").fontFamily("/path/to/font.ttf"); // 任意 TTF/OTF
```

### 系统 fallback（自动懒加载）

- Windows：`msyh.ttc`、`simhei.ttf`、`seguiemj.ttf`
- macOS：`Apple Color Emoji.ttc`、`Arial Unicode.ttf`、`Supplemental/Arial.ttf`
- Linux：`NotoColorEmoji.ttf`、`NotoSansCJK-Regular.ttc`、`DejaVuSans.ttf`

### ⚠️ 重要限制

- **`fontWeight` 数值传入，但 runtime 不会真正切换字重 face**（只打包了 Bold 一个文件）
- **没有 fontFamily 枚举 API**（不能列出系统已安装字体）
- 字体大小单位是逻辑像素（DIP），自动按 DPI 缩放

---

## 6. 图标系统

### 内置图标机制

把文字节点的 `fontFamily` 设为 `"Icon"`，文本设为 FontAwesome 字符：

```cpp
// codepoint
ui.text("icon").icon(0xF073); // fa-edit

// 字符串
ui.text("icon").icon("");

// 在 button 里
components::button(ui, "btn").icon(0xF00C).text("Save");
```

- 别名：`"Icon"` / `"FontAwesome"` / `"Font Awesome"` / `"Font Awesome 7 Free"`
- 用 `core::dsl::utf8(unsigned int codepoint)` 编码 codepoint

### Unicode Emoji

- U+1F000-U+1FAFF、U+2600-U+27BF 自动路由到彩色 emoji 字体
- macOS `Apple Color Emoji.ttc` / Windows `seguiemj.ttf` / Linux `NotoColorEmoji.ttf` 自动 fallback

### 自定义 SVG

```cpp
ui.svg("icon").source("<svg viewBox='0 0 24 24'>...</svg>")
    .size(24, 24)
    .tint(core::Color("#D97757"))
    .build();

// 或加载 SVG 文件
ui.image("icon").source("assets/myicon.svg");
```

- 解码器：nanosvg，dpi 96、units "px"
- **SVG 在绘制时被栅格化**（不是矢量渲染），无法在运行时无损放大

### 系统托盘（**严重受限**）

来源：`core/platform/tray_bridge.c`（35500 行，4 后端）

| 能力 | AppKit (macOS) | SNI (Linux) | WinAPI | AppIndicator |
|------|------|------|------|------|
| 设置图标 | ✅ | ✅ | ✅ | ✅ |
| 设置 tooltip | ❌ 硬编码 "EUI NEO" | ❌ | ❌ | ❌ |
| 自定义菜单 | ❌ 硬编码 "Show / Exit" | ❌ | ❌ | ❌ |
| 点击事件 | ✅ | ✅ | ✅ | ✅ |

**结论**：DevDesk 想要"切换 Profile"、"查看日志"等自定义托盘菜单项**不可行**——必须用窗口 UI 替代。

---

## 7. 关键交互能力

### 拖拽 API

来源：`core/dsl.h:756`、`components/mousearea.h`

```cpp
// 底层 onDrag
ui.rect("drag").interactive().onDrag([](const core::dsl::DragEvent& e) {
    // e.x, e.y, e.deltaX, e.deltaY
    // e.totalX, e.totalY
});

// 高层 mouseArea（推荐）
mouseArea(ui, "drag")
    .dragThreshold(8.0f)
    .suppressClickAfterDrag(true)
    .onDragStart([]{})
    .onDrag([](const MouseDragEvent& e) {})  // totalX, totalY
    .onDragEnd([](const MouseDragEvent&) {})
    .onTap([] { /* 区分点击 vs 拖拽 */ })
    .build();
```

**真实能做的**：
- 在元素按下并移动 → 持续触发 `onDrag`

**做不到的**：
- ❌ **没有 drop target API**（不能注册"接受 drop 的区域"）
- ❌ **没有跨元素 drop 事件**
- ❌ **没有 OS 文件拖入**

DevDesk 必须自实现 `DragController`（约 200 行）。

### Hover / 进入 / 离开

```cpp
ui.rect("btn").interactive().onHover([](bool hover) {});
mouseArea(ui, "btn").onHover(...).onEnter([]{}).onLeave([]{}).build();
```

### 焦点系统

```cpp
ui.rect("input").focusable()
    .preserveFocusOnPress(true)
    .onFocusChanged([](bool focused) {});

components::input(ui, "txt").value("...").build();  // 自动获得焦点 + IME
```

- 框架内部跟踪 `focusedId_`，KeyEvent 派发给焦点元素
- ⚠️ **Tab 焦点循环未实现**——自实现

### 键盘快捷键

```cpp
// 元素级
ui.rect("body").focusable().onKeyEvent([](const core::KeyEvent& key) -> bool {
    if (key.modifiers.shortcut() && key.key == core::InputKey::S) { save(); return true; }
    return false;
});

// 窗口级 fallback
DslAppConfig config;
config.onKeyEvent([](const eui::KeyEvent& key) { /* ... */ });
```

- `key.modifiers.shortcut()` 自动 Cmd（macOS）/ Ctrl（其他）
- 完整 InputKey：Backspace/Tab/Enter/Escape/方向键/F1-F24/numpad/修饰键
- ❌ **没有全局 OS hotkey**（窗口未聚焦时收不到）

### 右键菜单

```cpp
mouseArea(ui, "item").onContextMenu([](const MouseEvent& ev) {});

components::contextMenu(ui, "ctx")
    .items({"Cut", "Copy", "Paste",
            ContextMenuItem("Submenu", {"A", "B"})})
    .position(x, y)
    .screen(w, h)
    .bindOpen(openSignal)
    .onSelectPath([](const std::vector<int>& path) {})
    .build();
```

### IME（中文/日文输入）

来源：`core/platform/ime_bridge.c`（16944 行）

```cpp
ui.rect("input").focusable()
    .imeRect(caretX, caretY, w, h)
    .onTextInput([](const core::TextInputEvent& e) {
        // e.text / e.pasteText / e.compositionText / e.composing
    });
```

- macOS：swizzle `firstRectForCharacterRange:` + 读 `markedText` ivar
- Windows：IMM + Microsoft Pinyin 优化 + 强制 "Microsoft YaHei UI" ClearType
- Linux：no-op
- `components::input` **完整支持** IME 预编辑

### 文本选区（input 内置）

- `Cmd+A` / `Cmd+C` / `Cmd+X` / `Cmd+V`
- Shift + 方向键 / Shift + Home/End 选区
- 鼠标拖拽选区
- 撤销 / 重做栈（最多 128 步）

### 剪贴板

```cpp
std::string text = core::window::clipboardText(window);
core::window::setClipboardText(window, "copy text");
```

### 滚轮

```cpp
ui.rect("list").interactive().onScroll([](const core::ScrollEvent& e) {});
```

---

## 8. 动画与过渡

来源：`core/animation.h`（389 行）

### Transition（声明式）

```cpp
ui.rect("btn").color(c1)
    .transition(core::Transition::make(0.25f, core::Ease::OutCubic))
    .animate(core::AnimProperty::Color | core::AnimProperty::Transform)
    .build();
```

### Easing（8 种）

`Linear / InQuad / OutQuad / InOutQuad / OutCubic / InOutCubic / OutExpo / OutBack`

```cpp
core::setGlobalTransitionDurationScale(1.5f);  // 全局时长倍数
```

### AnimProperty 位掩码

`Frame | Color | TextColor | Opacity | Radius | Border | Shadow | Blur | Transform | All`

### 没有的关键帧 / spring

- ❌ imperative keyframes API
- ❌ spring physics API
- ❌ path/timeline 动画

### 替代方案

- `AnimatedValue<T>`（duration-eased，带 delay）
- `SmoothedValue<T>`（指数平滑 `1 - exp(-speed*dt)`，speed cap 32）—— 用于 hover/press blend 和 scroll inertia

### Scroll inertia

`runtime_state_bindings.h` 内置：
- `scrollInertiaFriction = 12.0`
- `scrollInertiaStopVelocity = 2.0`
- `scrollInertiaMaxVelocity = 7200.0`

### onTimer / onFrame

```cpp
ui.stack("tick").size(0, 0).onTimer(1.0f, []{ state.tick++; }).build();
ui.rect("anim").onFrame([](float dt) { /* 每帧 */ });
```

---

## 9. 反馈组件

### Toast

```cpp
components::toast(ui, "t")
    .visible(true)
    .bindVisible(visibleSignal)
    .duration(3.0f)
    .title("保存成功")
    .message("笔记已写入 inbox/ideas.md")
    .icon(0xF058)
    .build();
```

- ⚠️ **没有队列管理器**——单实例；多条要自实现队列

### Loading 状态

- `onTimer` + 改 state
- `components::progress` 0..1 进度条
- ❌ **没有 skeleton loader / shimmer 占位**

### 进度条

```cpp
components::progress(ui, "p").value(0.6f).build();
```

### 空状态

用通用组件自画（rect + text + icon）

---

## 10. DevDesk 功能核对表

| spec 功能 | 状态 | 实现路径 |
|----------|------|---------|
| 文件树 UI | ⚠️ 需自实现 | `virtualList` + `▶/▼` 字符 + 状态机（~150 行）|
| 多行编辑器 | ✅ 支持 | `components::input.multiline(true).scrollbar(true)` |
| 三栏布局 | ✅ 支持 | `ui.row { column(left) | column(mid) | column(right) }` |
| 卡片网格 / 看板列 | ✅ 支持 | `components::card` + `ui.row/column` 自实现 |
| 标签 chip（pill）| ✅ 支持 | `rect.radius(999).padding()` |
| 状态指示点 | ✅ 支持 | `rect.size(8,8).color().radius(8)` |
| 模态弹窗 | ✅ 支持 | `components::dialog` |
| 搜索框 + 实时过滤 | ✅ 支持 | `input` + `Signal<string>` 绑定 |
| 折叠/展开面板 | ⚠️ 需 hack | `state.expanded ? h : 0` + transition |
| 拖拽跨列 | ❌ 不支持 | 自实现 `DragController`（~200 行）|
| 文件拖入窗口 | ❌ 不支持 | "+ 新建"按钮 + `openFileDialog` |
| Toast | ✅ 支持（单实例）| `components::toast` |
| Loading | ✅ 支持 | `onTimer` + state |
| 进度条 | ✅ 支持 | `components::progress` |
| 系统托盘 | ⚠️ 部分支持 | 图标可设，菜单只有 Show/Exit |
| Cmd+S / Ctrl+N 快捷键 | ✅ 支持 | `onKeyEvent` 元素级 + 窗口级 fallback |
| IME 中文输入 | ✅ 支持（macOS/Windows，Linux no-op）| 三平台 bridge |
| 全文搜索 | ✅ 框架无关 | SQL FTS5 + 渲染列表 |
| 多窗口 | ✅ 支持 | `app::openWindow` |

---

## 11. 关键源码索引

| 文件 | 行数 | 用途 |
|------|------|------|
| `core/dsl.h` | 1934 | DSL 顶层 API（所有 builder）|
| `core/layout.h` | 702 | 布局算法 |
| `core/animation.h` | 389 | 过渡动画 |
| `components/input.h` | 517 | 输入框（含 IME）|
| `components/input_model.h` | 932 | 输入框数据模型 |
| `components/markdown.h` | 1719 | Markdown 渲染 |
| `components/dialog.h` | 247 | 模态弹窗 |
| `components/toast.h` | 196 | Toast |
| `components/tabs.h` | 146 | Tabs |
| `components/theme.h` | 276 | 主题系统 |
| `core/platform/ime_bridge.c` | 16944 | IME 三平台实现 |
| `core/platform/tray_bridge.c` | 35500 | 托盘 4 后端 |
| `include/eui/signal.h` | 42 | Signal（**仅 mutable wrapper，无 computed/effect**）|

---

## 12. 后续开发要注意的 5 件事

1. **写新组件前先 grep `components/`** —— 大概率已有，别造轮子
2. **配色直接用 hex** —— `core::Color("#D97757")`，无 CSS 变量
3. **字体只打包 Bold 一份** —— 不打包 SemiBold/Medium，字重靠 stroke / opacity 模拟
4. **拖拽自实现 DragController** —— 不要等"原生 API"
5. **测试用 mock upstream** —— 用 httpbin 或自己写 mock server（cpp-httplib 服务端）

---

## 13. 报告统计

- 覆盖 39 个 component 头文件 + 7 个 core 子系统 + 4 个 platform bridge + 11 个 example/app
- 所有结论都有源码出处
- 找出 **5 个 spec 之前未识别的硬限制** + **5 个隐藏陷阱**
- 给 DevDesk spec 的 5 处具体修订建议（已采纳）