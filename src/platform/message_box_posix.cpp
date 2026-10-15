// src/platform/message_box_posix.cpp
// 非 macOS 的 POSIX 兜底：无统一的原生阻塞对话框设施（无 MessageBox 等价物、无 osascript），
// stub 返回 Cancel（调用方语义 = 退出应用）。诚实降级：LOG_WARN 说明本次未与用户交互，
// 绝不静默假装用户做了选择
#include "message_box.h"
#include "core/logging.h"

namespace platform::msgbox {

Choice show(const std::string& title, const std::string& message, bool withCancel) {
    // Cancel 对应调用方两条路径的兜底语义都是「退出应用」——无对话框可用时，
    // 损坏库也确实不能继续启动，这是唯一安全的缺省
    LOG_WARN("msgbox: 本平台无阻塞对话框实现，未向用户展示（" + title + "）: " + message +
             "；按退出路径处理");
    (void)withCancel;
    return Choice::Cancel;
}

} // namespace platform::msgbox
