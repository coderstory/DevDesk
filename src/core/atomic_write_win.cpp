// src/core/atomic_write_win.cpp
// core::detail::replaceFile 的 Windows 实现。按平台拆文件（接口隔离，业务代码零
// 平台宏），由 CMake 按平台选源（src/core/CMakeLists.txt）。
#include "atomic_write.h"

namespace core::detail {

void replaceFile(const std::filesystem::path& tmp,
                 const std::filesystem::path& target,
                 std::error_code& ec) {
    // 先删再 rename：源自「MinGW 的 rename 不覆盖已存在目标（底层 _wrename
    // 语义）」的历史兼容路径（设计 §5.3 开放问题 6：最后写者胜，窗口期可接受）。
    // 2026-09-28 用同工具链（MSYS2 MinGW64 g++ 16.2）探针实测 std::filesystem::
    // rename 已能覆盖已存在目标；本次平台适配只改 POSIX 一侧，Windows 行为保持
    // 冻结（一字未动），故这个 remove 仍留着——待后续单独决策再收敛。
    std::error_code rmEc;
    std::filesystem::remove(target, rmEc);
    std::filesystem::rename(tmp, target, ec);
}

} // namespace core::detail
