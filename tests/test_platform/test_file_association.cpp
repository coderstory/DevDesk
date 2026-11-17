// tests/test_platform/test_file_association.cpp
// platform::file_association 覆盖（F-18 记事本多扩展关联，方案卡 §十；接管
// F-06 单 .md 版）：
//   1. 三态判定/多扩展聚合纯函数 + 注入缝配置/登记内容契约钉死（漂移即红）。
//   2. _WIN32 注入缝实登记链路：testConfig()（DevDeskTest.notepad +
//      .mdtest/.notepadtest）写 → HKCU 键值直读断言（不经被测查询通路）→
//      聚合三态回查 → 幂等双写 → SHChangeNotify spy 计数 → 清理 → 清理后不再
//      ours。
//   3. _WIN32 unregisterAll 幂等 + 归己才清（不误删他者默认值）。
// 测试红线：用例只用注入缝配置（DevDeskTest.notepad/.mdtest/.notepadtest），
// 禁触开发机真实关联登记；QueryCurrentDefault 真判据 = 实机 UAT 项，单测只验
// 注入缝配置下的回读。回退会红：删掉写入逻辑（writeRegistration）则直读断言/
// 回查 ours/幂等断言全红；删掉 unregisterAll 归己判定则他者保留断言红。
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
              "fileassoc: 三态/聚合纯函数 + 注入缝配置与登记内容契约钉死", {
    using namespace platform::file_association;
    // 三态：查询失败=Unknown（≠Others）、回读==本应用=Ours、回读他者=Others
    DOCTEST_CHECK_EQ(classifyState(false, "Whatever", productionConfig().progId),
                     AssocState::Unknown);
    DOCTEST_CHECK_EQ(classifyState(true, "DevDesk.notepad", "DevDesk.notepad"),
                     AssocState::Ours);
    DOCTEST_CHECK_EQ(classifyState(true, "OtherApp.md", "DevDesk.notepad"),
                     AssocState::Others);
    // 多扩展聚合：全 ours / 含 unknown / 含 others / 空表（逐档语义见 .h）
    DOCTEST_CHECK_EQ(aggregateAssocStates({AssocState::Ours, AssocState::Ours}),
                     AssocState::Ours);
    DOCTEST_CHECK_EQ(aggregateAssocStates({AssocState::Ours,
                                           AssocState::Unknown}),
                     AssocState::Unknown);
    DOCTEST_CHECK_EQ(aggregateAssocStates({AssocState::Ours,
                                           AssocState::Others}),
                     AssocState::Others);
    DOCTEST_CHECK_EQ(aggregateAssocStates({}), AssocState::Unknown);
    // 注入缝钉死：生产/测试配置漂移即红（防用例误触真实关联登记）
    DOCTEST_CHECK_EQ(productionConfig().progId, std::string("DevDesk.notepad"));
    DOCTEST_CHECK_EQ(productionConfig().extensions.size(),
                     core::notepad::notepadExtensions().size());
    DOCTEST_CHECK_EQ(testConfig().progId, std::string("DevDeskTest.notepad"));
    DOCTEST_REQUIRE_EQ(testConfig().extensions.size(), size_t{2});
    DOCTEST_CHECK_EQ(testConfig().extensions[0], std::string(".mdtest"));
    DOCTEST_CHECK_EQ(testConfig().extensions[1], std::string(".notepadtest"));
    // 登记内容契约：exe 与 %1 各自带引号（路径/文件名含空格不拆参）；图标首枚
    DOCTEST_CHECK_EQ(buildOpenCommandValue("C:\\Apps\\Dev Desk.exe"),
                     std::string("\"C:\\Apps\\Dev Desk.exe\" \"%1\""));
    DOCTEST_CHECK_EQ(buildDefaultIconValue("C:\\Apps\\DevDesk.exe"),
                     std::string("\"C:\\Apps\\DevDesk.exe\",0"));
    DOCTEST_CHECK_EQ(std::string(kProgIdDescription),
                     std::string("DevDesk Notepad Document"));
    return true;
})

