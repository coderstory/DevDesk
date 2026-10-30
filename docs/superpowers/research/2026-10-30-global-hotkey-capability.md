# 全局快捷键能力摸底（需-12 核实项）

- 日期：2026-10-30。作者：切图仔（UI/UX）。
- 结论：**EUI-NEO 现状无全局 OS hotkey 能力**，能力盘点文档（`docs/EUI-NEO-CAPABILITIES.md:36/:464`）的 ❌ 记载仍准确。

## 证据（一手源码）

- 键盘事件唯一入口 = GLFW key callback（`EUI-NEO/core/window/window_input_backend.cpp:238`）——GLFW 只在窗口聚焦时投递按键，失焦即断。
- 全仓（排除 3rd/glfw 自身注释）无 `RegisterHotKey`（Win）/ `addGlobalMonitor`、`CGEventTap`、`RegisterEventHotKey`（mac）任何实现或封装。
- 元素级 `onKeyEvent`（`core/dsl.h:750`）是焦点路由的应用内事件，与 OS 全局热键无关。

## 收集箱落地路径（两条，按项目口径选）

1. **EUI-NEO 反哺**（推荐，符合上游贡献政策）：fork 加平台隔离的 hotkey 缝——头文件平台无关接口 + `*_win.cpp`（`RegisterHotKey` + `WM_HOTKEY` 转发）/`*_macos.cpp`（`NSEvent addGlobalMonitorForEventsMatchingMask:KeyDown`，需用户授予辅助功能权限；或 `RegisterEventHotKey` 免权限但组合键受限）薄适配，注册/注销 API 挂 window_backend。成本约 1 天，DevDesk 收集箱等多处受益。
2. **DevDesk 内自建**：platform 层开平台文件薄适配，热键回调转 app 事件总线。成本相近，但能力留在 DevDesk 不反哺。

无论哪条：热键注册冲突（与其他 app 抢占）需处理失败反馈；无三方依赖，纯系统 API。

## 约束提醒

macOS 全局监听需辅助功能权限（系统设置手动授予），收集箱若依赖全局热键，首次启用需引导授权——这是产品面无法绕开的系统约束。
