// notepad_prefs.h — 记事本应用级持久键（app_config，need:273 批 1）。
// 窗口尺寸/位置记忆 = 应用级一份（设计稿 §2：per-file 会多窗互踩）；字体族 /
// wrap 开关键位本批先落库格式（UI 面批 3 接管）。core::appDatabase() 未就绪
//（测试进程 / 极早帧）→ 全默认值，不报错。
#pragma once

#include <string>

namespace module::notepad {

struct NotepadPrefs {
    int width = 900;      // 默认 900×640（对齐查看器窗档）
    int height = 640;
    int x = 0;
    int y = 0;
    bool posSet = false;  // 首窗 = 屏幕居中（不调 windowPosition）
    std::string fontFamily;  // 空 = 平台等宽默认族（批 3 下拉接管）
    bool wrap = true;        // 自动换行默认开（设计稿 §4.9 菜单项）
    int editorFontSize = 14; // 编辑字号（12–20，批 3 A-/A+ 接管）
};

NotepadPrefs notepadLoadPrefs();
// 关窗时回写窗口几何（尺寸 + 位置一并；posSet 恒 true——记忆过即有位）。
void notepadSaveWindowGeometry(int width, int height, int x, int y);

}  // namespace module::notepad
