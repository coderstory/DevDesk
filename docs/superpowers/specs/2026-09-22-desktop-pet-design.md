# 桌宠功能 — 调研与设计

日期：2026-09-22 ｜ 状态：设计定稿（待评审）｜ 关联：`2026-09-22-eui-window-frost-design.md`（透明管线 Phase B）、`2026-09-22-module-plugin-architecture-design.md`（模块插拔方案 A）、`docs/EUI-NEO-CAPABILITIES.md`（多窗口 ✅ 项）

**结论先行**：可行，推荐形态是**精灵图序列帧（sprite PNG + alpha 通道，格式数据驱动、兼容 shimeji 生态）+ 独立无边框置顶子窗口**，走 `app::openWindow` 多窗口通路，**Windows 11 与 macOS 27+ 双平台覆盖**（核心 hint 组合 `DECORATED=0 + TRANSPARENT_FRAMEBUFFER + FLOATING + MOUSE_PASSTHROUGH + FOCUS_ON_SHOW=0` 在 GLFW 层两平台同构，差异只在 platform 层：Windows 补 `WS_EX_TOOLWINDOW`，macOS 补全桌面空间常驻等 NSWindow 属性，按 `*_win.cpp`/`*_macos.mm` 接口隔离惯例拆文件）。**选型标准（用户 2026-09-22 最新决策）：纯按技术优劣、实现成本、效果质量评估，不设授权/开源合规约束**——Live2D、Spine、Rive 付费档全部回到桌面重新权衡（§3.1）；维持序列帧推荐的核心理由是**实现成本最低（~200 行帧播放器）+ 素材渠道现成（CC0 序列帧资源 + shimeji 生态）+ 与 EUI-NEO 现有能力零缝隙对齐**，Rive（MIT 运行时 + 状态机天然适合行为驱动）与 Spine/Live2D 列为表现力升级路径而非起步路径（§3.2 给出升级触发条件）。桌宠的透明需求与磨砂设计共享 **Phase B（透明帧缓冲管线）** 的全部基础设施（`GLFW_TRANSPARENT_FRAMEBUFFER` hint + premultiplied blit），**被 Phase B 阻塞、不被 Phase C（DWM backdrop）阻塞**——桌宠不需要磨砂材质，只要像素级透明。EUI-NEO 侧需一次约 6 项小扩展的上游 PR（子窗口窗口级配置透传 / setWindowPos / 鼠标穿透 / 不抢焦点 / 任务栏隐藏 / 子窗口最小尺寸下限放开），DevDesk 侧按模块插拔方案 A 的模式新增 `module_pet`（可插拔，关掉 = 不开窗口 = 零执行）。MVP（静态/眨眼 sprite + 拖拽 + 点击反应 + 位置持久化）估计 **3-5 个工作日**（含 EUI-NEO 上游扩展），全量（状态机/气泡/菜单/皮肤）另加 3-4 天。

---

## 1. 需求与用户价值

### 1.1 需求（用户原话转述）

增加桌宠功能：常驻桌面的小动物/角色，可交互。

拆成可验收的语义：

1. **常驻**：DevDesk 启动后（或用户开启后），一个独立小窗口浮在桌面上，置顶、无边框、不占任务栏。
2. **透明**：窗口背景全透，只显示角色本体（非矩形）。
3. **可交互**：可拖拽移动、点击有反应（被戳/被拍）、右键出菜单。
4. **可关闭**：桌宠是可选功能，关掉后与没有这个功能完全一致（对齐模块插拔方案 A 的「关闭 = 零执行」语义）。
5. **持久化**：位置、皮肤、开关状态重启保持。

### 1.2 用户价值

- DevDesk 定位「开发者桌面工具集」，长驻系统托盘；桌宠是低成本的情感化入口，与 Home 仪表盘互补（一个管效率，一个管陪伴）。
- 未来可挂真实功能：气泡提醒（Todo 逾期、代理异常）、点击宠物跳转模块——本设计把这些列为进阶，不进 MVP 承诺。

### 1.3 约束

- 单 exe 零外部 DLL（T3 发布形态）——排除 CEF/Electron 壳类方案。
- **选型标准（用户 2026-09-22 最新决策）**：纯按技术优劣、实现成本、效果质量评估，**不设授权/开源合规约束**（早先「尽可能开源」的要求已作废，Live2D/Spine/Rive 全部回到桌面）；素材授权从「一票否决项」降为工程偏好——CC0/CC-BY 仍是首选（零义务/低摩擦），但不再是硬门槛。
- **双平台（硬要求）**：DevDesk 目标平台是 Windows 11 + macOS 27+，桌宠技术设计必须双平台覆盖；平台差异按仓库 `*_win.cpp`/`*_macos.mm` 接口隔离惯例落位（EUI-NEO 侧对齐其 platform 分层，DevDesk 侧对齐 T6 模式）。
- 不抛 C++ 异常，错误走 `Result<T>`；平台适配接口隔离（`*_win.cpp` 拆文件，业务代码零 `#ifdef`）。
- EUI-NEO 上游优先：窗口能力缺口修 `../EUI-NEO/`，DevDesk 只做接线。

---

## 2. EUI-NEO 能力现状与缺口（源码核实，2026-09-22 工作区）

### 2.1 多窗口：通路存在，但子窗口配置面太窄

- **API 实态**：`app::openWindow(const DslWindowConfig&, DslWindowCompose)`（`include/eui/dsl_app.h:189`）→ `DslWindowRequest`（`include/eui/app.h:16-25`）→ 主循环 `createRequestedWindows`（`core/app/glfw_app_main.cpp:411-419`）→ `createManagedWindow`（:264-307）。子窗口有独立 `RenderBackend`（与主窗口共享 GL context）、独立 `DslWindowRuntime`、独立输入回调（`installInputCallbacks`）。**开第二个窗口成立，且子窗口可以有自己的 compose / clearColor / onKeyEvent。**
- **缺口（关键）**：`DslWindowConfig`（dsl_app.h:159-184）只有 `title/pageId/clearColor/windowSize/modal/onKeyEvent` 六项。`createManagedWindow` 组装 `WindowCreateRequest` 时（glfw_app_main.cpp:267-273）**不设 decorated / alwaysOnTop / resizable / position**，全部落到 `WindowCreateRequest` 默认值（decorated=true, alwaysOnTop=false, resizable=true, window_types.h:19-38）——即子窗口目前**必然带标题栏、必然不置顶、必然可缩放**。而后端层 `WindowCreateRequest` 其实已支持这四项（`window_backend.cpp:577-580` 已正确设 `GLFW_DECORATED/GLFW_FLOATING/GLFW_RESIZABLE`，:604-606 支持 `glfwSetWindowPos`）。**纯接线缺口，不是能力缺口。**
- 另一硬伤：`openWindow` 实现里 `request.width = std::max(160, ...)` / `std::max(120, ...)`（`dsl_app_impl.h:149-150`）——子窗口最小 160×120，桌宠 128×128 的窗口会被强行放大。需要放开（配置化或下调）。

