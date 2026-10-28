// src/module_memory/engine/python_locator_win.cpp
// Windows 候选清单：uv 管的 cpython-3.12 → 官方安装器目录 → PATH。
// 顺序即优先级（任务书定案：uv 管的 3.12 优先于 PATH；本机默认 3.14 在版本判定步被拦）。
// uv 安装目录两处都找：%APPDATA%\uv\python 是当前 uv 默认（Roaming，live 验证实测落此），
// %LOCALAPPDATA%\uv\python 是旧版 uv 目录——两处都 glob，存在才算候选。
#include "python_locator.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace module::memory::engine {

namespace {

namespace fs = std::filesystem;

// 展开目录模式：取匹配 cpython-3.12* 的子目录，按名字降序（新补丁版优先）
std::vector<std::string> globUvPythons(const fs::path& uvRoot) {
    std::error_code ec;
    if (!fs::exists(uvRoot, ec) || ec) return {};
    std::vector<fs::path> dirs;
    for (const fs::directory_entry& e : fs::directory_iterator(uvRoot, ec)) {
        if (ec) break;
        const std::string name = e.path().filename().string();
        if (name.rfind("cpython-3.12", 0) == 0) dirs.push_back(e.path());
    }
    std::sort(dirs.rbegin(), dirs.rend());
    std::vector<std::string> out;
    for (const fs::path& d : dirs) {
        out.push_back((d / "python.exe").string());
    }
    return out;
}

} // namespace

std::vector<std::string> pythonCandidatePaths() {
    std::vector<std::string> out;
    // 1) uv 管的 3.12（%APPDATA%\uv\python\ 与 %LOCALAPPDATA%\uv\python\）
    for (const char* env : {"APPDATA", "LOCALAPPDATA"}) {
        if (const char* base = std::getenv(env)) {
            for (std::string& p :
                 globUvPythons(fs::path(base) / "uv" / "python")) {
                out.push_back(std::move(p));
            }
        }
    }
    // 2) 官方安装器默认目录（Python310/311/312）
    if (const char* localAppData = std::getenv("LOCALAPPDATA")) {
        const fs::path base = fs::path(localAppData) / "Programs" / "Python";
        for (const char* v : {"Python312", "Python311", "Python310"}) {
            out.push_back((base / v / "python.exe").string());
        }
    }
    // 3) PATH 上的 python.exe / python3.exe（3.14 会 exec 失败于版本判定）
    out.push_back("python.exe");
    out.push_back("python3.exe");
    return out;
}

} // namespace module::memory::engine
