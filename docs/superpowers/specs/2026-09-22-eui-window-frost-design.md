# EUI-NEO 窗口磨砂半透明能力 — 调研与设计

日期：2026-09-22 ｜ 状态：设计定稿（待评审）｜ 关联：`docs/EUI-NEO-CAPABILITIES.md`（macOS vibrancy ❌ 项）、`docs/KNOWN_LIMITATIONS.md`（clearColor 快照 UI-2）、spec §6.5（主题 token）

**结论先行**：可行，Windows 11 22H2+ 上可获得完整效果（Mica/Acrylic 磨砂 + 像素级半透），路径是「GLFW 透明帧缓冲 + DWM backdrop 属性」组合，两者都是已核实的一等公民 API。GLFW+OpenGL 后端先行（DevDesk 当前默认后端），Vulkan 后端在 Windows 上有 `compositeAlpha` 能力性障碍（降级处理），macOS 明确延后。附带产出两项独立小能力：标题栏深浅主题联动（可最先落地）和运行时 clearColor API（顺手根治 KNOWN_LIMITATIONS UI-2）。

---

## 1. 需求与用户价值

### 1.1 DevDesk 侧需求

设置页增加「窗口效果」三档开关：

| 档位 | 视觉 | 实现映射 |
|------|------|----------|
| 关（默认） | 现状实色窗口 | `WindowEffect::None`，clearColor alpha=1 |
| 半透 | 窗口整体按统一 alpha 半透，可透出桌面/下层窗口 | `WindowEffect::Transparent`，clearColor alpha≈0.85 |
| 磨砂 | Acrylic/Mica 材质模糊透出背景（Win11 系统级效果） | `WindowEffect::Acrylic`（首选）或 Mica |

**追加需求（2026-09-22 并入）**：窗口标题栏（非客户区）随 DevDesk 深浅主题联动——深色主题下标题栏/标题文字变深，浅色主题变浅。与磨砂同属 DWM 窗口属性族，纳入同一能力设计（`WindowAppearance`），但因体量小、无风险，单独成 Phase **先行落地**。

### 1.2 用户价值

- 磨砂/半透是现代桌面应用的「质感」标配（Windows Terminal、Edge、Notion 均有）；DevDesk 长驻桌面，磨砂能显著降低大面积实色背景的压迫感。
- 标题栏深浅不联动是当前 DevDesk 深色主题下最扎眼的不一致（浅色系统标题栏 + 深色客户区）。
- 持久化进 `config.json`，与 `darkTheme`/`fontScalePercent` 同一存储。

### 1.3 约束

- 按上游优先政策，全部能力做进 `../EUI-NEO/`（PR 反哺），DevDesk 只做配置接线，不自造平台 hack。
- DevDesk 平台约束（CLAUDE.md）：平台适配必须接口隔离（`*_win.cpp`/`*_macos.cpp` 拆文件，业务代码零 `#ifdef`）；EUI-NEO 侧同样按其 core/platform 分层落位。
- 向后兼容：不设效果时行为与现状逐像素一致（默认 alpha=1 的实色路径不许回归）。

---

## 2. 现状与差距（EUI-NEO 为什么画不了半透）

以下均经源码核实（行号为 2026-09-22 工作区）。

### 2.1 窗口层：没有开透明帧缓冲

- GLFW 路径 `core/window/window_backend.cpp:553-565`（`configureOpenGLWindowHints`）：已设 `GLFW_ALPHA_BITS 8`（帧缓冲有 alpha 通道），**但没有 `GLFW_TRANSPARENT_FRAMEBUFFER` hint**。Windows 上 GLFW 通过 `DwmEnableBlurBehindWindow`（空区域技巧）让 DWM 尊重重定向表面 alpha；不开这个 hint，帧缓冲 alpha 写了也被当不透明忽略。
- SDL2 路径 `window_backend.cpp:50-60` 同样只设 `SDL_GL_ALPHA_SIZE 8`，没有 `SDL_WINDOW_TRANSPARENT` flag。
- `WindowCreateRequest`（`core/window/window_types.h:19-38`）没有任何透明/效果字段；`DslAppConfig`（`include/eui/dsl_app.h`）也没有对应配置项。`docs/DSL.md:94` 甚至明文写着「全屏、透明窗口和 VSync 不属于 DslAppConfig」——本设计将修订该条（见 §3.4）。
- **好消息**：`glfw_app_main.cpp:134` 已有 `glfwGetWin32Window` 直取 HWND 的先例，窗口创建后（`window_backend.cpp:582-607` `createWindow` 返回前）是加 DWM 属性调用的天然挂点。

