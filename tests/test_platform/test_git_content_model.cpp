// tests/test_platform/test_git_content_model.cpp
// git 管理器批次三（need:10）：右栏内容数据面——FileContent::readFromDisk 四态判据
//   （F-11）：空 / 二进制（首 8KiB 含 NUL）/ 超大（>1 MiB）/ 非 UTF-8 编码 / 读取失败
//   + Ok 态行切分（\r 剥除、换行结尾不产生末尾空行）。临时文件直打，零 git 依赖。
#include "doctest.h"
#include "test_support.h"

#include "module_git/git_content_model.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using module::git::ContentState;
using module::git::FileContent;

namespace {

fs::path writeTemp(const std::string& name, const std::string& bytes) {
    static const fs::path dir =
        test_support::makeTempDir("devdesk_content_model_", "case");
    const fs::path p = dir / name;
    std::ofstream f(p, std::ios::binary);
    f << bytes;
    f.close();
    return p;
}

} // namespace

REGISTER_TEST(git_content_ok_split, "git_content: Ok 态行切分与 CRLF 剥除", {
    const auto p = writeTemp("ok.txt", "first\nsecond\r\nthird");  // 末行无换行
    const auto fc = FileContent::readFromDisk(p);
    DOCTEST_CHECK(fc.state == ContentState::Ok);
    DOCTEST_CHECK(fc.lines.size() == 3);
    DOCTEST_CHECK(fc.lines[0] == "first");
    DOCTEST_CHECK(fc.lines[1] == "second");  // \r 剥除
    DOCTEST_CHECK(fc.lines[2] == "third");
    return true;
})

REGISTER_TEST(git_content_trailing_newline, "git_content: 换行结尾不产生末尾空行", {
    const auto p = writeTemp("tail.txt", "a\nb\n");
    const auto fc = FileContent::readFromDisk(p);
    DOCTEST_CHECK(fc.state == ContentState::Ok);
    DOCTEST_CHECK(fc.lines.size() == 2);
    DOCTEST_CHECK(fc.lines[1] == "b");
    return true;
})

REGISTER_TEST(git_content_four_states, "git_content: 空/二进制/超大/坏编码四态判据", {
    {
        const auto fc = FileContent::readFromDisk(writeTemp("empty.txt", ""));
        DOCTEST_CHECK(fc.state == ContentState::Empty);
    }
    {
        // 首 8KiB 内含 NUL = 二进制
        const auto fc =
            FileContent::readFromDisk(writeTemp("bin.txt", std::string("text\x00binary", 11)));
        DOCTEST_CHECK(fc.state == ContentState::Binary);
    }
    {
        // >1 MiB：占位优先，不做内容扫描（含 NUL 也不得报二进制——超大先短路）
        std::string big(1024 * 1024 + 1, 'a');
        big[10] = '\0';
        const auto fc = FileContent::readFromDisk(writeTemp("big.txt", big));
        DOCTEST_CHECK(fc.state == ContentState::Oversize);
    }
    {
        // 恰好 1 MiB：不超限 → UTF-8 有效 → Ok
        std::string exact(1024 * 1024, 'x');
        const auto fc = FileContent::readFromDisk(writeTemp("exact.txt", exact));
        DOCTEST_CHECK(fc.state == ContentState::Ok);
    }
    {
        // GBK 双字节（0xCES 段非法 UTF-8 组合）→ 编码不支持
        const std::string gbk = "\xC4\xE3\xBA\xC3";  // 「你好」GBK 编码
        const auto fc = FileContent::readFromDisk(writeTemp("gbk.txt", gbk));
        DOCTEST_CHECK(fc.state == ContentState::BadEncoding);
    }
    return true;
})

REGISTER_TEST(git_content_utf8_edges, "git_content: UTF-8 校验拒绝过长/代理区/截断", {
    {
        // 过长编码（C0 AF = '/' 的 2 字节过长形）→ BadEncoding
        const auto fc =
            FileContent::readFromDisk(writeTemp("overlong.txt", "\xC0\xAF"));
        DOCTEST_CHECK(fc.state == ContentState::BadEncoding);
    }
    {
        // 代理区（ED A0 80 = U+D800）→ BadEncoding
        const auto fc =
            FileContent::readFromDisk(writeTemp("surrogate.txt", "\xED\xA0\x80"));
        DOCTEST_CHECK(fc.state == ContentState::BadEncoding);
    }
    {
        // 截断多字节（行尾悬挂首字节）→ BadEncoding
        const auto fc =
            FileContent::readFromDisk(writeTemp("trunc.txt", "ok\xe4\xb8"));
        DOCTEST_CHECK(fc.state == ContentState::BadEncoding);
    }
    {
        // 合法中文 + emoji（4 字节）→ Ok
        const auto fc =
            FileContent::readFromDisk(writeTemp("cjk.txt", "中文 \xF0\x9F\x98\x80\n"));
        DOCTEST_CHECK(fc.state == ContentState::Ok);
        DOCTEST_CHECK(fc.lines.size() == 1);
    }
    return true;
})

REGISTER_TEST(git_content_read_error, "git_content: 读取失败态（不存在/目录不可读）", {
    const auto dir = test_support::makeTempDir("devdesk_content_model_", "err");
    const auto fc = FileContent::readFromDisk(dir / "不存在.txt");
    DOCTEST_CHECK(fc.state == ContentState::ReadError);
    DOCTEST_CHECK(!fc.errorMessage.empty());
    return true;
})

REGISTER_TEST(git_content_unicode_path, "git_content: 中文文件名路径直读（F-10 锚）", {
    const auto p = writeTemp("说明文档.txt", "中文内容\n");
    const auto fc = FileContent::readFromDisk(p);
    DOCTEST_CHECK(fc.state == ContentState::Ok);
    DOCTEST_CHECK(fc.lines.size() == 1);
    DOCTEST_CHECK(fc.lines[0] == "中文内容");
    return true;
})
