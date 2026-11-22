#include "mcp_config_common.h"

#include <system_error>

#include "core/atomic_write.h"
#include "core/fs_text.h"

namespace module::mcp::config_common {

std::filesystem::path backupFile(const std::filesystem::path& file) {
    std::filesystem::path bak = file;
    bak += ".bak-devdesk";
    return bak;
}

core::Result<void> rewriteExisting(
    const std::filesystem::path& file,
    const std::function<core::Result<std::string>(const std::string&, bool*)>& mutate,
    const char* label, const char* productName) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) {
        return core::Result<void>::fail(
            core::ErrorCode::FileNotFound,
            "未检测到 " + std::string(productName) + " 配置 " + file.string() +
                "（不自动创建）");
    }
    bool readFailed = false;
    const std::string current = core::readTextFile(file, &readFailed);
    if (readFailed) {
        return core::Result<void>::fail(core::ErrorCode::FileReadFailed,
                                       "读取 " + file.string() + " 失败");
    }
    bool changed = false;
    auto next = mutate(current, &changed);
    if (next.isFail()) {
        return core::propagate<void>(next);
    }
    if (!changed) return core::Result<void>::ok();  // 幂等：一字节不动

    // 备份原文件（失败即整体失败，目标文件保持原内容）
    const auto bakResult = core::atomicWriteFile(
        backupFile(file), current, std::string(productName) + " 配置备份");
    if (bakResult.isFail()) {
        return core::propagate<void>(bakResult);
    }
    return core::atomicWriteFile(file, next.value(), label);
}

}  // namespace module::mcp::config_common
