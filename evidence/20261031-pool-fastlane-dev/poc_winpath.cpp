// POC：验证 normalizeSourceFile 对 Windows __FILE__ 形态的归一化行为
// （与 tests/test_main.cpp 同源逻辑，跑完删除或留证据，不入仓库）
#include <filesystem>
#include <string>
#include <cstdio>
static std::string normalizeSourceFile(const char* raw) {
    std::string s = raw ? raw : "";
    for (char& c : s) if (c == '\\') c = '/';
    std::error_code ec;
    return std::filesystem::path(s).lexically_normal().generic_string();
}
static int fails = 0;
static void expect(bool ok, const char* label, const std::string& got) {
    if (!ok) { ++fails; std::printf("FAIL %s -> %s\n", label, got.c_str()); }
    else std::printf("ok   %s -> %s\n", label, got.c_str());
}
int main() {
    // 形态1：Windows 相对反斜杠（ninja/MinGW 可能形态，doctest.h repoRoot 注释证实）
    auto a = normalizeSourceFile("..\\tests\\test_modules\\test_md_highlight.cpp");
    expect(a.find("test_md_") != std::string::npos, "win-relative-backslash file:test_md_", a);
    // 形态2：Windows 绝对正斜杠
    auto b = normalizeSourceFile("C:/ws/DevDesk/tests/test_modules/test_markdown.cpp");
    expect(b.find("test_markdown") != std::string::npos, "win-absolute-forward file:test_markdown", b);
    // 形态3：POSIX 绝对（本机实测形态）
    auto c = normalizeSourceFile("/Users/x/DevDesk/tests/test_modules/test_markdown_service.cpp");
    expect(c.find("file:test_markdown") == std::string::npos, "posix file: full-path no false hit", c);
    expect(c.find("test_markdown") != std::string::npos, "posix file:test_markdown", c);
    // 形态4：中间有 .. 的绝对路径（词法正规化折叠）
    auto d = normalizeSourceFile("/ws/repo/tests/../tests/test_core/test_theme.cpp");
    expect(d.find("tests/test_core/test_theme.cpp") != std::string::npos, "dotdot-fold", d);
    // 形态5：双反斜杠混合
    auto e = normalizeSourceFile("..\\tests\\test_core\\..\\tests\\test_md_folder_tree.cpp");
    expect(e.find("test_md_folder_tree") != std::string::npos, "mixed-dotdot-backslash", e);
    // 形态6：file: 词为目录前缀（快 lane 清单核心用法）
    auto f = normalizeSourceFile("/ws/DevDesk/tests/test_core/test_router.cpp");
    expect(f.find("test_core/") != std::string::npos, "dir-prefix file:test_core/", f);
    std::printf(fails == 0 ? "POC ALL PASS\n" : "POC %d FAIL\n", fails);
    return fails;
}
