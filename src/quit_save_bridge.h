// src/quit_save_bridge.h
// 退出丢稿防线桥（need:291）：模块「有未保存修改」查询/放弃缝 + 退出 guard 体 +
// 主窗确认弹窗。模式同 toast_notice_bridge / memory_mcp_bridge（函数指针缝，
// 真实现由 main.cpp 装配段注入；本头被 main.cpp 与 app_sidebar.cpp 引用）。
//
// 链路：托盘 Exit / app::requestExit → EUI-NEO 主循环消费点问 quit guard
//（app::setQuitGuard 注册 need272 缝）→ 有未保存修改 = 开确认弹窗 + 阻止本次
// 退出；用户「放弃退出」/「放弃修改并退出」两钮收尾（后者清 dirty 后二次
// requestExit，guard 自然放行）。弹窗画在主窗 compose 尾部（drawDialogShell，
// #156 弹窗规范）。
#pragma once

#include <eui/dsl.h>

namespace app::quit_save {

using HasUnsavedHandler = bool (*)();
using DiscardHandler = void (*)();
using SaveNowHandler = bool (*)();

// 传 nullptr 对 = 恢复空桩（未装配 = 视为无未保存修改，退出直通）。
void setHandlers(HasUnsavedHandler hasUnsaved, DiscardHandler discard);

// 退出前同步保存缝（markdown.saveFinalForQuit）：guard 先保存再决定放行；
// 未装配 = 保持旧行为（只弹「有未保存修改」确认框，不尝试保存）。
void setSaveHandler(SaveNowHandler saveNow);

// ②need:300：guard 阻止分支唤起主窗的缝。托盘 Exit 在主窗隐藏/最小化态触发
// 时，确认弹窗必须画在可见窗上——阻止即先唤起。真实现由 main.cpp 装配段注入
// app::requestShow；单测注入 fake 计数。未装配 = 不唤起（保守原行为）。
using ShowMainHandler = void (*)();
void setShowHandler(ShowMainHandler show);

// app::setQuitGuard 落点（无捕获函数指针）：无未保存 → 放行；有未保存且有保存缝 →
// 先同步保存，成功即放行；保存失败 → 开「重试/放弃并退出」弹窗并阻止本次退出。
// 未装配保存缝 → 保持旧「有未保存修改」确认弹窗语义。
bool guard();

// 保存失败弹窗的「重试保存」主路径（onClick / 单测直调）：成功 → 关弹窗并退出；
// 失败 → 弹窗保持打开，返回 false。
bool retrySave();

// 「放弃修改并退出」主路径（弹窗主钮 onClick / 单测直调）：清未保存状态 →
// 关弹窗 → app::requestExit()（guard 二次消费时 hasUnsaved 已 false → 放行）。
void confirmDiscardAndExit();

// 主窗 compose 尾部调用：弹窗未开 = 零开销早退；开着画标准壳（危险变体）。
void drawConfirmDialog(eui::Ui& ui, const eui::Screen& screen);

// ── 单测观察缝（markdown_module_test_seam.h 同款「最小缝」纪律）──
// 确认弹窗是否处于打开态（guard 阻止后断言弹窗已开 / 收尾后断言已关）。
bool dialogOpenForTest();
// 清 handler + 关弹窗（用例间 static 状态隔离；生产代码不得调用）。
void resetForTest();

}  // namespace app::quit_save
