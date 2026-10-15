// src/platform/message_box.h
// 启动期阻塞模态对话框（平台适配：_win = MessageBoxW，_macos = osascript display dialog，
// 其余 POSIX = stub 诚实降级）。为什么不用 EUI-NEO dialog：损坏检测发生在 dslAppConfig
// 静态初始化期间（窗口创建前、compose 循环不存在），EUI-NEO 的 dialog/toast 都挂在
// compose 树上，时序上不可能；本接口是启动错误路径的专用模态通道，运行期 UI 一律走 EUI-NEO。
#pragma once

#include <string>

namespace platform::msgbox {

enum class Choice { Ok, Cancel };

// 阻塞直到用户选择。withCancel=true → 双按钮【Ok】【Cancel】（DevDesk 语义：还原/退出）；
// false → 单按钮确认（DevDesk 语义：仅退出）。线程约束：须在主线程调用（macOS 的
// osascript 派发无窗口亲和，但语义上属于启动路径）。UI 文案由调用方给全（本层不做拼接）
Choice show(const std::string& title, const std::string& message, bool withCancel);

} // namespace platform::msgbox
