#include "atomic_write.h"

#include <fstream>

namespace core {

core::Result<void> atomicWriteFile(const std::filesystem::path& target,
                                   const std::string& content,
                                   const std::string& label) {
    std::filesystem::path tmp = target;
    tmp += ".devdesk-tmp";
    {
        // 权限先就位再打开写句柄：POSIX 权限位在 open(O_CREAT) 时定死，事后
        // chmod 追不回已被别人 open 的 fd（调用点有含明文 API key 的文件）
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
