// src/core/paths_posix.cpp
// 其他平台（Linux 等）实现：appDataDir = ~/.local/share/DevDesk
#include "paths.h"

#include <cstdlib>

namespace fs = std::filesystem;

namespace core::paths {

fs::path appDataDir() {
    const char* home = std::getenv("HOME");
    fs::path base(home ? home : "");
    return base / ".local" / "share" / "DevDesk";
}

} // namespace core::paths
