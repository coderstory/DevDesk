// src/platform/single_instance.h
// 单实例互斥 + 二次运行唤起首个实例窗口
// 头文件平台无关；实现按平台拆文件（与 keychain / tray_icon 同一模式）
#pragma once

namespace platform::single_instance {

// 第一实例拿锁返回 true，第二实例发现锁已存在返回 false。
// Windows：命名互斥量 Local\DevDesk.SingleInstance.Mutex；首实例同时启动监听线程。
// macOS / POSIX：恒 true（.app bundle 由系统保证单实例；其它 POSIX 桌面环境同理）。
bool acquireInstanceLock();

// 向已运行的第一实例发送「唤起主窗口」信号。
// 仅在 acquireInstanceLock() 返回 false 之后调用。macOS / POSIX：no-op。
void notifyExistingInstance();

} // namespace platform::single_instance