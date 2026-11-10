// tests/test_platform/test_file_association.cpp
// platform::file_association 覆盖（F-06 .md 关联登记，方案卡 §二.F-06）：
//   1. 三态判定纯函数 + 注入缝配置/登记内容契约钉死（漂移即红）。
//   2. _WIN32 注入缝实登记链路：testConfig()（DevDeskTest.md + .mdtest）写 →
//      HKCU 键值直读断言（不经被测查询通路）→ 三态回查 → 幂等双写 →
//      SHChangeNotify spy 计数 → 清理 → 清理后不再 ours。
// 测试红线：用例只用注入缝配置（DevDeskTest.md/.mdtest），禁触开发机真实 .md
// 登记；QueryCurrentDefault 真判据 = 实机 UAT 项，单测只验注入缝配置下的回读。
// 回退会红：删掉写入逻辑（writeRegistration）则直读断言/回查 ours/幂等断言全红。
#include "doctest.h"
#include "platform/file_association.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "autostart.h"        // executablePath()：与被测写入同源的 exe 路径
#include "file_open_common.h" // platform::detail::wideToUtf8（测试直读转码）
#endif

#include <string>

namespace {

#if defined(_WIN32)
// HKCU 键默认值直读（测试自证：直接读注册表，不依赖被测查询通路）
bool readDefaultSz(const wchar_t* subkey, std::string* out) {
    wchar_t buf[1024] = {};
    DWORD size = sizeof(buf);
    const LSTATUS st =
        RegGetValueW(HKEY_CURRENT_USER, subkey, nullptr, RRF_RT_REG_SZ,
                     nullptr, buf, &size);
    if (st != ERROR_SUCCESS) return false;
    *out = platform::detail::wideToUtf8(buf, ::lstrlenW(buf));
    return true;
}

// spy 计数（裸函数指针缝不可捕获，用静态计数）
int g_spyCalls = 0;
void spyNotify() { ++g_spyCalls; }
#endif // _WIN32

} // namespace

REGISTER_TEST(file_association_classify_pure,
              "fileassoc: 三态判定纯函数 + 注入缝配置与登记内容契约钉死", {
    using namespace platform::file_association;
    // 三态：查询失败=Unknown（≠Others）、回读==本应用=Ours、回读他者=Others
    DOCTEST_CHECK_EQ(classifyState(false, "Whatever", productionConfig().progId),
                     AssocState::Unknown);
    DOCTEST_CHECK_EQ(classifyState(true, "DevDesk.md", "DevDesk.md"),
                     AssocState::Ours);
    DOCTEST_CHECK_EQ(classifyState(true, "OtherApp.md", "DevDesk.md"),
                     AssocState::Others);
    // 注入缝钉死：生产/测试配置漂移即红（防用例误触真实 .md 登记）
    DOCTEST_CHECK_EQ(productionConfig().progId, std::string("DevDesk.md"));
    DOCTEST_CHECK_EQ(productionConfig().extension, std::string(".md"));
    DOCTEST_CHECK_EQ(testConfig().progId, std::string("DevDeskTest.md"));
    DOCTEST_CHECK_EQ(testConfig().extension, std::string(".mdtest"));
    // 登记内容契约：exe 与 %1 各自带引号（路径/文件名含空格不拆参）；图标首枚
    DOCTEST_CHECK_EQ(buildOpenCommandValue("C:\\Apps\\Dev Desk.exe"),
                     std::string("\"C:\\Apps\\Dev Desk.exe\" \"%1\""));
    DOCTEST_CHECK_EQ(buildDefaultIconValue("C:\\Apps\\DevDesk.exe"),
                     std::string("\"C:\\Apps\\DevDesk.exe\",0"));
    DOCTEST_CHECK_EQ(std::string(kProgIdDescription),
                     std::string("DevDesk Markdown Document"));
    return true;
})