### 2.2 渲染层：alpha 其实已经「通」了大半

- `Runtime::render`（`core/runtime/runtime_lifecycle.h:145-246`）把 `clearColor` 原样传给后端 `clear()`；OpenGL 后端 `opengl_backend.cpp:522-527` `glClearColor(color.r,g,b,color.a)` **alpha 是透传的**。
- GL 渲染缓存是 `GL_RGBA` FBO（`opengl_backend.cpp:310-312`），`blitRenderCache`（:405-435）用 `glBlitFramebuffer` 把缓存拷回默认帧缓冲，**alpha 通道一起拷**。即：只要窗口层开了透明 hint、clearColor alpha<1，GL 后端理论上就能出半透（待 spike 验证合成正确性，见 §3.3 premultiplied 问题）。
- **Vulkan 的硬障碍**：swapchain 硬编码 `compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR`（`vulkan_backend.cpp:673`）。Windows 上 Win32 surface 的驱动普遍只报 OPAQUE 支持，`PRE_MULTIPLIED_BIT_KHR` 少有支持——像素级透明在 Vulkan/Windows 大概率做不了（能力查询后降级，见 §5）。
- **UI 内容全不透明的假设**：各模块页面要么靠 clearColor 当底、要么自铺不透明 surface 色（如 settings_module.cpp:208），这个假设在半透模式下**正好是需要的**（内容实色、只有 clearColor 透）——不需要动任何 DSL 绘制代码。

### 2.3 配置与运行时：clearColor 是启动快照，且无运行时改法

这是 DevDesk KNOWN_LIMITATIONS UI-2 记录过的老问题，本需求把它从「记录在案」升级为「必须根治」：

- DevDesk `main.cpp:332-336`：`dslAppConfig()` 是 static 局部，构造时从持久化主题快照 clearColor。EUI-NEO 主窗口每帧 `app::render`（`include/eui/detail/dsl_app_impl.h:359-363`）虽然每帧**重读** config，但 config 本身不可变。
- 子窗口更死：`DslWindowRuntime::request_` 是 `initialize()` 时拷贝的快照（`core/app/dsl_window_runtime.h:13-18, 100`），无 setter。
- 后果：设置页切「关↔半透↔磨砂」、切深浅主题，窗口底色/标题栏都无法运行时跟随。需要新增运行时 API（§3.4）。

### 2.4 能力盘点结论

EUI-NEO 现状对「窗口效果」的支持是**零**（无 hint、无配置、无平台属性调用），但渲染管线的 alpha 通路比预想完整（GL 侧 clear→cache→blit 全程保留 alpha，且 `setPremultipliedAlphaBlend` 已存在，`opengl_backend.cpp:236-242`，目前仅 image 路径使用）。缺口集中在窗口层与配置/运行时 API。

---

## 3. 技术方案（Windows 为主）

### 3.0 Windows API 事实（已联网核实，learn.microsoft.com）