### 2.2 透明窗口：与磨砂 Phase B 完全共享

磨砂设计文档（§2.1/§3.1）已论证的通路：`GLFW_TRANSPARENT_FRAMEBUFFER` hint（当前未设）+ clearColor alpha 通道（渲染管线 GL 侧 clear→cache→blit 全程保留 alpha）+ premultiplied alpha 问题（R1，DWM 合成要求 premultiplied，现管线 blit 是 straight alpha）。

桌宠的透明需求与之对照：

| 需求 | 磨砂设计中的归属 | 桌宠是否需要 |
|------|------------------|--------------|
| `GLFW_TRANSPARENT_FRAMEBUFFER` 创建期 hint | Phase B | **需要**（没有它 alpha 写了也被当不透明） |
| clearColor alpha=0 通路（GL 后端） | Phase B（现有管线已通大半，待 spike） | **需要**（桌宠背景全透，alpha=0） |
| premultiply quad blit（R1 修正） | Phase B（spike 定严重度） | **需要**（桌宠 sprite 边缘的半透明像素会被 straight alpha 瑕疵放大——磨砂是整体半透，桌宠是边缘抗锯齿像素，fringe 更显眼） |
| `DWMWA_SYSTEMBACKDROP_TYPE`（Acrylic/Mica 材质） | Phase C | **不需要** |
| 标题栏外观（Phase A） | Phase A（已合入 EUI-NEO main，commit `34b21f2`） | 不需要（无边框窗口无标题栏） |

**结论：桌宠被 Phase B 阻塞（hint + blit 两件），与 Phase C 零依赖。** 磨砂 Phase B 落地当天，桌宠的透明窗口就免费可用；反之若桌宠先行，其 spike（透明 hint + alpha=0 + PNG alpha 合成验证）就是磨砂 Phase 0 spike 的超集，成果直接反哺磨砂。

注意一个语义差异：磨砂是「clearColor 统一半透」，桌宠要的是「背景全透 + 内容不透」——即 clearColor alpha=0、sprite 内容自带 alpha。这比磨砂的用法**更简单**（不涉及 UI 内容半透假设），premultiply 修正对两者是同一个 blit 路径。

### 2.3 置顶 / 点击穿透 / 任务栏隐藏 / 不抢焦点

| 能力 | 现状 | 依据 |
|------|------|------|
| 置顶 | **后端已支持，子窗口未接线** | `WindowCreateRequest.alwaysOnTop` → `GLFW_FLOATING`（window_backend.cpp:579）；缺 DslWindowConfig 透传。Win32 侧等价 `HWND_TOPMOST`（失焦仍保持置顶，learn.microsoft.com） |
| 无边框 | **后端已支持，子窗口未接线** | `decorated` → `GLFW_DECORATED`（:578）；同上 |
| 鼠标穿透 | **GLFW 3.4 自带，EUI-NEO 未暴露** | 3rd/glfw 是 3.4（glfw3.h:287/930），有 `GLFW_MOUSE_PASSTHROUGH`（创建期 hint + 运行期 `glfwSetWindowAttrib`，官方文档：仅无边框窗口支持；Win32 实现即 `WS_EX_TRANSPARENT`）；EUI-NEO 全库无引用 |
| 不抢焦点 | **缺**。`createWindow` 未设 `GLFW_FOCUS_ON_SHOW`（默认 GLFW_TRUE，即 `glfwShowWindow` 会抢焦点） | GLFW 3.4 window guide；Win32 补强手段：`WS_EX_NOACTIVATE`（点击不成为前台）+ `ShowWindow(SW_SHOWNOACTIVATE)`，均 learn.microsoft.com 核实 |
| 任务栏隐藏 | **无**。GLFW 无此 hint，需要平台层 `WS_EX_TOOLWINDOW`（`SetWindowLongPtr` 改 GWL_EXSTYLE；官方语义：任务栏和 Alt+Tab 均不出现） | grep EUI-NEO core/ 零命中；这是唯一需要全新 Win32 代码的项（~20 行，接口隔离进 platform） |
| 运行时移动窗口（拖拽宠物） | **无**。`core/window/window_backend.h` 的 API 清单里没有 setWindowPos / getWindowPos | 拖拽需要：onDrag 给 deltaX/Y + 运行时 setWindowPos；均需新增（GLFW 的 `glfwSetWindowPos` 一行封装） |

**关键技术事实**（影响透明与命中路线选择，外部调研 + 用户调研双重核实）：

1. 经典「按像素 alpha 命中」分层窗口路线：`WS_EX_LAYERED` 与 OpenGL 窗口类传统要求的 `CS_OWNDC` 类样式冲突（learn.microsoft.com）；且 OpenGL 不能直染分层窗口——要离屏 FBO → `glReadPixels` → `CreateDIBSection`（**BGRA 字节序交换**）→ `UpdateLayeredWindow`（**预乘 alpha**，否则半透明边缘黑边），`glReadPixels` 是 CPU-GPU 同步点（用户调研：100-300px 窗口 30fps 足够，可脏矩形优化）。
2. GLFW 3.4 透明窗口走 **DWM 合成路线**（`GLFW_TRANSPARENT_FRAMEBUFFER` + blur-behind），天然规避 1 的全部三个坑（无类样式冲突、无读回同步点、premultiply 在 blit 一次修）。
3. **结论：桌宠透明走 DWM/GLFW 路线**（与磨砂设计结论一致，共享 Phase B）；分层窗口读回路线留作 DWM 路线失效时的备选。逐像素命中则**不需要**分层窗口——`WM_NCHITTEST` 读帧缓冲 alpha 阈值即可（O1，用户调研给出）。

### 2.4 动画与图像渲染：能力充足

- **序列帧动画**（桌宠核心）：三条现成路径，按成本排序：
  1. **多 PNG + `image(id).source()` 切换**：stb_image 加载 RGBA PNG（`core/render/image_source.cpp:801-809`），纹理有缓存；onTimer 周期换 source。最简单，帧数少（<20）时纹理缓存无压力。
  2. **sprite sheet + `clip(true)` + `translate()`**：单张 PNG，`clip` 已有（dsl.h:502），配合 ImageBuilder 的 transform 偏移显示帧区域。省纹理切换，但布局代码复杂一点。
  3. **`ImageStream`（RGBA8）**：`submit()` 逐帧喂 RGBA（image_stream.h:13-19 有 RGBA8），本是视频通路（examples/dynamic_texture.cpp、ffmpeg_video_player.cpp），桌宠帧率低用它偏重。**推荐路径 1 起步，帧数多再升路径 2。**