#if defined(_WIN32)
REGISTER_TEST(file_association_registry_roundtrip,
              "fileassoc: 注入缝实登记链路（写→HKCU直读→聚合回查→幂等双写→spy→清理）", {
    using namespace platform::file_association;
    const AssocConfig cfg = testConfig();  // DevDeskTest.notepad（禁 productionConfig）

    // 前置清场（上次用例残留也吞掉）：清理缝幂等
    DOCTEST_REQUIRE(removeRegistrationForTest(cfg).isOk());

    // 装 spy（保存旧值，用例退出前恢复，防跨用例泄漏）
    ShellNotifySink* const oldSink = setShellNotifySink(&spyNotify);
    g_spyCalls = 0;

    // 写入：ok 且 bool=true（写后聚合回查归本——无 UserChoice 回落类默认值路径，
    // 同机 F-07 POC 证该回读通路生效）
    const core::Result<bool> first = setAsDefaultFor(cfg);
    DOCTEST_REQUIRE(first.isOk());
    DOCTEST_CHECK(first.value());
    DOCTEST_CHECK(g_spyCalls >= 1);  // 写完必调 SHChangeNotify（官方 must-call）

    // HKCU 键值直读断言（不经被测查询通路）：ProgId 三件套一次 + 逐扩展指向
    const std::string exe = platform::autostart::executablePath();
    DOCTEST_REQUIRE(!exe.empty());
    std::string v;
    DOCTEST_REQUIRE(readDefaultSz(L"Software\\Classes\\DevDeskTest.notepad", &v));
    DOCTEST_CHECK_EQ(v, std::string(kProgIdDescription));
    DOCTEST_REQUIRE(readDefaultSz(L"Software\\Classes\\DevDeskTest.notepad\\DefaultIcon", &v));
    DOCTEST_CHECK_EQ(v, buildDefaultIconValue(exe));
    DOCTEST_REQUIRE(readDefaultSz(
        L"Software\\Classes\\DevDeskTest.notepad\\shell\\open\\command", &v));
    DOCTEST_CHECK_EQ(v, buildOpenCommandValue(exe));
    DOCTEST_REQUIRE(readDefaultSz(L"Software\\Classes\\.mdtest", &v));
    DOCTEST_CHECK_EQ(v, std::string("DevDeskTest.notepad"));
    DOCTEST_REQUIRE(readDefaultSz(L"Software\\Classes\\.notepadtest", &v));
    DOCTEST_CHECK_EQ(v, std::string("DevDeskTest.notepad"));

    // 聚合回查：写入后 overallState = Ours；逐扩展查询也各归本
    const core::Result<AssocState> st = overallState(cfg);
    DOCTEST_REQUIRE(st.isOk());
    DOCTEST_CHECK_EQ(st.value(), AssocState::Ours);
    const core::Result<AssocState> perExt = isDefaultFor(".mdtest", cfg);
    DOCTEST_REQUIRE(perExt.isOk());
    DOCTEST_CHECK_EQ(perExt.value(), AssocState::Ours);

    // 幂等双写：再写一次——spy 再触发、键值仍是同一目标值（无累积无变体）
    const core::Result<bool> second = setAsDefaultFor(cfg);
    DOCTEST_REQUIRE(second.isOk());
    DOCTEST_CHECK(second.value());
    DOCTEST_CHECK(g_spyCalls >= 2);
    DOCTEST_REQUIRE(readDefaultSz(
        L"Software\\Classes\\DevDeskTest.notepad\\shell\\open\\command", &v));
    DOCTEST_CHECK_EQ(v, buildOpenCommandValue(exe));
    DOCTEST_REQUIRE(readDefaultSz(L"Software\\Classes\\.mdtest", &v));
    DOCTEST_CHECK_EQ(v, std::string("DevDeskTest.notepad"));

    // 清理：ProgId 树 + 逐扩展默认值全消失
    DOCTEST_REQUIRE(removeRegistrationForTest(cfg).isOk());
    DOCTEST_CHECK_FALSE(readDefaultSz(
        L"Software\\Classes\\DevDeskTest.notepad\\shell\\open\\command", &v));
    DOCTEST_CHECK_FALSE(readDefaultSz(L"Software\\Classes\\.mdtest", &v));
    DOCTEST_CHECK_FALSE(readDefaultSz(L"Software\\Classes\\.notepadtest", &v));
    // 清理后不再 ours（查询失败=Unknown 或无归属，绝不能残留 Ours）
    const core::Result<AssocState> after = overallState(cfg);
    DOCTEST_CHECK(after.isOk());
    DOCTEST_CHECK_NE(after.value(), AssocState::Ours);

    setShellNotifySink(oldSink);
    return true;
})

