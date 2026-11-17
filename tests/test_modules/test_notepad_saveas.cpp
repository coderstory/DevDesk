// tests/test_modules/test_notepad_saveas.cpp
// need289 修复棒 1 —— F-04 另存为落地单点无头直测（实现
// src/module_notepad/notepad_file_io.cpp notepadSaveSessionAsTo）：session
// 迁移字段矩阵（path/normKey/title/rawBytes/基线三件/enc/mtime/脏态/语言）/
// 写失败零副作用 / 干净缓冲字节直通 / 含中文空格路径 fs::path 宽路径纪律。
// 对话框面（取消零副作用/OS 覆盖确认）不可无头化 = probe H 段守卫断言 +
// 真机 QA（FD-1 已交付面）。
#include "doctest.h"

#include "module_notepad/notepad_encoding.h"  // kUtf8Bom
#include "module_notepad/notepad_session.h"  // notepadSaveSessionAsTo/notepadNormKeyFor/sessionSetDirty

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace np = module::notepad;

namespace {

std::filesystem::path makeFixtureDir() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      "devdesk_test_notepad_saveas";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

}  // namespace

// 脏会话迁移矩阵：BOM+CRLF 重编码落盘 → 基线三件前移 + 身份/查重键/标题/mtime
// 全量迁移 + 脏态清零（迁移代码删掉任一字段赋值必红）。
REGISTER_TEST(notepad_saveas_dirty_session_migration, "notepad_saveas: 脏会话全量迁移矩阵", {
    const std::filesystem::path dir = makeFixtureDir();
    const std::filesystem::path oldPath = dir / "old.txt";
    const std::filesystem::path newPath = dir / "new.txt";
    {
        std::ofstream f(oldPath, std::ios::binary);
        f << "orig\n";
    }
    np::NotepadSession s;
    s.path = oldPath.string();
    s.normKey = np::notepadNormKeyFor(s.path);
    s.title = np::notepadWindowTitleFor(s.path);
    s.editText = "body\n";
    s.loadText = "orig\n";
    s.hasBom = true;          // 点切后的保存档
    s.loadHasBom = false;     // 载入基线（无 BOM）
    s.eolMode = np::NotepadEol::Crlf;
    s.loadEol = np::NotepadEol::Lf;
    s.rawBytes = "orig\n";
    s.language = "Json";      // 旧扩展名映射残留（迁移必须清空重算）
    np::sessionSetDirty(s, true);

    const core::Result<void> r =
        np::notepadSaveSessionAsTo(s, newPath.string());
    DOCTEST_CHECK(r.isOk());
    const std::string expected =
        std::string(np::kUtf8Bom, 3) + "body\r\n";  // BOM 回写 + CRLF 全文重编码
    DOCTEST_CHECK(readFile(newPath) == expected);
    DOCTEST_CHECK(s.path == newPath.string());
    DOCTEST_CHECK(s.normKey == np::notepadNormKeyFor(newPath.string()));  // 查重键随新路径
    DOCTEST_CHECK(s.title == "new.txt");
    DOCTEST_CHECK(s.rawBytes == expected);
    DOCTEST_CHECK(s.loadText == "body\n");   // 基线三件前移到刚写入字节对应态
    DOCTEST_CHECK(s.loadHasBom == true);
    DOCTEST_CHECK(s.loadEol == np::NotepadEol::Crlf);
    DOCTEST_CHECK(s.mixedEol == false);
    DOCTEST_CHECK(s.hasMtime == true);
    if (s.hasMtime) {
        std::error_code ec;
        const auto mtime = std::filesystem::last_write_time(newPath, ec);
        DOCTEST_CHECK(!ec);
        DOCTEST_CHECK(s.loadMtime == mtime);  // mtime 重取（后续保存拦截基准）
    }
    DOCTEST_CHECK(s.dirty == false);          // 脏态清零
    DOCTEST_CHECK(s.skipCloseConfirm == false);
    DOCTEST_CHECK(s.language.empty());        // 语言随新路径懒求值重算
    DOCTEST_CHECK(s.enc.bom == np::NotepadBom::Utf8);
    DOCTEST_CHECK(s.enc.validUtf8 == true);
    std::filesystem::remove_all(dir);
    return true;
})