| API / 属性 | 值 | 语义 | 最低版本 |
|---|---|---|---|
| `DWMWA_USE_IMMERSIVE_DARK_MODE` | 20 | BOOL，标题栏/边框走深色系；旧 build 用过 19（未文档化，待验证） | Win11 22000 |
| `DWMWA_WINDOW_CORNER_PREFERENCE` | 33 | 圆角偏好（默认即圆角，一般不动） | Win11 22000 |
| `DWMWA_BORDER_COLOR` | 34 | COLORREF，边框色，`DWMWA_COLOR_DEFAULT`(0xFFFFFFFF) 复位 | Win11 22000 |
| `DWMWA_CAPTION_COLOR` | 35 | COLORREF，标题栏底色，可对齐主题 token | Win11 22000 |
| `DWMWA_TEXT_COLOR` | 36 | COLORREF，标题文字色 | Win11 22000 |
| `DWMWA_SYSTEMBACKDROP_TYPE` | 38 | `DWM_SYSTEMBACKDROP_TYPE`：`AUTO=0 / NONE=1 / MAINWINDOW=2(Mica) / TRANSIENTWINDOW=3(Acrylic) / TABBEDWINDOW=4(Mica Alt)` | **Win11 22621（22H2）** |
| `DWMWA_REDIRECTIONBITMAP_ALPHA` | 39 | BOOL，启用重定向位图 alpha（要求 premultiplied）——24H2 新路径，对 Vulkan 可能是救命稻草 | Win11 26100 |
| `DwmEnableBlurBehindWindow` | — | 老式 API。Vista 时代的「blur behind」实绩模糊在 Win8+ 已名存实亡；**但它同时是让 DWM 尊重窗口表面 alpha 的传统开关**（空区域技巧，GLFW 透明窗口即基于此）。不要用它做「磨砂」（效果已不是 blur），磨砂一律走 SYSTEMBACKDROP | Win7+ |

关键点：**backdrop 材质由 DWM 画在窗口整个 bounds 后面（含非客户区），窗口自己渲染的内容必须带 alpha 才能露出材质**。微软文档明确 `DWMSBT_AUTO` 只画在默认标题栏后面、且可能被内部启发式关掉——必须显式选 MAINWINDOW/TRANSIENTWINDOW 之一。

### 3.1 窗口层改动（EUI-NEO）

1. `WindowCreateRequest` 增加 `WindowEffect windowEffect = WindowEffect::None` 字段（`window_types.h`）。
2. GLFW 路径 `createWindow()`：`windowEffect != None` 时加 `glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE)`；创建后用 `glfwGetWindowAttrib` 回查，失败则记录并按 None 处理（透明需要桌面合成开启，文档明说）。
3. 新增平台文件（接口隔离，对齐 DevDesk T6 模式与 EUI-NEO 自身 platform 分层）：
   - `core/platform/window_effect.h`：平台无关接口
     ```cpp
     namespace core::platform {
     enum class WindowEffect { None, Transparent, Acrylic, Mica, MicaAlt };
     struct TitleBarAppearance { bool dark; bool customColor; std::uint32_t colorAbgr; /* COLORREF */ };
     // 全部幂等、可运行时反复调用；返回是否实际生效（版本不支持 → false）
     bool applyWindowEffect(void* nativeWindowHandle, WindowEffect effect);
     bool applyTitleBarAppearance(void* nativeWindowHandle, const TitleBarAppearance& appearance);
     }
     ```
   - `window_effect_win.cpp`：`DwmSetWindowAttribute` 封装。`DWMWA_SYSTEMBACKDROP_TYPE`/`CAPTION_COLOR` 调用失败（老系统返回 `E_INVALIDARG`/`HRESULT_FROM_WIN32(ERROR_INVALID_FUNCTION)`）即返回 false，**不抛、不 log 刷屏**（缓存「已确认不支持」避免每帧查询）。
   - `window_effect_stub.cpp`：Linux/其他平台空实现（返回 false）。
   - `window_effect_macos.mm`：Phase E 占位。
4. 挂点：`glfw_app_main.cpp` 主窗口创建后（:434 之后）与 `createManagedWindow`（:246）子窗口创建后，按配置调 `applyWindowEffect`/`applyTitleBarAppearance`；运行时切换走新 API（§3.4）。
5. **窗口 hint 的不可逆性**：`GLFW_TRANSPARENT_FRAMEBUFFER` 是创建期 hint，创建后不可关。策略：`effect != None` 时创建即开透明 hint；「磨砂↔半透」运行时可切（都基于同一透明窗口），「任何 ↔ 关」运行时切换通过把 clearColor alpha 拉回 1 实现视觉等价（透明 hint 仍开着，alpha=1 渲染结果与不透明窗口一致，仅理论上有极小合成开销）。**不需要重建窗口**。