REGISTER_TEST(file_association_unregister_all,
              "fileassoc: unregisterAll 幂等 + 仅归己才清（他者默认值保留）", {
    using namespace platform::file_association;
    const AssocConfig cfg = testConfig();
    DOCTEST_REQUIRE(removeRegistrationForTest(cfg).isOk());  // 清场

    ShellNotifySink* const oldSink = setShellNotifySink(&spyNotify);
    g_spyCalls = 0;

    // 未登记态注销 = 幂等 ok（不炸不误报）
    DOCTEST_REQUIRE(unregisterAll(cfg).isOk());
    DOCTEST_CHECK_EQ(g_spyCalls, 0);  // 无清除不广播

    // 登记 → 逐扩展默认值在位
    DOCTEST_REQUIRE(setAsDefaultFor(cfg).isOk());
    std::string v;
    DOCTEST_REQUIRE(readDefaultSz(L"Software\\Classes\\.mdtest", &v));
    DOCTEST_CHECK_EQ(v, std::string("DevDeskTest.notepad"));

    // 他者默认值（模拟用户后选他者）：.notepadtest 手写 Foreign ProgId 默认值
    HKEY k = nullptr;
    DOCTEST_REQUIRE_EQ(RegCreateKeyExW(HKEY_CURRENT_USER,
                                       L"Software\\Classes\\.notepadtest", 0,
                                       nullptr, REG_OPTION_NON_VOLATILE,
                                       KEY_WRITE, nullptr, &k, nullptr),
                       static_cast<LSTATUS>(ERROR_SUCCESS));
    const std::wstring foreign = L"Foreign.notepad";
    DOCTEST_REQUIRE_EQ(
        RegSetValueExW(k, nullptr, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(foreign.c_str()),
                       static_cast<DWORD>((foreign.size() + 1) * sizeof(wchar_t))),
        static_cast<LSTATUS>(ERROR_SUCCESS));
    RegCloseKey(k);

    // 注销：归己的 .mdtest 清掉；他者的 .notepadtest 默认值保留；广播恰一次
    g_spyCalls = 0;
    DOCTEST_REQUIRE(unregisterAll(cfg).isOk());
    DOCTEST_CHECK_FALSE(readDefaultSz(L"Software\\Classes\\.mdtest", &v));
    DOCTEST_REQUIRE(readDefaultSz(L"Software\\Classes\\.notepadtest", &v));
    DOCTEST_CHECK_EQ(v, std::string("Foreign.notepad"));
    DOCTEST_CHECK_EQ(g_spyCalls, 1);  // 有清除才广播

    // 幂等：再注销一次 = ok（他者仍在，无新增广播）
    g_spyCalls = 0;
    DOCTEST_REQUIRE(unregisterAll(cfg).isOk());
    DOCTEST_CHECK_EQ(g_spyCalls, 0);
    DOCTEST_REQUIRE(readDefaultSz(L"Software\\Classes\\.notepadtest", &v));
    DOCTEST_CHECK_EQ(v, std::string("Foreign.notepad"));

    // 终清（只清本用例造的他者物）
    DOCTEST_CHECK_EQ(RegDeleteKeyW(HKEY_CURRENT_USER,
                                   L"Software\\Classes\\.notepadtest"),
                     static_cast<LSTATUS>(ERROR_SUCCESS));
    setShellNotifySink(oldSink);
    return true;
})

