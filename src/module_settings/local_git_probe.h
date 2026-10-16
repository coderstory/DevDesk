// src/module_settings/local_git_probe.h
// 本地 git 可用性探测：结果只驱动「本地版本历史」可用
// 与否的卡片灰字与首次警告，不参与同步判定。平台差异走 *_macos.cpp / *_win.cpp，
// 业务侧只问「本机 git 可用吗」。
// 探测必须在后台线程调用（macOS 实现内部有 3s 超时上界，但仍是阻塞式）；
// 结果由调用方缓存，配置变更时失效重探。
#pragma once

namespace module::settings {

// 测试注入：强制探测结果（-1 = 不覆盖走真探测；0 = 不可用；1 = 可用）。
// 场景 14「本地 git 不可用」在真机上造不出来——开发机 git 恒可用，只能注入。
// 与 keychain.h 的 setBackendForTest 同款：仅测试调用，用完复位。
inline int& localGitProbeOverrideForTest() {
    static int v = -1;
    return v;
}

// 平台实现：探测只做判定、不执行 git 子命令——macOS 直接调 /usr/bin/git 会触发 CLT
// 安装 GUI（系统弹窗），故 macOS 侧先 xcode-select -p 再查文件存在性；
// Windows 侧探 PATH + 两个已知安装位置，纯文件系统检查。
// 业务侧不要直接调，走 localGitAvailable()。
bool localGitAvailableImpl();

// 实际判定：测试覆盖优先，否则走平台实现。
inline bool localGitAvailable() {
    const int forced = localGitProbeOverrideForTest();
    if (forced >= 0) return forced != 0;
    return localGitAvailableImpl();
}

}  // namespace module::settings
