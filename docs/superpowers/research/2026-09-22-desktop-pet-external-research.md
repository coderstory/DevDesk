# 桌宠外部调研报告（用户提供，2026-09-22）

> 来源：用户自行调研后提供，供桌宠设计文档（2026-09-22-desktop-pet-design.md）引用并入。
> 注意：本报告以 Windows 为主线，macOS 侧由设计文档补充覆盖。

## 1. 开源桌宠项目盘点（要点）

- **Shimeji 家族**：原版 Java（zlib，停更）；Shimeji-ee（New BSD 主体，低维护，PNG 帧 + XML 配置）；libshijima（**纯 C++ 跨平台仿真核心库，GPL-3.0**——闭源应用不可链接，只可参考思路重写）；Shijima-Qt（GPL-3.0+，Linux）
- **其他**：OpenPets（Electron，MIT）、BongoCat（Tauri/Rust）、Desktop-Pet-Godot（LLM 驱动）、NeurolingsCE（C++/Qt6，响应 AI 编码工具事件）
- **非开源**：veadotube（免费不开源）；Live2D（专有 SDK，FREE 档商用限个人/年营收<1000万日元）；Spine（运行时源码可见非开源，需购买编辑器许可）

## 2. 动画方案对比结论

| 方案 | 运行时开源 | 创作工具开源 | C++ 集成 | 结论 |
|---|---|---|---|---|
| **序列帧 Sprite** | ✅ 自研 | ✅ GIMP/Krita/Aseprite | ✅ 最简 | **推荐**：桌宠动画集有限完全够用，Shimeji 生态验证多年 |
| DragonBones | MIT | 编辑器已死（腾讯归档） | 可用 | 维护风险极高，不推荐 |
| Spriter SCML | MIT | 专有（Free 受限） | 可用 | 生态萎缩，不推荐 |
| Lottie/rlottie | MIT（Samsung） | 依赖 AE（专有） | 简单 | 桌宠场景过度设计 |
| Rive | MIT 完全开源 | SaaS，Free 档不能生产导出（Cadet $17/座/月） | OpenGL 渲染器现成 | 状态机适合行为驱动但不全开源，备选 |
| CoaTools | 无成熟 C++ 运行时 | Blender 插件 GPL | ❌ | 不适合 C++ 嵌入 |

## 3. 素材渠道（CC0 优先）

- **CC0（零义务）**：Kenney Animal Pack / Cube Pets / All-in-1（opengameart.org、kenney.nl 全站 CC0）、Tiny Creatures（itch.io，180 sprite）、Pixel Adventure（Pixel Frog）
- **CC-BY/宽松**：Kawaii Pixel Pet Pack（明确兼容桌宠）、Lacey Animated Pixel Doll（57 帧透明 PNG + atlas）、Desktop Pet Sprite（tatsu-san，含 walk/run/eat/sleep/grab）
- **红线**：避免 GPL / CC-BY-SA / CC-BY-NC 素材（传染或禁商用）；OpenGameArt 用 License 过滤器筛 CC0/CC-BY

## 4. Windows 技术要点（自研 OpenGL 框架关键）

- 窗口样式：`WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW` + `WS_POPUP`；点击穿透 `WS_EX_TRANSPARENT`（与 LAYERED 联用）
- **每像素命中**：不设全窗穿透，处理 `WM_NCHITTEST` 读帧缓冲 alpha 阈值 → HTCLIENT/HTTRANSPARENT；拖拽/闲置动态切换 WS_EX_TRANSPARENT（SetWindowLongPtr + SWP_FRAMECHANGED 生效）
- **OpenGL 不能直染分层窗口**：离屏 FBO → `glReadPixels(GL_RGBA)` → CreateDIBSection（**BGRA 字节序交换**）→ `UpdateLayeredWindow`（**预乘 alpha**，否则半透明边缘黑边）
- 性能：glReadPixels+UpdateLayeredWindow 是 CPU-GPU 同步点，100-300px 窗口 30fps 足够；可脏矩形优化
- 其他坑：PerMonitorV2 DPI 感知必须声明；TOOLWINDOW 任务栏残留（配合无标题文字/ITaskbarList::DeleteTab）；全屏应用检测自动隐藏（shell hook）；多显示器逐屏 DPI；睡眠唤醒后 OpenGL 上下文可能丢失需重建

## 5. 推荐组合（用户调研结论）

**序列帧 Sprite + Kenney/OpenGameArt CC0 素材 + 自研 ~200 行帧播放器** + Win32 分层窗口方案。
- 代码层自研无 GPL 传染；参考 libshijima 状态机思路但完全重写
- 素材全 CC0 零署名义务；CC-BY 素材在「关于」页致谢
- 备选：Rive（MIT 运行时 + 状态机，但编辑器付费、格式非开放，不满足全开源）

## 6. 用户报告原文

完整原文（含六方案雷达图数据与逐项详解）见本文档 git 历史或用户会话记录；关键数据已全部收录于上。
