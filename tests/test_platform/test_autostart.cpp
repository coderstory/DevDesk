// tests/test_platform/test_autostart.cpp
// platform::autostart 覆盖（UAT6 设置-通用「开机自启动」）：
//   1. 纯 builder：Run 值「"<exe>" --minimized」格式钉死（引号 + 旗标后缀）；
//      plist 最小字段集（Label / ProgramArguments[exe, --minimized] / RunAtLoad）
//      + XML 转义（路径含 & < > 不毁文档）。
//   2. --minimized 旗标解析（纯函数，生产捕获与单测共用同一实现）：
//      命中 / 未命中 / 精确匹配（--minimized=1 不认）/ argv[0]=exe 跳过。
//   3. 控制器流（注入缝，构造注入假 ops）：注册内容含真 exe 路径与 --minimized、
//      注销走 removeEntry、写失败如实上抛且注册态不翻转、回读 = 注入态。
//      headless 零系统写入——真注册表/LaunchAgents 只在 isRegistered 真值回读里
//      被只读触碰（设置页打开时的同一路径）。
// 防的事故：自启动项写错格式（exe 不带引号被空格拆参 / 旗标拼写漂移）、
// 注销写成空值残留死项、开关显示与注册态漂移（回读口径）。
#include "doctest.h"
#include "platform/autostart.h"
#include "core/result.h"

#include <memory>
#include <string>
#include <vector>

namespace {

using platform::autostart::AutostartController;
using platform::autostart::AutostartOps;

// 假 writer：捕获写入内容与调用次数（断言「项值含 exe 路径与 --minimized」「注销删项」）
struct FakeOps final : public AutostartOps {
    bool registered = false;
    bool failWrites = false;
    int writeCalls = 0;
    int removeCalls = 0;
    std::string lastContent;

