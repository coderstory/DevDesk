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

## module_settings/ 与 module_wallpaper/

| 能力 | macos | win | 备注 |
|---|---|---|---|
| local_git_probe | ✅ | ✅ | |
| image_probe | — | ✅ | ⚠ 仅 Windows |
| wallpaper_host | — | ✅ | ⚠ 仅 Windows |
| yield_watchers | — | ✅ | ⚠ 仅 Windows |

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
| keychain | ✅ | ✅ | ✅ | 已知红：keychain 空账号语义 ×1 在此族 |
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
