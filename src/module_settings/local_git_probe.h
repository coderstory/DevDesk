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

// 「排空子进程管道」的每圈决策（平台无关的优先级规则，macOS 实现消费；
// 判据 tests/test_modules/test_local_git_probe.cpp）。优先级：
//   ① 子进程已退出且已见 EOF → Done（东西收全了，杀一个死进程无意义）；
//   ② 子进程**还活着**且已过截止时间 → Timeout（调用方 SIGKILL 收尸）；
//   ③ 其余 → ReadMore。
// ②是这条规则的全部意义：deadline 检查曾被写在「读到数据 → continue」之后 ——
// 子进程持续输出时循环永不查表（3s 上界名存实亡、探测线程卡死）。**判据 ①** 就是
// 那种病态输入；改本函数等于改超时语义，先看判据。
enum class PipeDrainStep { ReadMore, Done, Timeout };

inline PipeDrainStep pipeDrainStep(bool childExited, bool sawEof,
                                   bool pastDeadline) {
    if (childExited && sawEof) return PipeDrainStep::Done;
    if (!childExited && pastDeadline) return PipeDrainStep::Timeout;
    return PipeDrainStep::ReadMore;
}

// 实际判定：测试覆盖优先，否则走平台实现。
inline bool localGitAvailable() {
    const int forced = localGitProbeOverrideForTest();
    if (forced >= 0) return forced != 0;
    return localGitAvailableImpl();
}

}  // namespace module::settings
