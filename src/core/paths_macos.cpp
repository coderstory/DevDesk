// src/core/paths_macos.cpp
// macOS 平台实现：appDataDir = ~/Library/Application Support/DevDesk
#include "paths.h"

#include <cstdlib>

namespace fs = std::filesystem;

namespace core::paths {

fs::path appDataDir() {
    const char* home = std::getenv("HOME");
    fs::path base(home ? home : "");
    return base / "Library" / "Application Support" / "DevDesk";
}

} // namespace core::paths
