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
    // MinGW 的 std::filesystem::rename 不覆盖已存在目标（底层 _wrename
    // 语义），先删再 rename（设计 §5.3 开放问题 6：最后写者胜，窗口期可接受）
    std::error_code ec;
    std::filesystem::remove(target, ec);
    std::filesystem::rename(tmp, target, ec);
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
