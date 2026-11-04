// src/module_memory/engine/python_locator_win.cpp
// Windows 候选清单：uv 管的 cpython-3.12 → 官方安装器目录 → **py 启动器** → PATH。
// 顺序即优先级（任务书定案：uv 管的 3.12 优先于 PATH）。
// uv 安装目录两处都找：%APPDATA%\uv\python 是当前 uv 默认（Roaming，live 验证实测落此），
// %LOCALAPPDATA%\uv\python 是旧版 uv 目录——两处都 glob，存在才算候选。
// py 启动器（bug157 真机打回 2026-11-04）：C:\Windows\py.exe 经注册表（PEP 514）解析
// 用户装的 Python——官方目录布局之外的安装（本机 C:\Tools\Python\3.13）只有它够得着；
// PATH 的 python.exe/python3.exe 常被 WindowsApps Store stub 占位（rc=49 假候选），
// 开发 shell 的 PATH 与 app 继承的用户 PATH 不同——E2E 镜像脚本跑在前者会漏掉本缺陷
// （环境错配盲区），候选验证必须按 app 环境口径。py.exe 无参取最新注册版，未来若装
// 3.14 会随版本闸拦下（3.10~3.13 合格域由 python_locator.cpp 定）。
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
    // 3) py 启动器（bug157：注册表解析，官方布局外的安装唯一可靠入口）
    out.push_back("py.exe");
    // 4) PATH 上的 python.exe / python3.exe（3.14 会 exec 失败于版本判定；
    //    Store stub 版本探针返回空/非零，在版本判定步天然落选）
    out.push_back("python.exe");
    out.push_back("python3.exe");
    return out;
}

} // namespace module::memory::engine
