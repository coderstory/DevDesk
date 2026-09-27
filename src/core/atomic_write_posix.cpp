// src/core/atomic_write_posix.cpp
// core::detail::replaceFile 的 POSIX 实现（macOS / Linux 共用）。按平台拆文件
// （接口隔离，业务代码零平台宏），由 CMake 按平台选源（src/core/CMakeLists.txt）。
#include "atomic_write.h"

namespace core::detail {

void replaceFile(const std::filesystem::path& tmp,
                 const std::filesystem::path& target,
                 std::error_code& ec) {
    // POSIX rename 覆盖已存在目标 = 原子替换：一步到位，不再"先删后建"
    //（此前为迁就 MinGW 语义所有平台都先 remove，POSIX 上的"原子写"因此退化成
    // 有崩溃窗口的非原子写——目标会在 remove 与 rename 之间短暂消失）。
    std::filesystem::rename(tmp, target, ec);
}

} // namespace core::detail
