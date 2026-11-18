# 小抄：DevDesk 平台拆分文件映射（2026-10-28 从真实代码生成）

> 用途：改任何平台相关能力时翻此表，确认**同族另一头**的文件；只改一头 = 违规（AGENTS.md §二）。
> 规则：能力 → 三平台文件对照；`—` 表示该平台无此拆分（Windows-only / 无 posix 兜底处已标注 ⚠）。
> EUI-NEO 侧拆分文件现查：`find EUI-NEO -name "*_macos.cpp" -o -name "*_win.cpp" -o -name "*.mm"`

## core/

| 能力 | macos | posix | win |
|---|---|---|---|
| atomic_write | — | ✅ | ✅ |
| file_perms | — | ✅ | ✅ |
| paths | ✅ | ✅ | ✅ |
| time | — | ✅ | ✅ |
| window_layout | ✅ | ✅ | ✅ |

## module_memory/engine/

| 能力 | posix | win |
|---|---|---|
| llama_host | ✅ | ✅ |
| python_locator | ✅ | ✅ |
| telemem_process | ✅ | ✅ |

## module_settings/、module_wallpaper/ 与 terminal 两族（2026-11-18 need:296 D4 修订）

| 能力 | macos | posix | win | 备注 |
|---|---|---|---|---|
| local_git_probe（module_settings） | ✅ | **—** | ✅ | ⚠ 无 posix 兜底（实存 _macos/_win 两实现） |
| image_probe（module_wallpaper） | ✅ | stub | ✅ | 改标：macos 列原误写 —，`image_probe_macos.cpp` 实存；posix = _stub 降级 |
| wallpaper_host（module_wallpaper） | ✅(.mm) | stub | ✅ | 改标：`wallpaper_host_macos.mm` 实存；posix = _stub 降级 |
| yield_watchers（module_wallpaper） | ✅(.mm) | stub | ✅ | 改标：`yield_watchers_macos.mm` 实存；posix = _stub 降级 |
| importer（module_wallpaper） | ✅ | stub | stub | 新补行：仅 macos 有实现，win/posix 走 `importer_stub.cpp` |
| mpv_controller（module_wallpaper） | ✅(.mm) | stub | ✅ | 新补行：win = `mpv_controller.cpp`，posix = `_stub` |
| terminal_settings_platform（module_terminal） | （复用 posix 列） | ✅ | ✅ | 新补行：无 _macos 文件，CMake 非 WIN32 分支统一挂 `_posix.cpp`（mac 构建走它） |
| terminal_backend（platform/terminal） | ✅ | **—** | ✅ | 新补行：macos 文件名漂移为 `_mac.cpp`（非 `_macos`），⚠ 无 posix 兜底 |

## platform/

| 能力 | macos | posix | win | 备注 |
|---|---|---|---|---|
| autostart | ✅ | ✅ | ✅ | |
| credential_protection | — | ✅ | ✅ | |
| exec_capture | — | ✅ | ✅ | |
| file_open | ✅ | ✅ | ✅ | |
| file_watcher | ✅ | **—** | ✅ | ⚠ 无 posix 兜底（已知红：macOS 轮询 ×2 在此族） |
| folder_picker | ✅ | ✅ | ✅ | |
| http_client_ca | — | ✅ | ✅ | |
| message_box | ✅ | ✅ | ✅ | |
| notification | ✅(.mm) | ✅ | ✅ | 唯一 Objective-C++ 文件 |
| process_host | — | ✅ | ✅ | |
| process_runner | ✅ | ✅ | ✅ | |
| single_instance | ✅ | ✅ | ✅ | |
| system_theme | ✅ | ✅ | ✅ | |
| terminal_launch | ✅ | ✅ | ✅ | |
| tray_icon | ✅ | ✅ | ✅ | |
| wallpaper_capability | ✅ | ✅ | ✅ | |
| window_visibility | ✅ | **—** | ✅ | ⚠ 无 posix 兜底 |

## 新增平台能力时的清单

1. 文件命名 `<能力>_<平台>.cpp`，放进对应层目录（core / platform / module 内）
2. CMake 按平台选源（禁止业务代码 `#ifdef`）
3. 更新本表（同族三列补齐或标注 ⚠ 原因）