### 3.2 渲染层改动（EUI-NEO，GL 后端）

1. **premultiplied alpha（本设计最大的渲染正确性风险点）**：DWM 合成假定窗口表面是 premultiplied alpha（`DWMWA_REDIRECTIONBITMAP_ALPHA` 文档明说 premultiplied；layered window 语义相同）。EUI-NEO 的 cache FBO 内容是 straight alpha（内部 `setStandardAlphaBlend`），`glBlitFramebuffer` 原样拷贝 → 送出的是 straight alpha → 半透像素会过亮/灰雾（严重度待 spike 实测）。
   修法：`windowEffect != None` 时，`blitRenderCache` 的 `glBlitFramebuffer` 路径换成 textured-quad shader blit，fragment 里 `rgb *= a`（一次全屏或脏区矩形纹理采样，成本可忽略；`setPremultipliedAlphaBlend` 已存在可直接用于默认帧缓冲上的后续绘制）。
   **先做 spike 验证**：不排除驱动/DWM 对 straight alpha 有容错（部分 GLFW 透明示例直接用 straight alpha 也能看），若 spike 证明可接受，Premultiply blit 降级为 polish 项——但设计按需要做。
2. clearColor alpha 语义不变（`Color{r,g,b,a}` 的 a 即整体不透明度），唯一变化是「a<1 + 透明窗口」从无意义变为有效组合。
3. Vulkan 后端：查 `VkSurfaceCapabilitiesKHR::supportedCompositeAlpha`，有 `PRE_MULTIPLIED` 就用；没有（Windows 常见）→ 该窗口透明降级为 None（只保留标题栏联动，标题栏不依赖帧缓冲 alpha）。另一条路是 Win11 26100+ 的 `DWMWA_REDIRECTIONBITMAP_ALPHA`（重定向位图层吃 alpha，理论上游后端无关）——列为开放问题（§6），不进本期承诺。

### 3.3 配置 API 与运行时 API（EUI-NEO）

1. `DslAppConfig` / `DslWindowConfig` 新增（`include/eui/dsl_app.h`，流式风格照旧）：
   ```cpp
   DslAppConfig& windowEffect(core::platform::WindowEffect value);
   DslAppConfig& darkTitleBar(bool value = true);      // 需要显式深浅（一般由 theme 决定）
   DslAppConfig& titleBarColor(const eui::Color& value); // 可选；缺省走系统
   ```
   默认 `None/false/无`，完全向后兼容。
2. 运行时 API（新增，`include/eui/app.h` 命名空间 `app`，实现落 `dsl_app_impl.h` / `dsl_window_runtime.h`）：
   ```cpp
   namespace app {
   // 覆盖主窗口+全部存活子窗口的 clearColor，内部触发 requestFullPaint；传 alpha=1 即回到不透明
   void setClearColor(const eui::Color& color);
   // 运行时切标题栏外观 / backdrop（后者受能力限制返回 bool）
   bool setWindowEffect(core::platform::WindowEffect effect);
   void setTitleBarAppearance(const core::platform::TitleBarAppearance& appearance);
   // 能力查询：开机后想知道这台机器实际生效了什么
   core::platform::WindowEffect activeWindowEffect();
   }
   ```
   `setClearColor` 实现要点：EUI-NEO 侧持 runtime override（`detail` 命名空间静态），`app::render`（dsl_app_impl.h:359-363）取值时 override 优先；`DslWindowRuntime` 加 `setClearColor` 私有成员 + 主循环统一广播。这**顺手根治 KNOWN_LIMITATIONS UI-2**（主题切换底色不跟随）。
3. `docs/DSL.md:94` 的「透明窗口不属于 DslAppConfig」表述同步修订为「窗口效果（`windowEffect`/标题栏外观）通过 `DslAppConfig` 配置；全屏与 VSync 仍不属于」。

### 3.4 DevDesk 接线