    bool isRegistered() override { return registered; }
    core::Result<void> writeEntry(const std::string& content) override {
        ++writeCalls;
        lastContent = content;
        if (failWrites) {
            return core::Result<void>::fail(core::ErrorCode::Internal,
                                            "注入的写入失败");
        }
        registered = true;
        return core::Result<void>::ok();
    }
    core::Result<void> removeEntry() override {
        ++removeCalls;
        registered = false;
        return core::Result<void>::ok();
    }
};

// 平台取材断言（断言宏体禁嵌 #if，-Werror：平台条件收在 helper，先例
// test_tray_icon.cpp expectedExtension）：Windows Run 值首字符是引号（exe 加
// 引号防拆参）；macOS 是 plist 文档；posix 恒空串（无注册路径）。
bool entryLooksRight(const std::string& content) {
#if defined(_WIN32)
    return !content.empty() && content.front() == '"';
#elif defined(__APPLE__)
    return content.find("<plist") != std::string::npos;
#else
    return content.empty();
#endif
}

// 能力查询的平台预期：win/mac 支持，posix 兜底 false（同上，#if 收在 helper）
bool expectedSupported() {
#if defined(_WIN32) || defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

} // namespace

REGISTER_TEST(autostart_builders_pure,
              "autostart: Run 值/plist 内容纯函数（--minimized 契约 + XML 转义）", {
    using namespace platform::autostart;

    // Run 值：引号包 exe + 空格分隔旗标（路径带空格不能被拆参）
    DOCTEST_CHECK_EQ(buildCommandValue("C:\\Apps\\DevDesk.exe"),
                     std::string("\"C:\\Apps\\DevDesk.exe\" --minimized"));
    DOCTEST_CHECK_EQ(buildCommandValue(""),
                     std::string("\"\" --minimized"));  // 空 exe 仍保旗标位（写入方前置拦截）

    // plist：Label / ProgramArguments[exe, --minimized] / RunAtLoad 最小字段集
    const std::string exe = "/Applications/DevDesk.app/Contents/MacOS/DevDesk";
    const std::string plist = buildPlistContent(exe);
    DOCTEST_CHECK(plist.find("<key>Label</key>") != std::string::npos);
    DOCTEST_CHECK(plist.find("<string>com.coderstory.devdesk</string>") !=
                  std::string::npos);
    DOCTEST_CHECK(plist.find("<key>ProgramArguments</key>") != std::string::npos);
    DOCTEST_CHECK(plist.find("<string>" + exe + "</string>") != std::string::npos);
    DOCTEST_CHECK(plist.find("<string>--minimized</string>") != std::string::npos);
    DOCTEST_CHECK(plist.find("<key>RunAtLoad</key>") != std::string::npos);
    DOCTEST_CHECK(plist.find("<true/>") != std::string::npos);

    // XML 转义：路径含 & < > 不毁文档（plist 是 XML，裸写即坏）
    const std::string escaped = buildPlistContent("/tmp/a&b<c>/DevDesk");
    DOCTEST_CHECK(escaped.find("/tmp/a&amp;b&lt;c&gt;/DevDesk") !=
                  std::string::npos);
    DOCTEST_CHECK(escaped.find("a&b<c>") == std::string::npos);  // 不许裸出现
    return true;
})

REGISTER_TEST(autostart_parse_minimized_flag,
              "autostart: --minimized 旗标解析纯函数（命中/未命中/精确匹配/argv[0] 跳过）", {
    using platform::autostart::parseMinimizedFlag;
    // 命中：旗标独立成参（开机注册项写的就是这个形态）
    DOCTEST_CHECK(parseMinimizedFlag({"devdesk.exe", "--minimized"}));
    // 旗标 + 文件参数共存（开机自启动拉起后又被双击打开文件）：两者各自成立
    DOCTEST_CHECK(parseMinimizedFlag({"devdesk.exe", "--minimized", "note.md"}));
    DOCTEST_CHECK(parseMinimizedFlag({"devdesk.exe", "note.md", "--minimized"}));
    // 未命中：无旗标 / 同族前缀不认（精确匹配，防 --minimized=1 误判）
    DOCTEST_CHECK_FALSE(parseMinimizedFlag({"devdesk.exe"}));
    DOCTEST_CHECK_FALSE(parseMinimizedFlag({"devdesk.exe", "--minimized=1"}));
    DOCTEST_CHECK_FALSE(parseMinimizedFlag({"devdesk.exe", "-m", "--min"}));
    // argv[0]=exe 跳过：可执行自己叫 --minimized 不算命中
    DOCTEST_CHECK_FALSE(parseMinimizedFlag({"--minimized"}));
    DOCTEST_CHECK_FALSE(parseMinimizedFlag({}));
    return true;
})

REGISTER_TEST(autostart_controller_injection,
              "autostart: 控制器注入缝——注册内容/注销删项/失败上抛/回读（headless 不写系统）", {
    using namespace platform::autostart;

    if (supported()) {
        // 注册：写一次、内容含真 exe 路径与 --minimized（取材 win=Run 值/macos=plist）
        {
            auto owned = std::make_unique<FakeOps>();
            FakeOps* fake = owned.get();
            AutostartController ctl(std::move(owned));   // 构造注入
            DOCTEST_REQUIRE(ctl.setRegistered(true).isOk());
            DOCTEST_CHECK_EQ(fake->writeCalls, 1);
            DOCTEST_CHECK_EQ(fake->removeCalls, 0);
            DOCTEST_CHECK(fake->lastContent.find("--minimized") !=
                          std::string::npos);
            DOCTEST_CHECK(!executablePath().empty());  // 真机路径可解析（注册项写它）
            DOCTEST_CHECK(fake->lastContent.find(executablePath()) !=
                          std::string::npos);
            DOCTEST_CHECK(entryLooksRight(fake->lastContent));
            DOCTEST_CHECK(ctl.isRegistered());  // 回读 = 注入态
        }
        // 写失败如实上抛：注册态不翻转（开关按真值回退，不停留在错态）
        {
            auto owned = std::make_unique<FakeOps>();
            FakeOps* fake = owned.get();
            fake->failWrites = true;
            AutostartController ctl(std::move(owned));
            DOCTEST_REQUIRE(ctl.setRegistered(true).isFail());
            DOCTEST_CHECK_EQ(fake->writeCalls, 1);
            DOCTEST_CHECK_FALSE(ctl.isRegistered());
        }
    } else {
        // POSIX 兜底：exe 路径解析为空 → 注册诚实失败且不触 ops（能力查询恒 false）
        {
            auto owned = std::make_unique<FakeOps>();
            FakeOps* fake = owned.get();
            AutostartController ctl(std::move(owned));
            DOCTEST_REQUIRE(ctl.setRegistered(true).isFail());
            DOCTEST_CHECK_EQ(fake->writeCalls, 0);
        }
    }
    // 注销：走 removeEntry（删项），不产生新的写入（平台无关）
    {
        auto owned = std::make_unique<FakeOps>();
        FakeOps* fake = owned.get();
        fake->registered = true;
        AutostartController ctl(std::move(owned));
        DOCTEST_CHECK(ctl.isRegistered());
        DOCTEST_REQUIRE(ctl.setRegistered(false).isOk());
        DOCTEST_CHECK_EQ(fake->removeCalls, 1);
        DOCTEST_CHECK_EQ(fake->writeCalls, 0);
        DOCTEST_CHECK_FALSE(ctl.isRegistered());
    }
    // 空 ops：诚实失败（构造注入传空的护栏）
    {
        AutostartController ctl(nullptr);
        DOCTEST_CHECK_FALSE(ctl.isRegistered());
        DOCTEST_REQUIRE(ctl.setRegistered(true).isFail());
        DOCTEST_REQUIRE(ctl.setRegistered(false).isFail());
    }
    // 系统 ops 可装配（真值回读路径的装配护栏；只读，不写）
    DOCTEST_CHECK(systemOps() != nullptr);
    return true;
})

REGISTER_TEST(autostart_supported_platform_expectation,
              "autostart: 能力查询 + 真值只读回读不抛（设置页打开时的同一路径）", {
    using namespace platform::autostart;
    // 平台预期：win/mac 支持，posix 兜底 false
    DOCTEST_CHECK_EQ(supported(), expectedSupported());
    // 真值回读走系统（注册表有项 = 开），只读不写、不抛异常
    const bool registered = AutostartController(systemOps()).isRegistered();
    (void)registered;   // 本机注册态无先验值，只验路径不炸
    return true;
})