- **补间/easing**：`transition(duration, ease)` + `animate(AnimProperty)`（dsl.h:795-812，8 种 easing，位掩码属性）——气泡弹出、被戳抖动等反应动画直接用。
- **驱动回调**：`onTimer(seconds, cb)`（dsl.h:762，runtime_update.h:379-405 到点触发 + 重绘）和 `onFrame(delta)`（dsl.h:768，逐帧回调）都有。**注意 onTimer 语义**：等待期间置 `animating_=true`（runtime_update.h:403），主循环会按帧间隔空转等待（`waitForNextFrame`，高分辨率 waitable timer 睡眠，frame_pacing.h:20-54）——不是忙转，但唤醒频率 = 全局 fps（默认 90）。见 §2.5 功耗。
- **SVG 栅格化**：nanosvg（3rd/）+ `SvgBuilder`——皮肤如走矢量可行，但序列帧动画用 SVG 每帧栅格化不划算，**不推荐做动画主体**。
- **交互原语**：`onTap/onPress/onDrag/onContextMenu/onTimer/onFrame` 全有（dsl.h:174-186）；DragEvent 带 `deltaX/deltaY/totalX/totalY`（dsl.h:81-90）——拖拽移动窗口的数据是现成的，缺的只是 setWindowPos。contextMenu、toast、dialog 组件均在能力清单。

### 2.5 帧率与功耗模型（常驻窗口的特殊关注点）

EUI-NEO 的帧模型对桌宠其实很友好，但要理解边界：

- **无动画时零渲染**：子窗口不 animating 且无 paint 请求时，主循环 `glfwWaitEvents()` 阻塞等事件（glfw_app_main.cpp:625-630），事件驱动，CPU≈0。桌宠静止（如「睡觉」态停用呼吸动画）时**完全免费**。
- **动画期间按全局帧率跑循环**：onTimer 等待期 animating_=true，主循环以 `frameInterval`（= min(显示器刷新率, `DslAppConfig::fps`)，默认 90，app_runner.h:116-129）的节奏空转等待，但只在 timer 到点时才真正渲染。即 8fps 的眨眼动画 = 每秒 90 次极短唤醒 + 8 次小窗口渲染。**唤醒成本 = 高分辨率定时器睡眠，很低；渲染成本 = 128×128 小窗脏区重绘，可忽略。** 但严格说这是为 60/90fps UI 设计的节拍，给 8fps 的桌宠用偏浪费。
- **子窗口无独立帧率**：`fps` 是 app 级全局配置（DslAppConfig），子窗口跟随主窗口节拍（glfw_app_main.cpp:609-617 在主窗口帧回调里统一 update 子窗口）。
- **功耗结论**：MVP 不需要动帧模型——实测若空转唤醒可测出功耗（任务管理器/`RenderFrameStats`），再加「桌宠低频节拍」扩展（候选：`DslWindowConfig::fps` 子窗口独立帧间隔，或 onTimer 休眠路径改 `glfwWaitEventsTimeout`）。列为开放问题 O2，不进 MVP 承诺。

### 2.6 缺口清单汇总（全部走 EUI-NEO 上游 PR）

| # | 缺口 | 改动位置 | 规模 |
|---|------|----------|------|
| G1 | DslWindowConfig 透传 decorated/alwaysOnTop/resizable/position/focusOnShow（WindowCreateRequest 已支持前四项，纯接线；focusOnShow 需加 `glfwWindowHint(GLFW_FOCUS_ON_SHOW, ...)`） | dsl_app.h + dsl_app_impl.h + glfw_app_main.cpp `createManagedWindow` + window_backend.cpp | ~35 行 |
| G2 | 子窗口最小尺寸 160×120 下限放开（128×128 需要） | dsl_app_impl.h:149-150 | ~4 行 |
| G3 | `core::window::setWindowPos/getWindowPos`（glfwSetWindowPos 封装） | window_backend.h/.cpp | ~15 行 |
| G4 | 鼠标穿透：`GLFW_MOUSE_PASSTHROUGH` 暴露为创建期配置 + 运行时切换 | window_types.h + window_backend.cpp + DslWindowConfig | ~20 行 |
| G5 | 任务栏/Dock 层面的窗口属性：Windows `WS_EX_TOOLWINDOW`（+可选 `WS_EX_NOACTIVATE`）、macOS 全桌面空间常驻等 NSWindow 属性（platform 新文件，接口隔离，对齐 window_effect 先例） | core/platform/window_style.h/_win.cpp/_macos.mm/_stub.cpp | ~60 行 |
| G6 | 透明帧缓冲（**磨砂 Phase B，非桌宠专属**） | 见磨砂设计 §3.1/§3.2 | 磨砂设计已估 2-4 天 |

G1-G5 合计约一个 **1-2 天的上游 PR**（`feat(window): managed window configuration`，含 example），G6 由磨砂排期承担。

---

## 3. 形态方案对比（外网调研 + 用户自研调研，2026-09-22）

> 输入：外部调研（子 agent 实抓 GitHub/官方文档，来源见文末）+ 用户自研调研报告 `docs/superpowers/research/2026-09-22-desktop-pet-external-research.md`（六方案雷达图数据、素材渠道、Win32 技术要点）。两份调研独立得出同一初步结论（序列帧够用），本节按最新选型标准（技术/成本/质量，授权不作废门）复核。

### 3.1 六种实现模式对照

| 方案 | 运行时情况 | 与 EUI-NEO 集成成本 | 素材/创作生态 | 桌宠场景评价 |
|------|-----------|--------------------|--------------|--------------|
| **A. 精灵图序列帧（shimeji 式）** | 自研 ~200 行帧播放器（stb_image + ImageBuilder，全在仓库内） | **零**（§2.4 逐项对齐） | **最好**：shimeji 格式事实标准 + CC0 序列帧包现成（见 3.2 渠道表） | 动画集有限但桌宠完全够用，shimeji 生态验证多年 |
| B. Rive | 运行时 **MIT 完全开源**（OpenGL 渲染器现成），但编辑器是 SaaS 订阅（Free 档不能生产导出，Cadet $17/座/月），格式非开放 | 中：rive-cpp 引入新依赖树，渲染要接进自绘 GL 后端 | 编辑器门槛 + 桌宠素材生态薄 | **状态机天然适合行为驱动**（input/state machine 是 Rive 一等概念），矢量平滑、文件小——**技术上是序列帧的最强升级对手** |
| C. Spine（骨骼） | 运行时源码可见非开源（需购买编辑器许可，每用户一 seat） | 高：蒙皮变形渲染 ≈ 半个后端 | 编辑器成熟（买断），桌宠素材生态薄 | 表现力高、动作词汇量大，但 MVP 用不上这么多 |
| D. Live2D | 专有 Cubism SDK（FREE 档限个人/小规模，年营收有门槛，详见官网） | 高：网格变形 + 物理渲染接入自绘后端 | 模型需专门制作，成本人天级 | 角色表现力天花板，商业化桌宠（Desktop Mate）验证过 |
| E. DragonBones / Spriter SCML / CoaTools / Lottie | DragonBones MIT 但编辑器已死（腾讯归档）；Spriter 运行时萎缩；CoaTools 无成熟 C++ 运行时；Lottie 依赖 AE 创作 | 各异 | 全部生态萎缩或错位 | **不推荐**（维护风险/工具链断裂，用户调研同结论） |
| F. HTML/CEF 壳 | Electron/CEF 或 Tauri（BongoCat 先例） | 无（另一渲染栈） | 任意 web 素材 | **正面冲突**单 exe 零 DLL；Chromium 壳常驻 150-400MB |

