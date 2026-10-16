// src/module_settings/local_git_probe.h
// 本地 git 可用性探测（决策 #15 / 审计 F8）：结果只驱动「本地版本历史」可用
// 与否的卡片灰字与首次警告，不参与同步判定。平台差异走 *_macos.cpp / *_win.cpp，
// 业务侧只问「本机 git 可用吗」。
// F8：探测必须在后台线程调用（macOS 实现内部有 3s 超时上界，但仍是阻塞式）；
// 结果由调用方缓存，配置变更时失效重探。
#pragma once

namespace module::settings {

// 探测只做判定、不执行 git 子命令：macOS 直接调 /usr/bin/git 会触发 CLT
// 安装 GUI（系统弹窗），故 macOS 侧先 xcode-select -p 再查文件存在性；
// Windows 侧探 PATH + 两个已知安装位置，纯文件系统检查。
bool localGitAvailable();

}  // namespace module::settings
