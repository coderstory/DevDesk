// tests/probes/wallpaper_trash_probe.cpp — need356 B1 平台废纸篓能力真机 probe
//
// 被测对象：platform::file_trash::moveToTrash()。doctest 用例只能验 POSIX 兜底档
// （诚实 TrashUnavailable）；「真把文件挪进 ~/.Trash」这件事必须真机做，否则
// F-1「删除当前壁纸素材」的唯一删除动作就是没验过的。
//
// 为什么必须分条验：
//   ① 成功路径：临时文件 → moveToTrash → ok()==true → 原路径消失 → 废纸篓出现该文件。
//      判据用「文件名前缀」（系统遇重名会改名成 `<name> <HH-MM-SS.mmm>.mp4`），
//      本 probe 的文件名带 pid + 进程内序号，理论上不会撞名，但前缀匹配对两种形态都成立。
//   ② NotFound：不存在的绝对路径 → TrashFailure::NotFound（文案分流靠它，报错不能混）。
//   ③ PermissionDenied：只读目录里的文件 → 权限档。环境强制不了（root / 权限模型不同）
//      时跳过该子项，绝不假绿。
//
// 副作用纪律：probe 会往真实 ~/.Trash 写一个文件，RAII 守卫在**任何**退出路径
// （含断言失败早退）都把它删掉——测试机废纸篓不允许留垃圾。
//
// TCC 现实：macOS 未授予「完全磁盘访问」时，进程对 ~/.Trash 的 readdir 会被拒
// （EPERM），但按精确路径 stat/remove 仍可用。所以「出现了没」走双通道：能列目录
// 就前缀匹配，列不了就退化成精确名 stat（本 probe 名字唯一，不改名）。两条通道
// 都观察不到时跳过，不报红。
#include "test_support.h"

#include "platform/file_trash.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#if defined(__APPLE__)
#include <sys/types.h>
#include <unistd.h>  // geteuid
#endif

namespace {

constexpr int kSkipExitCode = 77;

#if defined(__APPLE__)

namespace fs = std::filesystem;
using platform::file_trash::TrashFailure;

bool gPassed = true;

void check(bool ok, const std::string& what) {
    if (ok) {
        std::cout << "  [ok] " << what << "\n";
    } else {
        gPassed = false;
        std::cerr << "wallpaper_trash_probe: FAIL——" << what << "\n";
    }
}

// ~/.Trash（HOME 缺失返回空；空 = 本 probe 什么都验不了，调用方跳过）
fs::path trashDir() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') return {};
    return fs::path(home) / ".Trash";
}

// 列 ~/.Trash 顶层文件名。被 TCC 拒读时返回 false（不报红，调用方退化到精确名判据）。
bool listTrash(const fs::path& dir, std::vector<std::string>* names) {
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec) return false;
    const fs::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) return false;
        names->push_back(it->path().filename().string());
    }
    return true;
}

// 前缀命中：精确同名，或系统的重名改名形态 `<stem> <HH-MM-SS.mmm>.<ext>`。
bool matchesTrashEntry(const std::string& entry, const std::string& fullName,
                       const std::string& stem) {
    if (entry == fullName) return true;
    return entry.rfind(stem, 0) == 0;
}

// RAII：析构时把 probe 造进废纸篓的那个文件删掉。名字带 pid + 序号，stem 唯一，
// 前缀匹配不会误删用户自己的文件。
struct TrashCleanup {
    fs::path dir;
    std::string stem;
    std::string fullName;

    ~TrashCleanup() {
        std::vector<std::string> names;
        if (listTrash(dir, &names)) {
            for (const std::string& name : names) {
                if (!matchesTrashEntry(name, fullName, stem)) continue;
                std::error_code ec;
                fs::remove(dir / name, ec);
            }
            return;
        }
        // 列不了（TCC）：精确名删——本 probe 名字唯一，不会改名。
        std::error_code ec;
        fs::remove(dir / fullName, ec);
    }
};

// RAII：把只读目录的权限位还原（否则 TempDir 析构删不掉）。
struct ScopedPerms {
    fs::path path;
    fs::perms restore;
    ~ScopedPerms() {
        std::error_code ec;
        fs::permissions(path, restore, fs::perm_options::replace, ec);
    }
};

