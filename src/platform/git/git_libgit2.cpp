// src/platform/git/git_libgit2.cpp
// 见 git_libgit2.h（need:8 批次一；方案 §5）。
#include "platform/git/git_libgit2.h"

#include <git2.h>

#include "core/logging.h"

namespace platform::git {

Libgit2Scope::Libgit2Scope() {
    const int rc = git_libgit2_init();
    if (rc < 0) {
        LOG_ERROR("git_libgit2_init failed: " + libgit2ToMessage(rc));
    }
}

Libgit2Scope::~Libgit2Scope() {
    git_libgit2_shutdown();
}

core::ErrorCode libgit2ToErrorCode(int rc) {
    const git_error* err = git_error_last();
    const char* msg = err != nullptr ? err->message : nullptr;
    if (rc == GIT_ENOTFOUND) {
        return core::ErrorCode::FileNotFound;
    }
    if (rc == GIT_EBUFS || rc == GIT_EINVALIDSPEC) {
        return core::ErrorCode::ValidationError;
    }
    // 权限面（方案 §5：EACCES/EPERM 系）：libgit2 不直接透出 errno，OS 类错误按
    // message 语义细分（MinGW "Permission denied" / Win32 "Access is denied"）
    if (err != nullptr && err->klass == GIT_ERROR_OS && msg != nullptr) {
        const std::string_view s(msg);
        if (s.find("denied") != std::string_view::npos ||
            s.find("Permission") != std::string_view::npos) {
            return core::ErrorCode::FilePermissionDenied;
        }
    }
    return core::ErrorCode::Internal;
}

std::string libgit2ToMessage(int rc) {
    const git_error* err = git_error_last();
    const char* msg = err != nullptr ? err->message : nullptr;
    if (msg == nullptr) {
        return "libgit2 " + std::to_string(rc) + ": (no error message)";
    }
    return "libgit2 " + std::to_string(rc) + ": " + msg;
}

std::string pathToUtf8(const std::filesystem::path& p) {
    // generic 形态：'/' 分隔与 libgit2 回传路径同风格（native 形态在 Windows 是 '\',
    // 与 git_repository_workdir 直接比较会假性不等）
    const std::u8string u8 = p.generic_u8string();
    return std::string(u8.begin(), u8.end());
}

std::filesystem::path utf8ToPath(std::string_view utf8) {
    const std::u8string u8(reinterpret_cast<const char8_t*>(utf8.data()),
                           reinterpret_cast<const char8_t*>(utf8.data() + utf8.size()));
    return std::filesystem::path(u8);
}

} // namespace platform::git
