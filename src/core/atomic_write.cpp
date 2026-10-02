// src/core/atomic_write.cpp
#include "atomic_write.h"

#include <fstream>

namespace core {

core::Result<void> atomicWriteFile(const std::filesystem::path& target,
                                   const std::string& content,
                                   const std::string& label) {
    std::filesystem::path tmp = target;
    tmp += ".devdesk-tmp";
    {
        // 权限**先**就位再打开写句柄：POSIX 上权限位在 open(O_CREAT) 那一刻定死，
        // 事后 chmod 追不回已被别的用户 open 住的 fd——那把 fd 之后仍能读到我们
        // 写进去的内容（本工具的调用点里有含明文 API key 的文件）。见
        // detail::prepareRestrictedFile。
        std::error_code permEc;
        if (!detail::prepareRestrictedFile(tmp, permEc)) {
            return core::Result<void>::fail(
                core::ErrorCode::Internal,
                label + "临时文件创建失败 " + tmp.string() + ": " + permEc.message());
        }
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            return core::Result<void>::fail(core::ErrorCode::Internal,
                                            label + "写入失败: " + tmp.string());
        }
        f << content;
        f.close();
        if (!f) {
            std::error_code rmEc;
            std::filesystem::remove(tmp, rmEc);
            return core::Result<void>::fail(
                core::ErrorCode::Internal, label + "写入未完成: " + tmp.string());
        }
    }
    // 覆盖式替换：平台语义差异（POSIX 的 rename 即原子替换；MinGW 走先删再 rename）
    // 收在 core::detail::replaceFile 的两个平台实现里（atomic_write_win.cpp /
    // atomic_write_posix.cpp，CMake 按平台选源），此处只判成败。
    std::error_code ec;
    detail::replaceFile(tmp, target, ec);
    if (ec) {
        std::error_code rmEc;
        std::filesystem::remove(tmp, rmEc);
        return core::Result<void>::fail(
            core::ErrorCode::Internal,
            label + "替换失败 " + target.string() + ": " + ec.message());
    }
    return core::Result<void>::ok();
}

} // namespace core
