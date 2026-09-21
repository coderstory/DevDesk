// src/core/paths_win.cpp
// Windows 平台实现：appDataDir = %APPDATA%/DevDesk
#include "paths.h"

#include <cstdlib>

namespace fs = std::filesystem;

namespace core::paths {

fs::path appDataDir() {
    const char* appdata = std::getenv("APPDATA");
    fs::path base(appdata ? appdata : "");
    return base / "DevDesk";
}

} // namespace core::paths