开源先例佐证 A 的可行域：Shimeji-EE（BSD 类）、Ark-Pets（GPL-3.0，1099★，明日方舟序列帧）、eSheep（1150★）、wl_shimeji（GPL-2.0）；事件驱动新形态参考 NeurolingsCE（C++/Qt6，响应 AI 编码工具事件）与 clawd-on-desk（6267★，监控 AI agent 状态）——**DevDesk 的桌宠将来可接代理/Claude 事件**（M2 气泡的进阶源，§4.5）。

### 3.2 推荐：A（精灵图序列帧）——维持，理由重述 + 升级触发条件

放开授权约束后重新权衡的**独立判断：仍然推荐 A 起步**，理由收敛为五条：

1. **实现成本最低**：~200 行帧播放器 vs Rive 一套新依赖树+渲染接入 vs Spine/Live2D 半个后端。桌宠 MVP 的表现力需求（待机/眨眼/被戳）本来就是序列帧的主场。
2. **素材渠道现成**：CC0 序列帧包即插即用（不用等美术/不用买编辑器订阅），shimeji 生态按帧数计的现成行为库（含 walk/run/eat/sleep/grab 全套状态的包都有，见下表 tatsu-san 桌宠包）。
3. **与 EUI-NEO 零缝隙**（§2.4）：多 PNG 切帧 / sprite sheet+clip / transition easing / onTimer 全部现成，B/C/D 每个都要动渲染后端。
4. **shimeji 格式是事实标准**，数据驱动的动画配置是所有成功桌宠项目的共同点（Shimeji 用 XML actions/behaviors、DyberPet 用 JSON MOD、eSheep 用 animations.xml + 编辑器）——**皮肤格式从第一天就走 JSON 数据驱动**（§4.3），并预留 shimeji `actions.xml` 子集兼容的导入路径（O5），白捡现存素材生态。C++ 状态机可参考 libshijima（纯 C++ 跨平台仿真核心，GPL-3.0——只参考思路重写，不链接）。
5. 反向佐证：Shijima-Qt（C++/Qt 重实现）作者归档时自述「框架选错导致整个项目变成 hack 集合」——用自绘轻量路径而非重型框架，正是本方案的方向。

**Rive 是认真考虑过的对手**（状态机行为驱动 + MIT 运行时 + 矢量质量），不选它做起步的唯一硬理由是集成成本与素材生态；**升级触发条件**（任一满足即立项重评）：① 需要大动作词汇量/程序化平滑动画（走路物理、连续朝向插值）；② 出现「动画由设计师持续迭代」的工作流需求（Rive 编辑器实时预览）；③ 桌宠素材生态出现 Rive 优先的拐点。届时窗口/交互/状态机/持久化层（§4 全部）**原样复用，只换渲染层**——这是 A 起步的结构性保险。

**素材获取渠道**（用户调研 + 外部调研合并；授权从硬约束降为偏好，CC0 仍最省事）：

| 渠道 | 许可 | 备注 |
|------|------|------|
| Kenney Animal Pack / Cube Pets / All-in-1（kenney.nl、opengameart.org） | **CC0**（全站） | 零义务，首选池 |
| Tiny Creatures（itch.io，180 sprite）、Pixel Adventure（Pixel Frog） | CC0 | 零义务 |
| Desktop Pet Sprite（tatsu-san） | 宽松（CC-BY 系） | **含 walk/run/eat/sleep/grab 全套状态**，最贴合桌宠需求 |
| Lacey Animated Pixel Doll（57 帧透明 PNG + atlas） | CC-BY | 帧序列完整，「关于」页致谢即可 |
| Kawaii Pixel Pet Pack | CC-BY（明确兼容桌宠） | |
| Shimeji-EE 官方素材包（Shimeji/KuroShimeji） | BSD 类（标注来源、修改明示） | 可随程序分发 |
| 自绘（Clay Cat，呼应品牌色 #D97757） | 自有 | **MVP 首选**：与功能开发解耦，不阻塞在素材选型上 |

避坑（用户调研红线，保留为偏好级建议）：GPL / CC-BY-SA / CC-BY-NC 素材有传染或禁商用条款，能用 CC0/CC-BY 替代时不用它们；OpenGameArt 用 License 过滤器筛。

### 3.3 交互面惯例（各项目 README/文档核实的公共集合）

- **拖拽移动**：全员标配；进阶为**抛掷物理**（初速度 + 重力下落，Shimeji/eSheep/Ark-Pets 均有）。
- **点击反应状态机**：Shimeji 动作词汇表（Stand/Walk/Run/Sit/Fall/...）+ 三档性格行为频率（Calm/Mischievous/Professional）；Ark-Pets「戳一戳」；Desktop Mate 摸头反应。典型状态集：待机/游走/坐下/睡觉/被戳/拖拽中/下落/边缘攀爬。
- **气泡对话**：Shimeji speech 配置、DyberPet AI 聊天、clawd-on-desk 权限请求气泡；实现惯例是**独立于宠物本体的第二个窗口**（可穿透/可点击切换）。
- **右键/托盘菜单**：Ark-Pets 双入口（托盘 + 右键宠物），菜单集合：手动模式 / **透明模式（点击穿透开关）** / 多形态切换 / 退出。
- **上下文感知行为**（进阶）：沿窗口边框攀爬（Shimeji）、站窗口边缘（Ark-Pets）；前台窗口检测在 macOS 要 Accessibility 权限、Linux 要 shell 插件，**Windows 原生最容易**——若做，Windows 先行。**事件驱动形态**（NeurolingsCE/clawd-on-desk）：宠物状态跟随 AI 编码工具事件——DevDesk 天然有素材源（代理流式状态、Claude 调用事件），列为 M2+ 气泡内容的进阶源（§4.5）。
- 本设计裁剪（§4.4/§4.5）：MVP 取拖拽+点击反应+右键菜单；气泡 M2；抛掷物理/攀爬/游走**明确不做**（屏幕工作区探测 + 长状态机，性价比低，列不承诺远期）。

---

## 4. 技术设计

### 4.1 总体形态：DevDesk 进程内独立子窗口 + module_pet

对齐模块插拔方案 A 的装配模式，但桌宠**不是 Router 页面模块**（没有页面内容、不该出现在侧边栏）：

```
main.cpp 启动（读 config.json）
  ├─ petEnabled=false → 不做任何事（零执行，方案 A 语义）
  └─ petEnabled=true  → pet::startPetWindow()   // module_pet 内
                          └─ app::openWindow(PetWindowConfig, petCompose)
                              → EUI-NEO ManagedWindow（无边框/置顶/透明/128×128）
```

