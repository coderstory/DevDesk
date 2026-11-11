// src/platform/single_instance_macos.cpp
// macOS 单实例：.app bundle 由系统保证单实例——再次打开同一 .app 时，系统不会启动
// 新进程，而是将 openFile 事件路由到已运行的实例。OS 原生行为无需任何代码，这里恒 true。
#include "platform/single_instance.h"

namespace platform::single_instance {

bool acquireInstanceLock() {
    return true;
}

bool notifyExistingInstance(const std::vector<std::string>& /*paths*/,
                            unsigned /*ackTimeoutMs*/) {
    // macOS .app bundle：再次打开时 OS 自动路由到既有实例（Apple Event 带路径），
    // 无需程序间通信，亦无「第二实例」形态
    return false;
}

void setOpenPathsHandler(PathsHandler /*handler*/) {}

bool consumeSectionSignalOnce() { return false; }

void setSingleInstanceRootForTest(const std::string& /*root*/) {}

bool writeSectionForTest(const std::string& /*bytes*/) { return false; }

} // namespace platform::single_instance
