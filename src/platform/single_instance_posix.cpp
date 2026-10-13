// src/platform/single_instance_posix.cpp
// POSIX 单实例：桌面环境通常由启动器/.desktop 文件保证单实例，无通用内核级机制。
// 暂 no-op 恒 true。将来需要时可走 Unix domain socket 或文件锁（flock）实现。
#include "platform/single_instance.h"

namespace platform::single_instance {

bool acquireInstanceLock() {
    return true;
}

void notifyExistingInstance() {
    // POSIX 桌面暂不实现
}

} // namespace platform::single_instance