- `src/module_pet/`：static lib（复制 module_todo 的 16 行 CMakeLists 模板），含 `pet_state.h`（状态机）、`pet_config.h`（皮肤目录解析 + 位置持久化）、`pet_compose.cpp`（窗口 UI）、`pet_service` 不需要（无后台线程——动画由 UI 线程 onTimer 驱动）。
- 装配门控在 main.cpp（与方案 A 的 `isEnabled()` 同一模式）：`petEnabled` 缺省 **false**（桌宠是氛围功能，默认关，用户在设置页主动开）。
- 设置页新增「桌宠」卡片：开关（重启生效，同方案 A 一期决策）、皮肤下拉（扫描 `assets/pets/*/pet.json`）、位置重置按钮。

### 4.2 窗口与平台层（EUI-NEO 扩展，G1-G5，双平台）

PetWindowConfig 映射（G1 落地后，**全部是平台无关的 GLFW/DslWindowConfig 层语义**）：

```cpp
app::openWindow(app::DslWindowConfig{}
    .title("DevDesk Pet")
    .pageId("pet")
    .windowSize(128, 128)          // sprite 紧包围盒，G2 放开下限
    .windowPosition(cfg.petX, cfg.petY)   // 持久化位置恢复
    .decorated(false)              // 无边框
    .alwaysOnTop(true)             // 置顶
    .resizable(false)
    .focusOnShow(false)            // 不抢焦点（G1，GLFW_FOCUS_ON_SHOW）
    .clickThrough(cfg.petClickThrough)    // G4
    .hideFromTaskbar(true)         // G5
    .clearColor({0, 0, 0, 0}),     // 全透（G6/磨砂 Phase B 落地后生效）
    petCompose);
```

**双平台能力映射**（核心事实：下表前三行全部由 GLFW 3.4 同一套 hint 覆盖，EUI-NEO 只需暴露，不分平台写代码）：

| 能力 | Windows 11 | macOS 27+ |
|------|-----------|-----------|
| 无边框 + 像素透明 | `GLFW_DECORATED=0` + `GLFW_TRANSPARENT_FRAMEBUFFER`（DWM 合成路线，规避 `WS_EX_LAYERED`×`CS_OWNDC` 冲突，§2.3） | 同一 hint 组合（GLFW 走 `NSWindow.opaque=NO` + clear 背景；磨砂设计 §3.7 已确认 macOS 技术障碍不大） |
| premultiplied alpha | DWM 合成要求（磨砂 R1，Phase B premultiply blit 修） | macOS 合成同样要求 premultiplied——**同一个 GL 层 blit 修正两平台通吃** |
| 置顶 | `GLFW_FLOATING` → `HWND_TOPMOST`（失焦保持置顶） | 同 hint → `NSFloatingWindowLevel` |
| 点击穿透 | `GLFW_MOUSE_PASSTHROUGH` → `WS_EX_TRANSPARENT` | 同 hint → `NSWindow.ignoresMouseEvents` |
| 不抢焦点 | `GLFW_FOCUS_ON_SHOW=0`；必要时 platform 层补 `WS_EX_NOACTIVATE` + `SW_SHOWNOACTIVATE` | 无边框 NSWindow 默认不抢主激活，但 `canBecomeKeyWindow` 行为待验证；若需精确不激活，`window_style_macos.mm` 用 `NSNonactivatingPanel` 思路处理（**待验证**，列为 M2 的 macOS 验收项） |
| 任务栏/Dock | `WS_EX_TOOLWINDOW`（任务栏 + Alt+Tab 均不出现，`SetWindowLongPtr` 改 GWL_EXSTYLE） | **无需处理**：Dock 图标是 app 级，子窗口本就不进 Dock（`LSUIElement` 只在做「仅宠物无主窗」模式时才需要，本期不需要） |
| 全桌面空间常驻 | 默认（topmost 窗口全桌面可见） | 需要 `NSWindowCollectionBehaviorCanJoinAllSpaces`（`collectionBehavior`），GLFW 不暴露 → `window_style_macos.mm` 补（**macOS 专属项**，不加则宠物只浮在创建时的 Space） |
| 坐标系 | 屏幕左上原点 | GLFW 已统一抽象为左上原点（底层 macOS 左下原点的换算在 GLFW 内）——位置持久化语义两平台一致 |

G4/G5 的平台文件落位（对齐 EUI-NEO 既有 `window_effect.h/_win.cpp/_stub.cpp` 先例，DevDesk 侧业务代码零 `#ifdef`，与 T6 惯例一致）：

```
core/platform/window_style.h        // 平台无关接口：
                                     //   bool setWindowStyleFlags(void* nativeHandle, WindowStyleFlags);
                                     //   bool setClickThrough(void* nativeHandle, bool enabled);
                                     //   bool getWindowPos / setWindowPos（G3 的平台半边）
core/platform/window_style_win.cpp  // WS_EX_TOOLWINDOW / WS_EX_NOACTIVATE / SetWindowPos
core/platform/window_style_macos.mm // CanJoinAllSpaces / ignoresMouseEvents / setFrame
core/platform/window_style_stub.cpp // Linux 空实现
```

- **窗口尺寸 = sprite 紧包围盒**，不用「大透明窗口内浮动小图」。理由：`GLFW_MOUSE_PASSTHROUGH` 是整窗穿透；逐像素命中可行但走 `WM_NCHITTEST` 读帧缓冲 alpha 阈值（Windows）/ `NSWindow` 事件链（macOS）属于 M3 精修（O1）。bbox 方案下窗口几乎全是有效像素，整窗穿透的语义损失可忽略。
- **穿透定位为可选模式**（右键菜单「挂机穿透」，对齐 Ark-Pets 的「透明模式」惯例）：睡觉/挂机时开 `clickThrough`（运行时切换——Windows 走 `glfwSetWindowAttrib(GLFW_MOUSE_PASSTHROUGH)`，macOS 走 `ignoresMouseEvents`，G4 的运行时半边）。默认关。
- 拖拽（G3）：`onDrag` 回调里 `core::window::setWindowPos(handle, petX + totalDeltaX, petY + totalDeltaY)`——需要拿到子窗口 Handle；`DslWindowRequest` 增加 `onWindowCreated(Handle)` 回调（G1 一并加，~5 行）。拖拽结束持久化位置。
- **macOS 27+ 注意**：项目已知 macOS 侧 Liquid Glass/材质体系待调研（磨砂设计 Phase E）；桌宠不依赖任何材质，只依赖透明 hint（GLFW 层）——**桌宠 macOS 移植不被磨砂 Phase E 阻塞**，但 premultiply blit（Phase B 的 GL 层修正）同样前置。

### 4.2.1 平台行为差异清单（双平台验收差异，集中记录）

