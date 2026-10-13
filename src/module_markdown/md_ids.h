// Markdown 模块 UI 元素 id 常量（模块实现 + 无头测试共用）。
// 落独立头：模块无对外头文件，.cpp 内常量测试引不到，跨 TU 复用只能放头。
#pragma once

namespace module::markdown {

// 编辑器主输入框基 id
inline constexpr const char* kEditorInputId = "md.editor.input";

// 聚焦命中 id。input 组件按 ui.isFocused(id + ".hit") 判聚焦 —— 编程式聚焦必须
// 发「基 id + .hit」，发基 id 只会挂到 stack 上（不画 caret、不挂键处理器）。
inline constexpr const char* kEditorInputHitId = "md.editor.input.hit";

} // namespace module::markdown