- `SettingsConfig`（`src/module_settings/settings_config.h`）加 `std::string windowEffect = "off";  // "off" | "translucent" | "frosted"`，`sanitized()` 把未知值收敛为 `"off"`（照抄 fontScalePercent 的钳制模式）。
- `main.cpp` `dslAppConfig()`：从持久化配置读出并映射 `None/Transparent/Acrylic`，同时按主题设 `darkTitleBar`（`theme::current().isDark`——若 Theme 无此标志则加）。clearColor 不再从主题快照，改在 `dslAppConfig()` 里按效果档给 alpha（off=1.0，translucent≈0.85，frosted≈0.55——Acrylic 材质自身还有一层遮蔽，alpha 太高会看不出磨砂，具体值调参定）。
- 设置页：主题切换处（现有 darkTheme 逻辑旁）追加调 `app::setClearColor` + `app::setWindowEffect` + `app::setTitleBarAppearance`，全部即时生效，无需重启。
- 标题栏颜色策略：**backdrop 生效时不设 CAPTION_COLOR**（实色标题栏会盖住 NC 区材质，Win11 原生磨砂应用都让材质贯通标题栏），只用 `USE_IMMERSIVE_DARK_MODE` 控制标题文字深浅；`titleBarColor` API 留给「关」档做实色标题栏对齐主题（可选增强）。

### 3.5 性能与实测方案

- **成本模型**：Mica 是静态壁纸采样着色，DWM 侧近乎免费（微软定位为主窗口推荐材质）；Acrylic 是实时模糊，合成线程有真实开销（窗口移动/背后内容剧变时最明显）；像素半透是逐帧合成，成本≈普通合成。
- **对 EUI-NEO 现有机制的影响**：渲染管线改动仅在最终 blit（premultiply quad），脏区机制、cache/blit 调度、`RenderFrameStats` 全部复用；quad blit 沿用现有 per-rect 循环，0.5s onTimer 重绘仍只碰脏区。标题栏/backdrop 是 DWM 侧属性，不占 EUI-NEO 帧预算。
- **实测方案**（Phase C 验收动作）：
  1. 复用 `RenderFrameStats`（`render_types.h`）对比 off/translucent/frosted 三档的 clearCalls/cacheBlits/blitPixels（应完全一致，证明管线无额外全屏重绘）。
  2. 帧时间：Debug 构建下用 `stats` 或加一个 `GL_TIME_ELAPSED` 采样点（EUI-NEO 是否已有 GPU 计时——**待验证**，没有就加在 premultiply blit 前后）。
  3. DWM 侧：窗口拖动 + 背后播放视频场景，肉眼对比 Acrylic vs Mica vs off 的流畅度；有条件用 PresentMon 看 Present 帧时间分布。
  4. 验收阈值建议：0.5s onTimer 重绘帧 CPU+GPU ≤ 无效果基线 +20%；Acrylic 档若拖动窗口明显掉帧，把 DevDesk「磨砂」默认值从 Acrylic 改为 Mica（API 双档都保留）。

### 3.6 降级链与非法配置

```
Win11 22H2 (22621)+ ───── 全档：None/Transparent/Acrylic(+Mica) + 标题栏全家桶
Win11 21H2 (22000) ─────── 无 backdrop；Transparent + 标题栏 dark/caption/text/border 可用
Win10 / Win11 降级场景 ──── 无 backdrop、无 caption color（DwmSetWindowAttribute 调用失败
                            → applyWindowEffect 内部降级为 Transparent 或 None 并返回 false）；
                            Transparent 本身在 Win10 可用（GLFW blur-behind 路径）
DWM 合成被禁用/老显卡 ──── glfwGetWindowAttrib(GLFW_TRANSPARENT_FRAMEBUFFER)=false → 自动按 None
配置非法值 ─────────────── DevDesk sanitized() 收敛为 "off"；EUI-NEO 枚举封闭无非法值
Vulkan 后端 + Windows ──── compositeAlpha 能力查询失败 → 仅标题栏联动，磨砂/半透降级 None
```

