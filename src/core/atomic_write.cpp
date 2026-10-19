#include "atomic_write.h"

namespace core {

core::Result<void> atomicWriteFile(const std::filesystem::path& target,
                                   const std::string& content,
                                   const std::string& label) {
    std::filesystem::path tmp = target;
    tmp += ".devdesk-tmp";
    {
        // 权限先就位再打开写句柄：POSIX 权限位在 open(O_CREAT) 时定死，事后 chmod 追不回已开的 fd
        std::error_code ec;
        if (!detail::prepareRestrictedFile(tmp, ec)) {
            return core::Result<void>::fail(
                core::ErrorCode::Internal,
                label + "临时文件创建失败 " + tmp.string() + ": " + ec.message());
        }
        if (!detail::writeBinaryFile(tmp, content, ec)) {
            std::error_code rmEc;
            std::filesystem::remove(tmp, rmEc);
            return core::Result<void>::fail(
                core::ErrorCode::Internal,
                label + "写入失败 " + tmp.string() + ": " + ec.message());
        }
    }
    // 覆盖式替换：平台语义差异收在 detail::replaceFile 的两个平台实现里，此处只判成败
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
