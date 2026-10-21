// src/platform/autostart_posix.cpp
// Windows/macOS 之外的 POSIX 兜底（Linux 等）：无统一的自启动公约（XDG autostart
// 依赖具体 DE），本项目不引入——能力恒 false、注册态恒未注册、写入诚实报
// NotImplemented；注销恒幂等成功（未注册 = 无事可做）。与 single_instance_posix /
// tray_icon_posix 同款「诚实降级」口径。
#include "autostart.h"

#include <string>
#include <vector>

namespace platform::autostart {

namespace {

struct NoopOps final : public AutostartOps {
    bool isRegistered() override { return false; }
    core::Result<void> writeEntry(const std::string&) override {
        return core::Result<void>::fail(
            core::ErrorCode::NotImplemented,
            "本平台不支持开机自启动（POSIX 兜底）");
    }
    core::Result<void> removeEntry() override {
        return core::Result<void>::ok();  // 未注册 → 注销幂等成功
    }
};

} // namespace

bool supported() { return false; }

std::unique_ptr<AutostartOps> systemOps() { return std::make_unique<NoopOps>(); }

std::string executablePath() { return {}; }

std::vector<std::string> processArgsUtf8() { return {}; }

std::string buildEntryContent(const std::string&) { return {}; }

} // namespace platform::autostart
