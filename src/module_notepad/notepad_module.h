// notepad_module.h — 记事本独立窗口模块公开面（need:273 批 1）。
// 开窗入口唯一消费方 = OS 打开漏斗（src/app_assembly.cpp，main 层 → module 合法
// 方向）与窗内「打开」对话框（F-02）。不进 Router / kModuleRegistry 零登记
//（PRD R-7：独立窗口形态，无侧边栏无 PageId 注册）。
//
// session 架构照抄 md_viewer_window 先例：静态注册表一窗一 session，
// onWindowDestroyed 注销防悬垂；同文件重复送达 = 规范路径查重命中 → 聚焦已有窗
// 不开新窗（v5 定案，防双窗静默覆盖）。
#pragma once

#include <cstddef>
#include <string>

namespace module::notepad {

// 窗内元素 id（探针 / 无头 compose 断言共用锚；notepad.* 前缀，设计稿 §4.2）。
// 每个子窗有独立 Ui 树，同 id 跨窗不冲突。
inline constexpr const char* kNotepadEditorInputId = "notepad.editor.input";
inline constexpr const char* kNotepadBtnOpenId = "notepad.btnOpen";
inline constexpr const char* kNotepadBtnSaveId = "notepad.btnSave";
inline constexpr const char* kNotepadToolbarId = "notepad.toolbar";
inline constexpr const char* kNotepadStatusbarId = "notepad.statusbar";

// OS 送达 / 窗内打开的唯一开窗入口：扩展名已由漏斗判定，本函数只负责
// 「查重聚焦或新窗」。仅主线程调用（OS 漏斗 / Apple Event / 键路由同域）。
void openNotepadWindow(const std::string& path);

// ── 测试缝（照抄查看器同名缝，D-6 随迁）──────────────────────────────

// session 存活数（漏斗单测 / 同文件聚焦「开窗数不变」断言）。
size_t notepadSessionCountForTest();
// 按开窗序取 session pageId（notepad.<递增序号> 格式断言；越界返回空串）。
std::string notepadSessionPageIdForTest(size_t index);
// 清空 session 注册表（不触窗口销毁——测试进程无主循环；用例间隔离）。
void resetNotepadSessionsForTest();

}  // namespace module::notepad