| 差异点 | Windows | macOS | 处理 |
|--------|---------|-------|------|
| 焦点语义 | `FOCUS_ON_SHOW=0` 即可；`WS_EX_NOACTIVATE` 作补强 | 无边框窗口 key window 行为不同，可能需要 NSNonactivatingPanel | macOS 项待验证（M2 验收）；不通过也不阻塞 MVP（宠物偶尔抢一次焦点是可接受的降级） |
| 多显示器 | 虚拟屏幕坐标，GLFW 左上原点一致 | 同左（GLFW 抽象） | 位置钳制逻辑一份代码（§4.6） |
| 全屏应用共存 | 全屏游戏/视频时 topmost 宠物悬浮其上（Ark-Pets 记录过与特定游戏引擎冲突、Wallpaper Engine 干扰窗口检测的案例） | 全屏 Space 切走时宠物留在原 Space（CanJoinAllSpaces 之前的默认行为） | M2：检测前台全屏窗口自动隐藏（Windows 原生易做，macOS 需要 API 核实——待验证）；MVP 先不处理，记录已知问题 |
| 开机自启 | 注册表 `HKCU\...\Run` | Login Items（SMAppService 系） | **M3，本期不做**；两平台 API 形态差异大，届时单独立项 |
| OpenGL 兼容性 | Ark-Pets FAQ 记录过 NVIDIA 驱动 OpenGL GDI 兼容导致黑窗的案例 | Metal/GL 合成路径由 GLFW/MoltenVK 承担 | 风险表 R8；实测覆盖 NVIDIA 机器 |

**Windows 杂项坑清单**（用户调研核实，实现期逐项对照）：

- **DPI**：PerMonitorV2 感知必须声明（EUI-NEO 已开 `GLFW_SCALE_TO_MONITOR`，HiDPI 默认路径一致）；多显示器逐屏 DPI 时持久化坐标按逻辑坐标存（§4.6），跨屏拖动后重存。
- **TOOLWINDOW 任务栏残留**：个别情况下设完 `WS_EX_TOOLWINDOW` 任务栏仍有残影——配合无标题文字 + `ITaskbarList::DeleteTab` 清理。
- **全屏应用检测**：`SetWinEventHook`/shell hook 监听前台全屏窗口（R9 的实现机制，Windows 原生易做）。
- **睡眠唤醒后 GL 上下文可能丢失**：唤醒事件里验证上下文有效性，失效则重建（EUI-NEO 现有 backend 是否已处理待验证——列 R10）。

### 4.3 渲染与帧动画

- 皮肤格式（自定 JSON，数据驱动是桌宠社区的成功共识，§3.2；解析用 platform::json）：

```
assets/pets/<skin-name>/
  pet.json        # {"frameRate": 8, "states": {"idle": ["f1.png","f2.png",...], ...},
                  #  "license": "CC0", "source": "..."}     # 授权信息随包自带，设置页可展示
  f1.png ...      # RGBA PNG，同尺寸（= 窗口尺寸），128×128 为 shimeji 标准帧尺寸
```

- **内置美术皮肤（2026-09-23）**：`assets/pets/anime-girl/` 随仓库分发——OpenGameArt
  "Anime Girl"（Michona）by Alicahordana，**CC0**（满足 R7 素材纪律）。30 帧四态映射：
  blink→idle / jump→poked / walk→typing / 闭眼帧→sleep，帧统一缩放规整 128×128（脚踩
  底边、内容居中）；CMake POST_BUILD 拷到 build/assets/pets 供运行期发现。第三方
  「下载后放置」素材的安装说明见 `assets/pets/README.md`。

- **shimeji 生态兼容（预留，不进 MVP）**：shimeji 包的 `conf/actions.xml`（`<Pose Image="/shime1.png" ImageAnchor Velocity Duration>`）是纯数据描述，后续加一个导入器（解析 actions.xml 子集 → pet.json）即可吃到 deviantART/itch.io 的现成素材生态；MVP 先固化自有格式，导入器等真需求（开放问题 O5）。
- 每状态一个帧序列；`image("pet.frame").source(currentFrame)` 切换（路径 1，§2.4），onTimer(1/frameRate) 推进帧指针。
- 反应动画（被戳抖动/气泡弹出）不用帧序列，用 `transition + animate`（easing 一次到位，比加帧省资源）。
- 皮肤加载：`stbi_load` 已有异步通路（image_source 有 `_thread` 变体）；帧数少同步加载也够，MVP 同步。

### 4.4 状态机

纯 C++ 枚举 + 转移表，无框架（ponytail：一个 enum + switch 就是一个状态机的全部）：

```
enum class PetState { Idle, Dragged, Poked, Sleep };

Idle   --onPress(单击)-->  Poked    （播 0.5s 抖动/表情帧，transition easing）
Idle   --onDrag-->        Dragged  （跟随指针移动窗口；结束回 Idle 并存位置）
Idle   --5min 无交互-->   Sleep    （停用 onTimer = 零渲染零功耗；任意交互回 Idle）
Poked  --动画结束-->       Idle
```

MVP 只做 Idle/Poked/Dragged 三态；Sleep 进 M2（它同时是功耗优化）。走路/爬窗等 shimeji 式行为明确不做（需要屏幕工作区探测 + 长状态机，性价比低，列为不承诺的远期）。

### 4.5 气泡对话与右键菜单

- **气泡**：M2。自绘（圆角矩形 + 小三角 + text，全在 DSL 能力内），出现在宠物上方，`transition` 淡入。内容源三级：① 内置静态语料随机；② 接 Home 数据（「你有 3 条逾期任务」）；③ **事件驱动**（NeurolingsCE/clawd-on-desk 形态）：宠物状态跟随代理/Claude 调用事件（流式中→打字动画、完成→庆祝）——DevDesk 有天然的 `core::proxy_stats` 素材源，这是桌宠与工具集结合的最高价值点，列 M2+。不用 toast——toast 是全局组件，位置不可控。
- **右键菜单**：`onContextMenu` + contextMenu 组件（已有），菜单项：挂机穿透开关 / 换肤（子菜单或弹设置页）/ 位置重置 / 关闭桌宠。

### 4.6 持久化

`config.json`（SettingsStore 模式扩展，字段缺省即默认，老配置零迁移）：

```jsonc
{
  "petEnabled": false,
  "petSkin": "clay-cat",
  "petX": 1600, "petY": 800,     // 逻辑屏幕坐标；启动时钳制进当前显示器工作区
  "petClickThrough": false
}
```

- `SettingsConfig`（settings_config.h）加 4 字段 + `sanitized()`（坐标钳制、皮肤名白名单校验——扫描 assets/pets 目录，未知名回默认皮肤）。
- 位置写入时机：拖拽结束后（不是每帧）；换肤/穿透即时写。
- **多显示器/DPI 变化**：启动时用 `glfwGetMonitorWorkArea` 类信息钳制（G3 的 getWindowPos 配套可加 `getWorkArea`，~10 行，或 MVP 先钳制主显示器）。

### 4.7 DevDesk 接线汇总

