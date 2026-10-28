// src/module_memory/engine/python_locator_posix.cpp
// POSIX/macOS 候选清单。TODO(mac 实测)：DevDesk 交付平台含 macOS，本清单未经实机验证，
// mac 落地时补 uv 实际安装路径核对（Windows 侧 uv 路径已按官方布局实现）。
#include "python_locator.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace module::memory::engine {

namespace fs = std::filesystem;

std::vector<std::string> pythonCandidatePaths() {
    std::vector<std::string> out;
    // 1) uv 管的 3.12（XDG 默认 ~/.local/share/uv/python/cpython-3.12*/bin/python3）
    if (const char* home = std::getenv("HOME")) {
        std::error_code ec;
        const fs::path uvRoot = fs::path(home) / ".local" / "share" / "uv" / "python";
        if (fs::exists(uvRoot, ec) && !ec) {
            std::vector<fs::path> dirs;
            for (const fs::directory_entry& e : fs::directory_iterator(uvRoot, ec)) {
                if (ec) break;
                const std::string name = e.path().filename().string();
                if (name.rfind("cpython-3.12", 0) == 0) dirs.push_back(e.path());
            }
            for (const fs::path& d : dirs) {
                out.push_back((d / "bin" / "python3").string());
            }
        }
    }
    // 2) PATH
    out.push_back("python3.12");
    out.push_back("python3");
    return out;
}

} // namespace module::memory::engine
