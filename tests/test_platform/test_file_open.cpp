// tests/test_platform/test_file_open.cpp
// platform::file_open 覆盖：
//   ① openDirectory（产品审计 2026-09-23 [15]）：只测失败路径——成功路径会
//      真的拉起资源管理器/Finder 窗口，属人工点验项；失败路径无 UI 副作用
//      （Win 侧 SEE_MASK_FLAG_NO_UI）。
//   ② openFileFromArgv（审计 [17]，此前 tests/ 零覆盖）：进程自身命令行不可
//      注入，故测其抽出的两段纯逻辑——「argv → 选文件参数」selectOpenFileArg
//      （ASCII/非 ASCII/无参/多参/flag）与 Windows UTF-16→UTF-8 编码链
//      wideToUtf8ForTest（W5 非 ASCII 路径乱码修复 90d5792 的回归锁）。
//   ③ openFileFromArgv 本尊 smoke：不崩、不抛、返回类型正确（测试跑时 argv
//      含过滤子串，内容不断言，只锁「任何输入都安全返回」）。
#include "doctest.h"

#include "platform/file_open.h"

#include <filesystem>
#include <string>
#include <vector>

REGISTER_TEST(file_open_directory_empty_rejected,
              "file_open: openDirectory 空路径 → ValidationError", {
    auto r = platform::file_open::openDirectory("");
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK(r.errorCode() == core::ErrorCode::ValidationError);
    return true;
})

REGISTER_TEST(file_open_directory_missing_fails,
              "file_open: openDirectory 不存在目录 → 失败返回（不弹窗）", {
    namespace fs = std::filesystem;
    const auto p = test_temp::tempPath("devdesk_file_open_no_such_dir_9e31");
    std::error_code ec;
    fs::remove_all(p, ec);  // 确保不存在（幂等，防上次残留）
    auto r = platform::file_open::openDirectory(p.string());
    DOCTEST_CHECK(r.isFail());
    return true;
})

// ============ ② argv → 文件路径纯选择（openFileFromArgv 的选参数段） ============

REGISTER_TEST(file_open_select_arg_paths,
              "file_open: selectOpenFileArg ASCII/空/无参/多参/flag（审计 [17]）", {
    using platform::file_open::selectOpenFileArg;
    std::vector<std::string> args;

    // 无参（空数组）→ 空
    DOCTEST_CHECK_EQ(selectOpenFileArg(args), std::string(""));

    // 只有 exe → 空（argv[1] 不存在）
    args.push_back("devdesk.exe");
    DOCTEST_CHECK_EQ(selectOpenFileArg(args), std::string(""));

    // ASCII 文件路径 → 原样返回
    args.push_back("D:/notes/hello.md");
    DOCTEST_CHECK_EQ(selectOpenFileArg(args), std::string("D:/notes/hello.md"));

    // 多参：只看 argv[1]，argv[2] 及以后忽略
    args.push_back("D:/notes/second.md");
    DOCTEST_CHECK_EQ(selectOpenFileArg(args), std::string("D:/notes/hello.md"));

    // argv[1] 为空串 → 空
    std::vector<std::string> emptyArg;
    emptyArg.push_back("devdesk.exe");
    emptyArg.push_back("");
    DOCTEST_CHECK_EQ(selectOpenFileArg(emptyArg), std::string(""));

    // argv[1] 以 '-' 开头 = flag → 跳过返回空
    std::vector<std::string> flagArg;
    flagArg.push_back("devdesk.exe");
    flagArg.push_back("--log");
    DOCTEST_CHECK_EQ(selectOpenFileArg(flagArg), std::string(""));

    // 单独一个 '-' 也算 flag（首字符判据）
    flagArg[1] = "-";
    DOCTEST_CHECK_EQ(selectOpenFileArg(flagArg), std::string(""));
    return true;
})