| 文件 | 改动 | 规模 |
|------|------|------|
| `src/module_pet/*`（新） | 状态机 + compose + 皮肤解析 + 位置持久化 | ~300 行 |
| `src/main.cpp` | petEnabled 门控调 `pet::startPetWindow()`（openWindow 必须在主循环起来前入队——实际 consumeWindowRequests 在首帧消费，启动时调用即可） | ~10 行 |
| 顶层 `CMakeLists.txt` + `src/module_pet/CMakeLists.txt` | add_subdirectory + link | ~20 行 |
| `src/module_settings/*` | 「桌宠」卡片（开关/皮肤/位置重置）+ config 字段 | ~80 行 |
| `assets/pets/clay-cat/` | 首套皮肤（自绘 Clay Cat，MVP 首选；备选 CC0，见 §3.2 渠道表） | 资源 |
| `../EUI-NEO`（G1-G5 PR） | 见 §2.6 | ~135 行 + example |

注意：桌宠**不进** `PageId` 枚举、不进 Router、不进侧边栏（它不是页面）。与方案 A 的 `enabledModules` 关系：`petEnabled` 是独立字段（桌宠不是 Router 模块，不适用 enabledModules 的页面语义）；若方案 A 已落地，两者并存互不感知。

---

## 5. 与磨砂 Phase B 的关系（依赖与排期）

```
磨砂 Phase 0 (spike: 透明hint+alpha+backdrop)  ─┬─> 磨砂 Phase B (透明管线) ─┬─> 磨砂 Phase C (backdrop) ─> 磨砂 Phase D
                                                │                            │
                                                └── 桌宠 spike（超集复用）──┴──> 桌宠 MVP（G1-G5 PR 可与 Phase B 并行）
```

- **阻塞关系**：桌宠 MVP 的「透明」验收项硬依赖 Phase B 的 hint + premultiply blit（G6）。**不依赖 Phase C**（无 backdrop 需求）。**macOS 侧同样不被磨砂 Phase E 阻塞**——Phase E 是 NSVisualEffectView 材质（桌宠不需要），而透明 hint 与 premultiply blit（GL 层）是 Phase B 的平台无关产出。
- **并行机会**：G1-G5（窗口配置/穿透/移动/任务栏）与 Phase B 完全正交，可先行或同 PR 分批合入 EUI-NEO；桌宠的 spike 若先做，其「透明 hint + alpha=0 + PNG alpha 合成」验证直接充当磨砂 Phase 0 的第 1 项（磨砂设计 §5 Phase 0.1），**建议两边的 spike 合并成一次**。
- **排期建议**：磨砂 Phase B 先行（其受益面是全应用），桌宠紧随；若桌宠优先级高，先合 G1-G5 + 桌宠 spike（验证透明组合），透明未落地前桌宠可以先以「不透明圆角小窗」形态开发调试（compose/状态机/拖拽/持久化全部不依赖透明），Phase B 合入当天切换 clearColor alpha=0 验收。**即：开发不被阻塞，只有最终视觉验收被阻塞。**

---

## 6. MVP 分期与验收

### M0 — Spike：透明组合验证（0.5 天，EUI-NEO 仓库，与磨砂 Phase 0 合并）

1. G1 最小版（decorated/alwaysOnTop 透传）+ G2 + 透明 hint：128×128 无边框置顶透明窗口，画一张 RGBA PNG，验证：桌面透出、sprite 边缘无 fringe（回答 premultiply 严重度）、鼠标命中窗口即命中 sprite。
2. G3 拖拽：onDrag + setWindowPos 移动，无残影（磨砂 R2 的 resize 残影在固定尺寸窗口上不应出现，验证记录）。

**验收**：截屏 + 三问结论回填本文档（含「不透明圆角窗降级形态可用」的结论，作为 Phase B 未合入时的开发模式）。

### M1 — 最小可玩（2-3 天）

- EUI-NEO：G1-G5 完整 PR（含 example `pet_window.cpp`：透明置顶小窗 + 拖拽 + 穿透切换按钮；平台文件 `_win.cpp`/`_macos.mm`/`_stub.cpp` 齐备）。
- DevDesk：module_pet（Idle/Poked/Dragged + 眨眼 onTimer）+ 设置页卡片 + config 持久化 + 首套皮肤（自绘 Clay Cat，规避授权风险）。

**验收**（Windows 11 全项；macOS 27+ 验收项标注）：
- Windows：开启后桌宠常驻桌面（透明、置顶、不在任务栏/Alt+Tab、不抢焦点）；拖拽流畅且重启位置保持；单击有反应动画；关闭开关重启后无宠物窗口、无任何 pet 代码执行痕迹（日志零 pet 行）。
- macOS：透明/置顶/穿透/拖拽同套验收（GLFW hint 层同构）；**额外验证** CanJoinAllSpaces 生效（切换 Space 宠物跟随）与不抢焦点行为（§4.2.1 待验证项）。macOS 侧当前无开发环境（2026-09 从 macOS 迁回 Windows），**macOS 验收允许延后到恢复 macOS 环境后补验**，Windows 验收不等待。
- 通用：137+ 用例全绿；截屏归档（spec §16.2 流程）。

### M2 — 进阶（2-3 天）

- Sleep 态（无交互降级 + 零渲染功耗路径）、气泡对话（含接 Home 逾期任务数）、右键菜单（穿透/换肤/重置/关闭）、多皮肤、位置多显示器钳制。

**验收**：Sleep 态任务管理器 GPU 占用归零（对比 Idle 态）；气泡内容与 Todo 数据一致（关 todo 模块时回退通用语料，不显示死数据——对齐方案 A 的降级原则）；菜单全项可用截屏。

### M3 — 后续（不承诺）

桌宠低频帧率路径（O2）、按像素命中（O1）、shimeji 导入器（O5）、开机自启、事件驱动形态（§4.5 三级气泡源）、表现力升级（Rive/Spine/Live2D，按 §3.2 升级触发条件立项重评）。

**总量**：M0+M1 = 3-4 天（若磨砂 Phase B 已合入；未合入则 +1 天透明联调，或先以不透明形态验收）；M2 再 +2-3 天。

---

## 7. 风险与开放问题