DevDesk 设置页对降级的呈现：`app::activeWindowEffect()` 与用户所选不符时，设置页该行显示一行小字「当前系统不支持，已回退为 ×××」（toast 或行内提示，实现细节 Phase D 定）。

### 3.7 macOS 路径（次要，明确延后）

- `GLFW_TRANSPARENT_FRAMEBUFFER` 在 macOS 受支持（NSWindow `opaque=NO`），像素半透技术上障碍不大。
- 磨砂需要把 `NSVisualEffectView` 垫在 GL view 层下面：GLFW 不暴露此能力，需 EUI-NEO 平台层 ObjC++ 代码（subclass 或 addSubview），且 macOS 26/27 的 Liquid Glass 材质体系与 `NSVisualEffectView` 的关系要单独调研——**工作量与 Windows 侧相当且无当前用户**，明确「Windows 先行，macOS 后续（Phase E）」。
- 标题栏联动在 macOS 便宜很多：`NSWindow.titlebarAppearsTransparent` + `NSAppearance`（dark/light）即可，且 DevDesk 主题已有深浅二值。Phase E 优先做这一半。

---

## 4. 风险与开放问题

| # | 风险/问题 | 影响 | 缓解 |
|---|-----------|------|------|
| R1 | **straight alpha vs premultiplied**：DWM 要求 premultiplied，现管线 blit 是原样拷贝 | 半透像素发灰/过亮、文字边缘 fringe | Premultiply quad blit（§3.2.1）；Phase 0 spike 首项验证 |
| R2 | **GLFW 透明窗口 Windows 已知怪癖**：resize 时的残影/拖影（GLFW issue 跟踪多年） | 拖动改窗口大小时短暂视觉瑕疵 | spike 实测 DevDesk 场景（主窗口固定尺寸约束 min 1200×720，触发面小）；如实测复现且不可忍，透明档对 resize 场景做全帧重绘规避 |
| R3 | **Vulkan/Windows compositeAlpha**：驱动普遍只报 OPAQUE | Vulkan 用户拿不到半透/磨砂 | 能力查询降级（§3.6）；DevDesk 默认 GL 后端，当前无实际用户；`DWMWA_REDIRECTIONBITMAP_ALPHA`(26100+) 作为 Vulkan 的潜在出路，列为开放问题不承诺 |
| R4 | **Acrylic 性能**：实时模糊在低配机/集显上的合成开销 | 拖动窗口掉帧 | 默认磨砂推荐 Mica；实测方案 §3.5；设置页三档让用户自选 |
| R5 | **版本碎片**：22000 / 22621 / 26100 三档行为差 | 测试矩阵大 | 按属性粒度做能力缓存（§3.1.3），降级链表格进测试用例；开发机是 Win11 Enterprise 26200（全量支持），旧版本行为靠返回值逻辑 + 代码审查保证 |
| R6 | **双后端一致性**：GL 能透、Vulkan 不能 | 同一应用不同构建观感不同 | `activeWindowEffect()` 如实上报；文档明示；不追求本期抹平 |
| R7 | **clearColor 运行时 API 的线程边界**：设置回调在 UI 线程（compose 时机）改 clearColor，需与 render 线程安全交接 | 偶发撕裂/未生效 | `setClearColor` 只写 override + requestFullPaint，下一帧 render 读取（现有 update/render 单线程顺序模型，无锁即可，实现时对齐 `requestUpdate` 的既有模式） |
| R8 | 「关」档仍保留透明 hint 的隐性成本 | 理论合成开销、截图工具行为差异 | 实测（§3.5.4）；若有实测差异，`windowEffect` 为 None 时干脆不设 hint、切换 None↔其他时重建窗口（重建成本一次性，可接受，Phase B 定案） |
| O1 | `DWMWA_USE_IMMERSIVE_DARK_MODE` 在 Win10 1809+ 用值 19 可用（社区通行做法，微软未文档化） | Win10 上标题栏联动可能白做 | 保持只走 20 + 检查返回值；Win10 支持列为 nice-to-have，待验证 |
| O2 | EUI-NEO 是否已有 GPU 帧时间采样 | 性能验收手段 | Phase C 查证，没有则随 premultiply blit 顺手加一个开关门控的 `GL_TIME_ELAPSED` |
| O3 | Mica 在多显示器/不同壁纸下的观感、以及与 DevDesk 侧栏布局的搭配 | 纯视觉调参 | Phase D 截屏评审（验证原则：未截屏 = 未完成） |