REGISTER_TEST(file_open_select_arg_non_ascii,
              "file_open: selectOpenFileArg 非 ASCII（中文/日文/emoji）原样往返（审计 [17]）", {
    using platform::file_open::selectOpenFileArg;
    std::vector<std::string> args;
    args.push_back("devdesk.exe");
    // 「中文笔记.md」UTF-8
    args.push_back("\xE4\xB8\xAD\xE6\x96\x87\xE7\xAC\x94\xE8\xAE\xB0.md");
    DOCTEST_CHECK_EQ(selectOpenFileArg(args),
                     std::string("\xE4\xB8\xAD\xE6\x96\x87\xE7\xAC\x94\xE8\xAE\xB0.md"));

    // 含空格的 UTF-8 路径原样保留（不被 token 化）
    args[1] = "C:/\xE7\xAC\x94\xE8\xAE\xB0 \xE7\x9B\xAE \xE5\xBD\x95/note.md";
    DOCTEST_CHECK_EQ(selectOpenFileArg(args), args[1]);

    // emoji 目录（4 字节 UTF-8）
    args[1] = "D:/\xF0\x9F\x90\x8D/cat.md";
    DOCTEST_CHECK_EQ(selectOpenFileArg(args), args[1]);

    // 非 ASCII 且以 flag 起始字符 '-' 开头 → 仍跳过（判据是首字节，与编码无关）
    args[1] = "-\xE4\xB8\xAD";
    DOCTEST_CHECK_EQ(selectOpenFileArg(args), std::string(""));
    return true;
})

// ============ ② Windows UTF-16 → UTF-8 编码链（W5 乱码修复 90d5792 回归锁） ============
// wideToUtf8ForTest 仅 file_open_win.cpp 定义，用例整体 _WIN32 守卫，
// macOS/Linux 不编译（避免未定义符号）。
#if defined(_WIN32)

REGISTER_TEST(file_open_win_wide_to_utf8,
              "file_open: Windows wideToUtf8 编码链 ASCII/非 ASCII/空（审计 [17]）", {
    using platform::file_open::wideToUtf8ForTest;

    // 空串 → 空
    DOCTEST_CHECK_EQ(wideToUtf8ForTest(L""), std::string(""));

    // ASCII 往返
    DOCTEST_CHECK_EQ(wideToUtf8ForTest(L"D:/notes/hello.md"),
                     std::string("D:/notes/hello.md"));

    // 「中文」= U+4E2D U+6587 → UTF-8 E4 B8 AD E6 96 87（关键回归：旧实现
    // 经 ANSI 代码页会得到 mojibake 或 '?'，这里锁 UTF-8 精确字节）
    const std::wstring cn = L"\x4E2D\x6587";
    DOCTEST_CHECK_EQ(wideToUtf8ForTest(cn),
                     std::string("\xE4\xB8\xAD\xE6\x96\x87"));

    // 「中文笔记.md」整段路径
    const std::wstring cnPath =
        L"D:/\x4E2D\x6587\x7B14\x8BB0.md";
    DOCTEST_CHECK_EQ(wideToUtf8ForTest(cnPath),
                     std::string("D:/\xE4\xB8\xAD\xE6\x96\x87\xE7\xAC\x94\xE8\xAE\xB0.md"));

    // 含空格的宽字符路径
    DOCTEST_CHECK_EQ(wideToUtf8ForTest(L"\x4E2D\x6587 note.md"),
                     std::string("\xE4\xB8\xAD\xE6\x96\x87 note.md"));

    // 编码链 + 选参数端到端：宽中文 argv[1] → UTF-8 → selectOpenFileArg 返回同串
    //（不用花括号初始化列表：REGISTER_TEST 宏按逗号切参，见本仓库既有约定）
    const std::wstring fileW = L"D:/\x7B14\x8BB0\x672C/note.md";
    std::vector<std::string> argvUtf8;
    argvUtf8.push_back("devdesk.exe");
    argvUtf8.push_back(wideToUtf8ForTest(fileW));
    DOCTEST_CHECK_EQ(platform::file_open::selectOpenFileArg(argvUtf8),
                     wideToUtf8ForTest(fileW));
    return true;
})

#endif  // _WIN32

// ============ ③ openFileFromArgv 本尊 smoke（安全返回，不崩不抛） ============

REGISTER_TEST(file_open_from_argv_smoke,
              "file_open: openFileFromArgv smoke 不崩不抛（审计 [17]）", {
    // 测试进程 argv[1] 可能是过滤子串（如 "file_open"）或空——内容不确定，
    // 只锁「无论命令行是什么，返回 string 且函数本身安全」
    const std::string r = platform::file_open::openFileFromArgv();
    DOCTEST_CHECK(r.empty() || !r.empty());   // 恒真：能正常返回即不崩
    return true;
})