| # | 风险/问题 | 影响 | 缓解 |
|---|-----------|------|------|
| R1 | **straight alpha premultiply**（继承磨砂 R1）：DWM 要求 premultiplied，sprite 边缘抗锯齿像素会发灰/毛边 | 透明桌宠视觉瑕疵 | 共享磨砂 Phase B 的 premultiply quad blit；M0 spike 首项验证 |
| R2 | **常驻渲染功耗**：呼吸/眨眼动画让主循环按 90fps 节拍唤醒 | 笔本电池、后台占用观感 | 实测（任务管理器 + RenderFrameStats）；Sleep 态零渲染（M2）；必要时加子窗口独立低帧率（O2） |
| R3 | **杀软/EDR 对置顶小窗的敏感度**：topmost + 无任务栏 + 常驻 + 本地监听端口的进程画像接近某些灰色软件 | 误报、信任问题 | DevDesk 本身常驻 + 已监听 8888（现状即如此，桌宠增量小）；不隐藏进程、设置页明示开关；远期代码签名 |
| R4 | **多显示器/缩放变化**：持久化坐标在新布局下落到屏外 | 宠物「消失」 | 启动钳制进工作区（§4.6）；M1 先主显示器钳制，M2 完善 |
| R5 | **子窗口生命周期**：主窗口最小化到托盘时子窗口是否跟随隐藏 | 托盘模式下宠物孤悬桌面 | 待验证：主窗口 hideToTray 时 childWindows 不隐藏（glfw_app_main.cpp:541-547 只推迟关闭）。**语义决策：宠物本来就该独立于主窗口存活，孤悬是特性不是 bug**；主窗口退出时 childWindows.destroyAll（:633）已保证宠物随进程退出 |
| R6 | **透明 + 置顶 + 穿透的组合兼容性**（GLFW 透明窗口在部分驱动/远程桌面下的已知怪癖） | 远程桌面/老显卡上宠物不可见或黑块 | glfwGetWindowAttrib 回查降级（磨砂 §3.1.2 同款模式）；降级形态=不透明圆角小窗 |
| R7 | 素材授权不干净（shimeji 社区第三方包多为个人授权；eSheep 内置角色无 license 文件） | 分发风险 | 首套皮肤自绘；内置分发只用 Shimeji-EE 官方包（BSD 类，标注来源）或 CC0（Kenney）；皮肤包内嵌 license/source 字段（§4.3）；用户自装素材责任自负（设置页注明） |
| R8 | **OpenGL 驱动兼容**：Ark-Pets FAQ 记录过 NVIDIA 显卡 OpenGL GDI 兼容问题导致黑窗 | 特定显卡上宠物不可见/黑块 | M1 验收覆盖 NVIDIA 机器；复现时降级为不透明圆角窗；严重时评估 ANGLE 路线（Ark-Pets 的绕法） |
| R9 | **置顶宠物与全屏应用/WallpaperEngine 冲突**：全屏游戏上悬浮宠物遮挡、WallpaperEngine 干扰窗口检测（Ark-Pets 案例） | 游戏时遮挡视野、偶发窗口异常 | M2 做「前台全屏窗口自动隐藏」（shell hook，Windows 原生易做，§4.2.1）；MVP 记录已知问题，右键菜单提供快速关闭 |
| R10 | **睡眠唤醒后 GL 上下文丢失**（用户调研坑清单）：唤醒后桌宠窗口黑屏/消失 | 唤醒后宠物不可用 | 唤醒事件验证上下文有效性、失效重建（EUI-NEO backend 现状待验证）；M2 验收含「睡眠唤醒后宠物正常」用例 |
| O1 | **按像素 alpha 命中**（透明区域点击穿透、不透明区域可交互） | 交互精细度 | 可行且不需要分层窗口：Windows `WM_NCHITTEST` 读帧缓冲 alpha 阈值 → HTCLIENT/HTTRANSPARENT；闲置/拖拽态动态切 `WS_EX_TRANSPARENT`（`SetWindowLongPtr` + `SWP_FRAMECHANGED` 生效）；macOS 走 `NSWindow` 事件链等价处理（待设计）。MVP 用 bbox 方案规避，M3 精修 |
| O2 | 子窗口独立帧率 / 低频 timer 的功耗路径值不值得做 | M3 | M2 实测 Sleep/Idle 功耗差再决策 |
| O3 | SDL2 后端（Linux 备选）无透明/穿透支持 | Linux 移植 | DevDesk 当前 Windows 优先 + macOS 27+，GLFW 后端；SDL2 侧标注不支持，Linux 移植时再议 |
| O4 | 首套皮肤素材选型（自绘 Clay Cat vs CC0 vs Shimeji-EE 官方包） | M1 资源 | 落地时定（§3.2 渠道表已给许可证名） |
| O5 | shimeji `actions.xml` 导入器（吃现成素材生态）值不值得做 | 生态扩展 | 出现「用户想装 shimeji 包」真实反馈再做（§4.3 格式已预留映射空间） |

---

## 附：调研来源

**用户自研调研**：`docs/superpowers/research/2026-09-22-desktop-pet-external-research.md`（六方案对比雷达图、CC0 素材渠道清单、Win32 分层窗口/逐像素命中技术要点、开源项目盘点——本设计 §3.1/§3.2/§4.2.1/O1 直接采用其结论）

**本地源码**（行号见 §2，2026-09-22 工作区，EUI-NEO commit `b172297`）：dsl_app.h / app.h / dsl.h / glfw_app_main.cpp / dsl_window_manager.h / dsl_window_runtime.h / app_runner.h / frame_pacing.h / window_backend.{h,cpp} / window_effect.h / image_source.cpp / image_stream.h / runtime_update.h

**本地设计文档**：磨砂设计 `2026-09-22-eui-window-frost-design.md`（Phase A-D 切分、premultiply/残影风险、降级链、macOS Phase E 定位）、模块插拔设计 `2026-09-22-module-plugin-architecture-design.md`（方案 A 装配门控、零执行语义、重启生效决策）

**外网**（2026-09-22 实抓）：
- Shimeji 格式与许可：github.com/gil/shimeji-ee（conf/actions.xml、licence.txt、originallicence.txt）
- 桌宠项目先例：github.com/isHarryh/Ark-Pets（+docs/FAQ.md，NVIDIA OpenGL/游戏引擎冲突案例）、github.com/pixelomer/Shijima-Qt（C++ 框架选型教训）、github.com/Adrianotiger/desktopPet（eSheep）、github.com/ChaozhongLiu/DyberPet、github.com/rullerzhou-afk/clawd-on-desk、github.com/shinyflvre/Mate-Engine（VRM 路线）、NeurolingsCE / BongoCat（事件驱动形态，见用户调研报告）
- 授权（选型参考，非约束）：live2d.com/en/sdk/license/（Cubism SDK 发布许可条款）、esotericsoftware.com/spine-runtimes-license（每用户编辑器授权要求）、rive（MIT 运行时 + SaaS 编辑器，见用户调研报告 §2）
- Windows API：learn.microsoft.com/en-us/windows/win32/winmsg/extended-window-styles（WS_EX_LAYERED×CS_OWNDC 冲突、WS_EX_TRANSPARENT、WS_EX_TOOLWINDOW、WS_EX_NOACTIVATE）、learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos
- GLFW 3.4：glfw.org/docs/3.4/window_guide.html（MOUSE_PASSTHROUGH、TRANSPARENT_FRAMEBUFFER、FLOATING、FOCUS_ON_SHOW、undecorated 限制）
- 参考产品：store.steampowered.com/app/3301060（Desktop Mate）、store.steampowered.com/app/431960（Wallpaper Engine）、veado.tube（veadotube，sprite 状态机路线佐证）