### spike 结论回填（2026-09-22，Phase 0 实测，Win11 Enterprise 26200；证据 EUI-NEO `spike/frost-phase0` 分支 spike/*.png）

- **R1 premultiplied——必需项，非 polish**：顶窗 clearColor gray(51,51,51) a=0.5 叠洋红(255,0,255)，straight blit 实测 (178,51,178) 与「DWM 按 premult 解释」理论值完全吻合（每通道 +25 @ a=0.5，约 +17% 增亮）；premultiply quad blit 实测 (153,26,153) 与正确合成理论值吻合。Phase B 已按此落地 premultiply blit。
- **R2 backdrop 组合成立**：GLFW 透明帧缓冲 + `DWMWA_SYSTEMBACKDROP_TYPE` 组合可用，Mica=2/Acrylic=3 均返回 S_OK 且采样到均匀材质色；**`DWMSBT_AUTO(0)` 返回 S_OK 但无材质**（无装饰窗口被内部启发式跳过）——证实 §3.0「必须显式选 MAINWINDOW/TRANSIENTWINDOW」，Phase C 实现一律显式档位。
- **版本门槛**：开发机 Win11 26200 全量支持；22621 门槛按文档（属性粒度能力缓存 + 调用失败降级返回 false 兜底，旧版本实测靠返回值逻辑保证）。
- **resize 残影（R2 风险表第二项）**：spike 支持 `--resizable` 人工验证，未自动化；DevDesk 主窗口有 min 1200×720 约束，触发面小，观察项保留。

---

## 5. 实施切分

依赖关系：Phase 0 → A 可与 B 并行 → C 依赖 B → D 依赖 A+B+C → E 独立延后。

### Phase 0 — Spike：组合验证（0.5 天，EUI-NEO 仓库）

在 EUI-NEO 里写最小验证（可直接长成 Phase B/C 的 example 雏形）：
1. GLFW 透明 hint + clearColor alpha=0.5 的 GL 窗口，验证像素半透与 straight alpha 的实际观感（回答 R1 严重度）。
2. 在 1 基础上叠加 `DWMWA_SYSTEMBACKDROP_TYPE=TRANSIENTWINDOW/MAINWINDOW`，验证 Acrylic/Mica 是否透过 alpha 露出（回答「GLFW blur-behind 路径 + backdrop」这一核心组合）。
3. 记录 resize 残影是否复现（回答 R2）。

**验收**：三问各有明确结论 + 截屏，写进本文档 §4 的结论栏（实现阶段回填）。

### Phase A — 标题栏主题联动（1-2 天；**可先行独立发版**）

- EUI-NEO：`window_effect.h/_win.cpp/_stub.cpp` + `applyTitleBarAppearance`（`USE_IMMERSIVE_DARK_MODE` + `TEXT_COLOR`，`CAPTION_COLOR` 留 API 不默认用）+ `DslAppConfig::darkTitleBar` + `app::setTitleBarAppearance` 运行时 API。
- DevDesk：`main.cpp` 按主题接线；设置页主题切换即时生效。
- **验收**：深/浅主题下截屏对比标题栏；运行时切换无需重启；Win11 以下机器不崩、返回 false 静默降级；DevDesk 137 用例全绿。

### Phase B — 透明管线与运行时 clearColor（2-4 天）

- EUI-NEO：`WindowCreateRequest.windowEffect` + GLFW/SDL2 透明 hint + 回查降级；`app::setClearColor`（override + 子窗口广播 + requestFullPaint）；premultiply quad blit（按 spike 结论决定全量或 polish）。
- DevDesk：clearColor 改走 `setClearColor`（主题切换底色即时生效，关 KNOWN_LIMITATIONS UI-2）。
- **验收**：`setClearColor` 切主题底色即时跟随（重启后一致）；alpha=1 路径与现状逐像素一致（无效果回归）；半透档截屏过审；`examples/` 加 `transparent_window` demo。