REGISTER_TEST(file_association_cleanup_tolerates_foreign_children,
              "fileassoc: 清理缝容忍残留他者子键（默认值清、他者内容留、返回 ok）", {
    using namespace platform::file_association;
    const AssocConfig cfg = testConfig();
    DOCTEST_REQUIRE(removeRegistrationForTest(cfg).isOk());  // 清场

    // 造残留态：.mdtest 有默认值指向 + 他者子键（模拟 shell 添子键后键非空）
    HKEY k = nullptr;
    DOCTEST_REQUIRE_EQ(RegCreateKeyExW(HKEY_CURRENT_USER,
                                       L"Software\\Classes\\.mdtest", 0, nullptr,
                                       REG_OPTION_NON_VOLATILE, KEY_WRITE,
                                       nullptr, &k, nullptr),
                       static_cast<LSTATUS>(ERROR_SUCCESS));
    const std::wstring ours = L"DevDeskTest.notepad";
    DOCTEST_REQUIRE_EQ(
        RegSetValueExW(k, nullptr, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(ours.c_str()),
                       static_cast<DWORD>((ours.size() + 1) * sizeof(wchar_t))),
        static_cast<LSTATUS>(ERROR_SUCCESS));
    RegCloseKey(k);
    k = nullptr;
    DOCTEST_REQUIRE_EQ(RegCreateKeyExW(HKEY_CURRENT_USER,
                                       L"Software\\Classes\\.mdtest\\ForeignProgId",
                                       0, nullptr, REG_OPTION_NON_VOLATILE,
                                       KEY_WRITE, nullptr, &k, nullptr),
                       static_cast<LSTATUS>(ERROR_SUCCESS));
    // 道具键写实值（空键读默认值 = FILE_NOT_FOUND，「保留」断言判据失真）
    const std::wstring foreign = L"Foreign content";
    DOCTEST_REQUIRE_EQ(
        RegSetValueExW(k, nullptr, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(foreign.c_str()),
                       static_cast<DWORD>((foreign.size() + 1) * sizeof(wchar_t))),
        static_cast<LSTATUS>(ERROR_SUCCESS));
    RegCloseKey(k);

    // 清理：返回 ok（HAS_CHILDREN 容忍）；我们的默认值清掉，他者子键原样保留
    DOCTEST_REQUIRE(removeRegistrationForTest(cfg).isOk());
    std::string v;
    DOCTEST_CHECK_FALSE(readDefaultSz(L"Software\\Classes\\.mdtest", &v));
    wchar_t buf[256] = {};
    DWORD sz = sizeof(buf);
    DOCTEST_CHECK_EQ(RegGetValueW(HKEY_CURRENT_USER,
                                  L"Software\\Classes\\.mdtest\\ForeignProgId",
                                  nullptr, RRF_RT_REG_SZ, nullptr, buf, &sz),
                     static_cast<LSTATUS>(ERROR_SUCCESS));

    // 终清（只清本用例造的他者物）：删子键→键空→整键删
    DOCTEST_REQUIRE_EQ(RegDeleteKeyW(HKEY_CURRENT_USER,
                                     L"Software\\Classes\\.mdtest\\ForeignProgId"),
                       static_cast<LSTATUS>(ERROR_SUCCESS));
    DOCTEST_CHECK_EQ(RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\.mdtest"),
                     static_cast<LSTATUS>(ERROR_SUCCESS));
    return true;
})
#endif // _WIN32
