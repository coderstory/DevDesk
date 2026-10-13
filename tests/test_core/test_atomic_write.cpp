// 原子写的权限时序：落点必须以 0600 **出生**。POSIX 权限位在 open(O_CREAT) 那一刻
// 定死，事后 chmod 追不回已被别的用户 open 住的那把 fd，而那把 fd 之后仍能从
// 同一 inode 读到内容。把 atomicWriteFile 重排成「建 tmp → 写 → chmod」等于重开这个
// 窗口，而调用点里有含明文 API key 的文件。
// 断言写在平台 helper 里而不是用例体内直接 stat：REGISTER_TEST 的宏体里不能嵌预处理
// 指令（-Wembedded-directive 直接拦），且 Windows 上压根没有 stat 模式位。
// Windows 上权限模型是 ACL、std::filesystem::permissions 是 no-op，本用例按「不适用」
// 跳过；那边凭据落点的真实断言在 test_credential_protection.cpp 的 DPAPI 分支 + 数据根
// 目录 ACL，不在本文件。
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

    // 精确 0600：group/other 一位都不能有，不能放宽成「至少属主可写」
    DOCTEST_CHECK_EQ(fileModeBits(target), 0600LL);

    std::ifstream in(target, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    DOCTEST_CHECK_EQ(buf.str(), secret);

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
    // 落点已有宽权限时也必须收紧到 0600，不能原样保留
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