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
inline constexpr const char* kNotepadBtnSaveAsId = "notepad.btnSaveAs";  // F-04 另存为（设计稿 §4.2 btnSaveAs）
inline constexpr const char* kNotepadToolbarId = "notepad.toolbar";
inline constexpr const char* kNotepadStatusbarId = "notepad.statusbar";

// 查找条浮层（设计稿 §4.5 notepad.searchBar；批 2 F-06/F-07/F-08 探针锚）。
inline constexpr const char* kNotepadSearchBarId = "notepad.searchBar";
inline constexpr const char* kNotepadSearchQueryInputId = "notepad.search.query";
inline constexpr const char* kNotepadSearchCountId = "notepad.search.count";
inline constexpr const char* kNotepadSearchPrevId = "notepad.search.prev";
inline constexpr const char* kNotepadSearchNextId = "notepad.search.next";
inline constexpr const char* kNotepadSearchReplaceToggleId = "notepad.search.expand";
inline constexpr const char* kNotepadSearchCloseId = "notepad.search.close";
inline constexpr const char* kNotepadSearchReplaceInputId = "notepad.search.replace";
inline constexpr const char* kNotepadBtnReplaceOneId = "notepad.search.replaceOne";
inline constexpr const char* kNotepadBtnReplaceAllId = "notepad.search.replaceAll";

// 批 3 工具条/状态栏（设计稿 §4.2/§4.7；F-09 字体下拉 / F-10 字号步进 /
// F-15 编码·行尾点切段——探针与无头断言锚）。
inline constexpr const char* kNotepadDdFontId = "notepad.ddFont";
inline constexpr const char* kNotepadStepperFontMinusId = "notepad.stepperFont.minus";
inline constexpr const char* kNotepadStepperFontPlusId = "notepad.stepperFont.plus";
inline constexpr const char* kNotepadStatusEncodingId = "notepad.status.encoding";
inline constexpr const char* kNotepadStatusEolId = "notepad.status.eol";

// F-19 自动换行开关钮（棒 C 接线；选中态高亮 + largeFile 禁用）。
inline constexpr const char* kNotepadBtnWrapId = "notepad.btnWrap";

// 批 4 棒 A（F-05 未保存保护三选弹窗，设计稿 §4.2 探针锚）：弹窗渲染在
// 子窗自己的 Ui 树内（每窗独立保护弹窗）；三钮 = 保存/不保存/取消。
inline constexpr const char* kNotepadProtectDialogId = "notepad.protectDialog";
inline constexpr const char* kNotepadProtectBtnSaveId =
    "notepad.protectDialog.btn.save";
inline constexpr const char* kNotepadProtectBtnDiscardId =
    "notepad.protectDialog.btn.discard";
inline constexpr const char* kNotepadProtectBtnCancelId =
    "notepad.protectDialog.btn.cancel";

// 批 3 棒 B（F-11/F-13，设计稿 §4.2/§4.6；探针锚）：JSON 格式化钮（仅 .json
// 扩展显示）/ md 预览钮（仅 .md/.markdown 显示）+ 预览槽位（编辑区同槽位替换）。
inline constexpr const char* kNotepadBtnFormatId = "notepad.btnFormat";
inline constexpr const char* kNotepadBtnPreviewId = "notepad.btnPreview";
inline constexpr const char* kNotepadPreviewId = "notepad.preview";
inline constexpr const char* kNotepadPreviewContentId = "notepad.preview.content";
inline constexpr const char* kNotepadPreviewEmptyId = "notepad.preview.empty";

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

// quit guard 本体（app_assembly 装配期 app::setQuitGuard 挂载，F-05/D-3）：
// 按注册表创建序逐窗检查 dirty，弹首个待决脏窗的三选并返回 false 阻止本次
// 退出；全干净/全豁免/空注册表返回 true 放行。实现与纯判定面在
// notepad_protect.{h,cpp}（clue #269 三条穷尽路径约束）。
bool notepadQuitGuard();

}  // namespace module::notepad