#if defined(_WIN32)
REGISTER_TEST(file_association_registry_roundtrip,
              "fileassoc: 注入缝实登记链路（写→HKCU直读→三态回查→幂等双写→spy→清理）", {
    using namespace platform::file_association;
    const AssocConfig cfg = testConfig();  // DevDeskTest.md + .mdtest（禁 productionConfig）

    // 前置清场（上次用例残留也吞掉）：清理缝幂等
    DOCTEST_REQUIRE(removeRegistrationForTest(cfg).isOk());

    // 装 spy（保存旧值，用例退出前恢复，防跨用例泄漏）
    ShellNotifySink* const oldSink = setShellNotifySink(&spyNotify);
    g_spyCalls = 0;

    // 写入：ok 且 bool=true（写后回查归本——无 UserChoice 回落类默认值路径，
    // 同机 F-07 POC 证该回读通路生效）
    const core::Result<bool> first = setAsDefaultForMd(cfg);
    DOCTEST_REQUIRE(first.isOk());
    DOCTEST_CHECK(first.value());
    DOCTEST_CHECK_GE(g_spyCalls, 1);  // 写完必调 SHChangeNotify（官方 must-call）

    // HKCU 键值直读断言（不经被测查询通路）：ProgId 三件套 + 扩展名指向
    const std::string exe = platform::autostart::executablePath();
    DOCTEST_REQUIRE(!exe.empty());
    std::string v;
    DOCTEST_REQUIRE(readDefaultSz(L"Software\\Classes\\DevDeskTest.md", &v));
    DOCTEST_CHECK_EQ(v, std::string(kProgIdDescription));
    DOCTEST_REQUIRE(readDefaultSz(L"Software\\Classes\\DevDeskTest.md\\DefaultIcon", &v));
    DOCTEST_CHECK_EQ(v, buildDefaultIconValue(exe));
    DOCTEST_REQUIRE(readDefaultSz(
        L"Software\\Classes\\DevDeskTest.md\\shell\\open\\command", &v));
    DOCTEST_CHECK_EQ(v, buildOpenCommandValue(exe));
    DOCTEST_REQUIRE(readDefaultSz(L"Software\\Classes\\.mdtest", &v));
    DOCTEST_CHECK_EQ(v, std::string("DevDeskTest.md"));

    // 三态回查：写入后 isDefaultForMd = Ours
    const core::Result<AssocState> st = isDefaultForMd(cfg);
    DOCTEST_REQUIRE(st.isOk());
    DOCTEST_CHECK_EQ(st.value(), AssocState::Ours);

    // 幂等双写：再写一次——spy 再触发、键值仍是同一目标值（无累积无变体）
    const core::Result<bool> second = setAsDefaultForMd(cfg);
    DOCTEST_REQUIRE(second.isOk());
    DOCTEST_CHECK(second.value());
    DOCTEST_CHECK_GE(g_spyCalls, 2);
    DOCTEST_REQUIRE(readDefaultSz(
        L"Software\\Classes\\DevDeskTest.md\\shell\\open\\command", &v));
    DOCTEST_CHECK_EQ(v, buildOpenCommandValue(exe));
    DOCTEST_REQUIRE(readDefaultSz(L"Software\\Classes\\.mdtest", &v));
    DOCTEST_CHECK_EQ(v, std::string("DevDeskTest.md"));

    // 清理：ProgId 树 + 扩展名默认值全消失
    DOCTEST_REQUIRE(removeRegistrationForTest(cfg).isOk());
    DOCTEST_CHECK_FALSE(readDefaultSz(
        L"Software\\Classes\\DevDeskTest.md\\shell\\open\\command", &v));
    DOCTEST_CHECK_FALSE(readDefaultSz(L"Software\\Classes\\.mdtest", &v));
    // 清理后不再 ours（查询失败=Unknown 或无归属，绝不能残留 Ours）
    const core::Result<AssocState> after = isDefaultForMd(cfg);
    DOCTEST_CHECK(after.isOk());
    DOCTEST_CHECK_NE(after.value(), AssocState::Ours);

    setShellNotifySink(oldSink);
    return true;
})
#endif // _WIN32