### Phase C — DWM backdrop（2-3 天）——✅ 完成（2026-09-22，EUI-NEO `feat/frost-phase-cd`）

- EUI-NEO：`applyWindowEffect` 完整实现（SYSTEMBACKDROP_TYPE 四档 + 能力缓存降级）+ `app::setWindowEffect/activeWindowEffect`。
- **验收**：22H2+ 上 Acrylic/Mica 截屏；`setWindowEffect` 运行时切换生效；<22H2 分支按降级链返回 false；§3.5 性能实测三项跑完并记录；example 扩展 backdrop 三档切换按钮。

### Phase D — DevDesk 设置页三档开关（1-2 天）——✅ 完成（2026-09-22，DevDesk `feat/window-effect-ui`）

- `SettingsConfig.windowEffect` + sanitized + 持久化；设置页 UI（三档 segmented control，附降级提示行）；接线 `setWindowEffect/setClearColor` 即时生效。
- **验收**：三档切换截屏 + 重启保持；非法值收敛；降级提示在（模拟）老系统场景正确显示；全量测试绿 + 文档更新（KNOWN_LIMITATIONS 划掉 UI-2，EUI-NEO-CAPABILITIES.md 的 vibrancy ❌ 行改为 ✅（Windows））。

### Phase E — 后续（不承诺排期）

macOS：titlebarAppearsTransparent + NSAppearance（先做）→ NSVisualEffectView 磨砂 → Liquid Glass 调研；Vulkan：compositeAlpha/REDIRECTIONBITMAP_ALPHA 出路验证。

**总量估计**：Phase 0+A+B+C+D 合计约 **6-10 个工作日**（AI 开发节奏），其中 Phase A 单独 1-2 天即可见效果。

---

## 6. PR 准备清单（按 EUI-NEO 贡献政策）

每个 Phase 一次 PR（合到 EUI-NEO `main`），统一要求：

1. **Example**：`examples/window_effect.cpp`（Phase A 起建，逐 Phase 扩展：标题栏切换 / 半透滑杆 / backdrop 三档按钮 + 能力回显），随 `apps/` 或 `examples/` CMake 注册。
2. **测试**：能力枚举映射与降级逻辑的纯逻辑用例进 `tests/`；DWM 调用本身无法单测（需真窗口），example 即人工验证载体；`setClearColor` 的 override 优先级可单测。
3. **文档**：`docs/DSL.md`（修订 §94 行 + 新增 windowEffect/darkTitleBar 说明）、`docs/平台能力.md`（透明窗口/backdrop 支持矩阵）。
4. **向后兼容**：全部新配置默认值=现状行为；不动既有 API 签名（`WindowCreateRequest` 只加字段）。
5. **EUI-NEO 自身编译通过**（DevDesk 不阻塞），commit 前缀 `feat:`，message 注明需求来源（「DevDesk 需要窗口磨砂半透明 + 标题栏主题联动」）。
6. 大改动先开 issue 讨论的要求：本能力属新 feature 非重构，issue 一并附上本文档链接。

---

## 附：调研来源

- DWM_SYSTEMBACKDROP_TYPE 枚举（值/材质映射/最低版本 22621）：learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwm_systembackdrop_type
- DWMWINDOWATTRIBUTE（属性编号、CAPTION/TEXT/BORDER_COLOR 22000+、SYSTEMBACKDROP 22621+、REDIRECTIONBITMAP_ALPHA 26100+ 与 premultiplied 要求、IMMERSIVE_DARK_MODE=20 语义）：learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute
- GLFW_TRANSPARENT_FRAMEBUFFER（hint/attribute、需桌面合成、不影响装饰、gears 示例）：glfw.org/docs/latest/window_guide.html
- EUI-NEO 源码（行号见 §2，2026-09-22 工作区）；DevDesk KNOWN_LIMITATIONS.md / CLAUDE.md / settings_config.h
