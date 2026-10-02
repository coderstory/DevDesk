// tests/test_core/test_atomic_write.cpp
// 原子写的**权限时序**（审计 m2）：临时文件必须以「仅属主可读写」出生。
//
// 病根同本批其它几条：修复前的顺序是「ofstream 建 tmp（权限由 umask 定，典型
// 0644）→ 写完内容 → chmod 0600」。POSIX 权限位在 open(O_CREAT) 那一刻定死，
// 事后 chmod **追不回**此前已被别的用户 open 住的那把 fd——而那把 fd 之后仍能从
// 同一个 inode 读到我们写进去的内容。调用点里有含**明文 API key** 的文件
//（claude_service.cpp 写 run-*.json），所以这个窗口不是理论问题。
//
// 断言写在平台 helper 里而不是用例体内直接 stat：REGISTER_TEST 的宏体里不能嵌
// 预处理指令（-Wembedded-directive 直接拦），且 Windows 上压根没有 stat 模式位。
//
// Windows 上权限模型是 ACL、std::filesystem::permissions 是 no-op，本用例按
// 「不适用」跳过（那边凭据落点的真实断言在 test_credential_protection.cpp 的
// DPAPI 分支 + 数据根目录 ACL，不在本文件）。
#include "doctest.h"
#include "test_support.h"

#include "core/atomic_write.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {

std::filesystem::path tempTarget(const char* stem) {
    return test_temp::tempPath(stem);
}

// 本平台的权限位断言是否适用（Windows 恒不适用）。
bool permBitsApplicable() {
#if defined(_WIN32)
    return false;
#else
    return true;
#endif
}

// 落点文件的权限位（& 07777）。Windows 侧返回 -1，调用方不会用到。
long long fileModeBits(const std::filesystem::path& p) {
#if defined(_WIN32)
    (void)p;
    return -1;
#else
    struct stat st {};
    if (::stat(p.c_str(), &st) != 0) return -1;
    return static_cast<long long>(st.st_mode & 07777);
#endif
}

// 手工把文件设成指定权限（只为造夹具）。
bool setModeBits(const std::filesystem::path& p, long long bits) {
#if defined(_WIN32)
    (void)p;
    (void)bits;
    return false;
#else
    return ::chmod(p.c_str(), static_cast<mode_t>(bits)) == 0;
#endif
}

}  // namespace

REGISTER_TEST(atomic_write_creates_file_owner_only,
              "atomic_write: 落点文件权限 0600（凭据不曾在宽权限时段可读）", {
    if (!permBitsApplicable()) {
        DOCTEST_SKIP("Windows 权限模型是 ACL，权限位断言不适用");
    }
    const std::filesystem::path target = tempTarget("devdesk_atomic_secret.json");
    const std::string secret = R"({"apiKey":"sk-plaintext-must-not-be-readable"})";

    const core::Result<void> r = core::atomicWriteFile(target, secret, "测试文件");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK(std::filesystem::exists(target));

    // 精确 0600：只允许属主读写，且 group/other 一位都不能有
    DOCTEST_CHECK_EQ(fileModeBits(target), 0600LL);

    // 内容确实落到位（权限对了不等于写对了）
    std::ifstream in(target, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    DOCTEST_CHECK_EQ(buf.str(), secret);

    // 临时文件不留残留
    std::filesystem::path leftover = target;
    leftover += ".devdesk-tmp";
    DOCTEST_CHECK_FALSE(std::filesystem::exists(leftover));

    std::error_code ec;
    std::filesystem::remove(target, ec);
    return true;
})

REGISTER_TEST(atomic_write_narrows_pre_existing_wide_permissions,
              "atomic_write: 覆盖一个已是 0644 的旧文件 → 权限仍收紧到 0600", {
    if (!permBitsApplicable()) {
        DOCTEST_SKIP("Windows 权限模型是 ACL，权限位断言不适用");
    }
    // 模拟「上一轮崩溃 / 人工建的宽权限文件」：先手工做成 0644，再走一次
    // atomicWriteFile。保证既有宽权限落点不会被原样留下。
    const std::filesystem::path target = tempTarget("devdesk_atomic_wide.json");
    {
        std::ofstream seed(target, std::ios::binary | std::ios::trunc);
        seed << R"({"apiKey":"sk-old-wide-permissions"})";
    }
    DOCTEST_REQUIRE(setModeBits(target, 0644));
    DOCTEST_CHECK_EQ(fileModeBits(target), 0644LL);

    const core::Result<void> r = core::atomicWriteFile(
        target, R"({"apiKey":"sk-rewritten"})", "测试文件");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(fileModeBits(target), 0600LL);

    std::error_code ec;
    std::filesystem::remove(target, ec);
    return true;
})