int runProbe() {
    const fs::path dir = trashDir();
    if (dir.empty()) {
        std::cout << "wallpaper_trash_probe: HOME 未设置，无法定位 ~/.Trash，跳过"
                  << " exit=" << kSkipExitCode << "\n";
        return kSkipExitCode;
    }
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::cout << "wallpaper_trash_probe: ~/.Trash 不存在，跳过"
                  << " exit=" << kSkipExitCode << "\n";
        return kSkipExitCode;
    }

    test_support::TempDir tmp("wallpaper_trash_probe");
    // TempDir 名字已含 pid + 进程内序号 → 天然唯一，用作废纸篓里的前缀。
    const std::string stem = tmp.path.filename().string();
    const std::string fullName = stem + ".mp4";

    // ---- ① 成功路径 ----
    const fs::path src = tmp.file(fullName.c_str());
    tmp.write(src, "wallpaper trash probe payload");
    TrashCleanup cleanup{dir, stem, fullName};

    const auto outcome = platform::file_trash::moveToTrash(src.string());
    check(outcome.ok(), "moveToTrash 返回 ok（wallpaper 素材已移废纸篓）");
    check(!tmp.exists(src), "原路径已不存在（移走后源文件消失）");

    std::vector<std::string> names;
    const bool listed = listTrash(dir, &names);
    bool found = false;
    std::string foundName;
    if (listed) {
        for (const std::string& name : names) {
            if (matchesTrashEntry(name, fullName, stem)) {
                found = true;
                foundName = name;
                break;
            }
        }
    } else {
        found = fs::exists(dir / fullName, ec);
        foundName = fullName;
    }
    if (!listed && !found) {
        // 既列不出也 stat 不到：本环境无法观察废纸篓，诚实跳过（不红）。
        std::cout << "wallpaper_trash_probe: ~/.Trash 既不可读也 stat 不到条目，"
                     "环境无法观察废纸篓，跳过 exit=" << kSkipExitCode << "\n";
        return kSkipExitCode;
    }
    check(found, "~/.Trash 下出现了该文件（列目录前缀匹配 / 精确名 stat）");
    if (found) std::cout << "       废纸篓条目：" << foundName << "\n";

    // ---- ② NotFound ----
    const fs::path missing = tmp.file("wallpaper_trash_probe_missing.mp4");
    const auto notFound = platform::file_trash::moveToTrash(missing.string());
    check(notFound.failure == TrashFailure::NotFound,
          "不存在的绝对路径 → TrashFailure::NotFound");

    // ---- ③ PermissionDenied（环境强制不了则跳过该子项） ----
    if (::geteuid() == 0) {
        std::cout << "  [skip] 以 root 运行，权限位不阻断文件操作，PermissionDenied 子项跳过\n";
    } else {
        const fs::path roDir = tmp.path / "wallpaper_trash_probe_ro";
        fs::create_directories(roDir, ec);
        const fs::path roFile = roDir / "wallpaper_trash_probe_denied.mp4";
        {
            std::ofstream out(roFile, std::ios::binary | std::ios::trunc);
            out << "x";
        }
        const fs::perms original = fs::status(roDir, ec).permissions();
        ScopedPerms restorePerms{roDir, original};
        // 目录只读（去掉 owner 写位）：删除其中文件会被系统拒。
        fs::permissions(roDir, fs::perms::owner_read | fs::perms::owner_exec,
                        fs::perm_options::replace, ec);
        const auto denied = platform::file_trash::moveToTrash(roFile.string());
        // 先还原写位再判定，保证清理路径可用。
        fs::permissions(roDir, fs::perms::owner_all,
                        fs::perm_options::replace, ec);
        if (denied.ok()) {
            std::cout << "  [skip] 只读目录未能阻断删除（文件系统/权限模型不强制），"
                         "PermissionDenied 子项跳过\n";
        } else {
            check(denied.failure == TrashFailure::PermissionDenied,
                  "只读目录中的文件 → TrashFailure::PermissionDenied");
        }
    }

    if (!gPassed) {
        std::cerr << "wallpaper_trash_probe: FAIL\n";
        return 1;
    }
    std::cout << "wallpaper_trash_probe: PASS\n";
    return 0;
}

#endif  // __APPLE__

}  // namespace

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
#if defined(__APPLE__)
    return runProbe();
#else
    std::cout << "wallpaper_trash_probe: 非 macOS 平台，跳过（真废纸篓能力仅 macOS 验证；"
                 "POSIX 兜底档由 doctest 用例把守） exit=" << kSkipExitCode << "\n";
    return kSkipExitCode;
#endif
}
