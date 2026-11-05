// tests/test_platform/test_terminal_conpty_lookup.cpp
// debt246：ConPTY v2 redist 三级查找顺序判定（terminal_conpty_lookup.h 纯函数）。
// 覆盖：env 优先于 exe 旁、env 空白/引号清洗、exe 旁路径尾部斜杠规整、双缺=空串
// （空串=调用方回落系统 inbox CreatePseudoConsole 的契约）。头无平台依赖，全平台编译。
#include "doctest.h"

#include "platform/terminal/terminal_conpty_lookup.h"

#include <string>

using platform::terminal::conptyDirCandidate;

REGISTER_TEST(conptyLookupEnvPriority,
              "ConPTY 目录查找：env 非空恒优先，原样透传", {
    DOCTEST_CHECK(conptyDirCandidate(L"D:/libs/conpty", L"C:\\app") == L"D:/libs/conpty");
    DOCTEST_CHECK(conptyDirCandidate(L"relative\\dir", L"C:\\app") == L"relative\\dir");
    return true;
});

REGISTER_TEST(conptyLookupEnvTrim,
              "ConPTY 目录查找：env 空白/引号清洗后非空仍优先", {
    DOCTEST_CHECK(conptyDirCandidate(L"  D:\\libs\\conpty  ", L"C:\\app") == L"D:\\libs\\conpty");
    DOCTEST_CHECK(conptyDirCandidate(L"\"D:\\libs\\conpty\"", L"C:\\app") == L"D:\\libs\\conpty");
    return true;
});

REGISTER_TEST(conptyLookupEnvEmptyFallback,
              "ConPTY 目录查找：env 全空白视同未设，落 exe 旁 conpty", {
    DOCTEST_CHECK(conptyDirCandidate(L"   ", L"C:\\app") == L"C:\\app\\conpty");
    DOCTEST_CHECK(conptyDirCandidate(L"\"\"", L"C:\\app") == L"C:\\app\\conpty");
    DOCTEST_CHECK(conptyDirCandidate(L"", L"C:\\app") == L"C:\\app\\conpty");
    return true;
});

REGISTER_TEST(conptyLookupTrailingSlash,
              "ConPTY 目录查找：exe 旁路径尾部斜杠规整", {
    DOCTEST_CHECK(conptyDirCandidate(L"", L"C:\\app\\") == L"C:\\app\\conpty");
    DOCTEST_CHECK(conptyDirCandidate(L"", L"C:\\app\\\\") == L"C:\\app\\conpty");
    DOCTEST_CHECK(conptyDirCandidate(L"", L"/opt/app/") == L"/opt/app\\conpty");
    return true;
});

REGISTER_TEST(conptyLookupBothMissing,
              "ConPTY 目录查找：双缺=空串（契约：调用方回落 inbox）", {
    DOCTEST_CHECK(conptyDirCandidate(L"", L"") == L"");
    DOCTEST_CHECK(conptyDirCandidate(L"  ", L"") == L"");
    DOCTEST_CHECK(conptyDirCandidate(L"", L"  ") == L"");
    DOCTEST_CHECK(conptyDirCandidate(L"", L"\"") == L"");
    return true;
});