// 干净会话：字节级直通（「打开不擅自转换」落盘侧强保证），且可覆盖已存在目标
//（覆盖确认由 OS 对话框承担，PRD F-04 口径；落地单点只管写）。
REGISTER_TEST(notepad_saveas_clean_bytes_passthrough, "notepad_saveas: 干净缓冲字节直通+覆盖既有目标", {
    const std::filesystem::path dir = makeFixtureDir();
    const std::filesystem::path oldPath = dir / "src.txt";
    const std::filesystem::path newPath = dir / "dst.txt";
    {
        std::ofstream f(oldPath, std::ios::binary);
        f << "RAW\x01bytes\r\n\xEF\xBB\xBFkept";  // 非典型字节原样保留
    }
    {
        std::ofstream f(newPath, std::ios::binary);
        f << "STALE-CONTENT";  // 预置既有目标（OS 对话框已确认覆盖的前提态）
    }
    np::NotepadSession s;
    s.path = oldPath.string();
    s.normKey = np::notepadNormKeyFor(s.path);
    s.title = np::notepadWindowTitleFor(s.path);
    s.rawBytes = readFile(oldPath);
    s.loadText = s.rawBytes;
    s.editText = s.rawBytes;
    // dirty=false（干净态）

    const core::Result<void> r =
        np::notepadSaveSessionAsTo(s, newPath.string());
    DOCTEST_CHECK(r.isOk());
    DOCTEST_CHECK(readFile(newPath) == s.rawBytes);  // 字节级往返一致
    DOCTEST_CHECK(s.path == newPath.string());
    DOCTEST_CHECK(s.title == "dst.txt");
    DOCTEST_CHECK(s.dirty == false);
    std::filesystem::remove_all(dir);
    return true;
})

// 写失败零副作用：目标父目录不存在 → fail 且 session 未落任何态（path/查重键/
// 标题/基线/脏态全保持——另存失败必须保持编辑态与原身份）。
REGISTER_TEST(notepad_saveas_write_fail_zero_side_effect, "notepad_saveas: 写失败零副作用", {
    const std::filesystem::path dir = makeFixtureDir();
    const std::filesystem::path oldPath = dir / "old.txt";
    {
        std::ofstream f(oldPath, std::ios::binary);
        f << "orig\n";
    }
    np::NotepadSession s;
    s.path = oldPath.string();
    s.normKey = np::notepadNormKeyFor(s.path);
    s.title = np::notepadWindowTitleFor(s.path);
    s.editText = "body\n";
    s.loadText = "orig\n";
    s.hasBom = true;
    s.eolMode = np::NotepadEol::Crlf;
    s.rawBytes = "orig\n";
    np::sessionSetDirty(s, true);

    const std::string badTarget =
        (dir / "no_such_dir" / "x.txt").string();  // 父目录不存在
    const core::Result<void> r = np::notepadSaveSessionAsTo(s, badTarget);
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK(s.path == oldPath.string());
    DOCTEST_CHECK(s.normKey == np::notepadNormKeyFor(oldPath.string()));
    DOCTEST_CHECK(s.title == np::notepadWindowTitleFor(oldPath.string()));
    DOCTEST_CHECK(s.loadText == "orig\n");
    DOCTEST_CHECK(s.rawBytes == "orig\n");
    DOCTEST_CHECK(s.hasMtime == false);
    DOCTEST_CHECK(s.dirty == true);  // 保持未保存态
    std::filesystem::remove_all(dir);
    return true;
})

// 含中文+空格路径：fs::path 宽路径纪律（Windows 窄化陷阱免疫），落盘/回读/
// 身份迁移全链正确。
REGISTER_TEST(notepad_saveas_cjk_space_path, "notepad_saveas: 中文空格路径宽路径纪律", {
    const std::filesystem::path dir = makeFixtureDir();
    const std::filesystem::path oldPath = dir / "old.txt";
    {
        std::ofstream f(oldPath, std::ios::binary);
        f << "orig\n";
    }
    const std::filesystem::path newPath = dir / "测 试 目 录" / "新 文件.md";
    std::filesystem::create_directories(newPath.parent_path());  // 父目录存在性由调用方保证（OS 对话框契约）
    np::NotepadSession s;
    s.path = oldPath.string();
    s.normKey = np::notepadNormKeyFor(s.path);
    s.title = np::notepadWindowTitleFor(s.path);
    s.editText = "# 标题\n";
    s.loadText = "orig\n";
    s.rawBytes = "orig\n";
    np::sessionSetDirty(s, true);

    const core::Result<void> r =
        np::notepadSaveSessionAsTo(s, newPath.string());
    DOCTEST_CHECK(r.isOk());
    DOCTEST_CHECK(readFile(newPath) == "# 标题\n");
    DOCTEST_CHECK(s.path == newPath.string());
    DOCTEST_CHECK(s.normKey == np::notepadNormKeyFor(newPath.string()));
    DOCTEST_CHECK(s.title == "新 文件.md");
    DOCTEST_CHECK(s.loadText == "# 标题\n");
    std::filesystem::remove_all(dir);
    return true;
})
