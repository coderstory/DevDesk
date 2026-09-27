// src/module_markdown/md_ids.h
// Markdown 模块 UI 元素 id 常量（模块实现 + 无头测试共用）。
// 为什么落独立头而不是留在 markdown_module.cpp：模块没有对外头文件（测试只有
// extern "C" create_markdown_module），.cpp 内的常量测试引不到；跨 TU 复用的
// id 只能放头（2026-09-27 审计 MD-06）。
#pragma once

namespace module::markdown {

// 编辑器主输入框基 id（编辑态三栏中栏的 input）。
inline constexpr const char* kEditorInputId = "md.editor.input";

// 编辑器主输入框的聚焦命中 id。input 组件的聚焦判定是 ui.isFocused(id + ".hit")
// （EUI-NEO components/input.h:113）——编程式聚焦必须发「基 id + .hit」，发基 id
// 只会把焦点挂到 stack 元素上：组件判定未聚焦（不画 caret、不挂键处理器），按键
// 静默落回模块 keyEventHandler_。编辑器四处 requestFocus 统一走本常量。
inline constexpr const char* kEditorInputHitId = "md.editor.input.hit";

} // namespace module::markdown
