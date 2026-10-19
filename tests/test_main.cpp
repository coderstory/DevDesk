// tests/test_main.cpp
// 唯一定义 main() 的文件
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "app_assembly.h"
#include "app_sidebar.h"
#include "claude_service.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/proxy_stats.h"
#include "core/router.h"
#include "core/service_locator.h"
#include "core/theme.h"
#include "module_toggle_bridge.h"
#include "platform/notification.h"
#include "quota_home_bridge.h"
#include "ui_kit/splash.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

// 环境变量通道：Windows 传空串即删除变量；POSIX 空串走 unsetenv
void setEnvVar(const char* key, const std::string& value) {
#if defined(_WIN32)
    ::_putenv_s(key, value.c_str());
#else
    if (value.empty()) {
        ::unsetenv(key);
    } else {
        ::setenv(key, value.c_str(), 1);
    }
#endif
}

// per-case 全局态重置（测试隔离）
// 运行器在每个用例体**之前**调用 resetCaseGlobals：把进程级全局态拉回出厂默认。
// 用例体内的显式 setLight / setFontScale 等在重置之后执行，不受影响。
//
// 为什么必须有它：用例体内 fail-fast（DOCTEST_CHECK/REQUIRE 直接 return false）
// 会跳过尾部的恢复代码，core::theme 全局值与模块视图状态随即跨用例、跨进程泄漏
// （markdown/todo 的视图状态持久化在 markdownViewStateFile() /
// todoViewStateFile() 指向的文件里；未设 DEVDESK_*_VIEW_STATE_PATH 的用例直接读写真实
// %APPDATA% 文件，三态模式在多次运行间轮换，「onStart 后默认 Edit 态」的断言
// 约 1/3 概率翻车）。
//
// 手工登记清单
// **新增进程级全局必须在此登记**——本函数是手工清单，漏登记的全局会跨用例泄漏
// 且无任何告警（一个失败用例把日志级别留在 Trace、或留下悬垂单例，后续用例全跑在
// 污染态里）。逐项写明清的是什么、来自哪：
//   1. core::theme          全局主题/字号/缩放          ← core/theme.h
//   2. core::appDatabase    共享 DB 连接                ← core/database.h
//   3. core::appRouter      进程级 Router               ← core/router.h
//   4. ClaudeService 单例                                ← module_claude_proxy/claude_service.h
//   5. core::proxy_stats    流式统计 + 注入的假时钟      ← core/proxy_stats.h
//   6. core::log 级别       日志级别（回到命令行基线）      ← core/logging.h
//   7. 路径环境变量          DEVDESK_* 系列              ← 本文件（setEnvVar）
//   8. ServiceLocator 槽位   全部接口类型（清空=未注册）      ← core/service_locator.h
//   9. argv 捕获（装配层）    待打开文件路径 + 已捕获门        ← app_assembly.h
//  10. 应用级通知（侧边栏）    ui_kit::ToastState 单例        ← app_sidebar.h
//  11. 侧边栏折叠态            app 进程内单值（展开回厂）      ← app_sidebar.h
//  12. 退出接线已挂门          armExitHook 幂等记账位回厂      ← app_assembly.h
//  13. 启动 splash 测试缝      无头进程禁 splash（2.3s 时序不适用） ← ui_kit/splash.h
//  14. 模块开关桥 handler      两条桥的注入槽（模块开关 + claude 惰性装配）← module_toggle_bridge.h
//  15. 余量功能总开关桥        devdesk::quota_bridge 的函数局部 static flag ← quota_home_bridge.h
//  16. 系统通知后端注入槽      platform::notify 的进程级 Backend 槽 ← platform/notification.h
//
// 同口径纪律：**进程级单例一律永不析构**——服务/注册表/统计状态这类「与进程同寿」
// 的对象不得写成「函数局部 static」。首次构造晚于 armExitHook 时，atexit 的 LIFO
// 会让析构先于 exit hook 跑，退出期读它就是 UAF（见 main.cpp getClaudeService /
// core/tool.cpp instance / core/proxy_stats.cpp state）。新增单例请照堆持有惯用法
// 写，并在下方清账。
//
// 日志级别的「出厂态」= 命令行选定的基线：--log → Debug，否则 Info。重置必须回到
// 这个基线而不是硬编码 Info——硬编码会把 --log 的 Debug 在第一个用例前冲掉，
// 命令行开关（文档承诺「打开 Debug 级控制台日志」）静默失效。
core::log::Level g_logLevelBaseline = core::log::Level::Info;
// 每用例的测试数据根（resetCaseGlobals 建、用例体跑完后主循环删）；空 = 未建或已清。
std::filesystem::path g_testDataRoot;

void resetCaseGlobals() {
    // 1) core::theme：默认深色 / 编辑器字号 15 / 缩放 1.0（与 theme.h 契约一致）。
    // 先摘 change handler 再切主题：泄漏的 handler 可能捕获已析构的用例栈变量
    // （test_theme 用 [&] 捕获），后切会让它摸到野指针。顺序不可换。
    core::theme::setChangeHandler(nullptr);
    core::theme::setDark();
    core::theme::setEditorFontSize(15.0f);
    core::theme::setFontScale(1.0f);
    // 2) 共享 DB 连接（core/database.h）：用例提前返回时手工 register 的可能悬垂
    // （DbEnv RAII 的已自注销；这里防非 RAII 写法）。需要 DB 的用例都在体内自行注册。
    ::core::registerAppDatabase(nullptr);
    // 3) 进程级 Router（core/router.h 的 registerAppRouter/appRouter）：用例把自己的
    // 栈上 Router 注册进去后提前返回，会让 appRouter() 指向已析构对象；无 Router
    // 假设的用例（home 降级渲染等）依赖此处为 null。
    ::core::registerAppRouter(nullptr);
    // 4) ClaudeService 单例（claude_service.h 的 setGlobal/global）：与 3 同理，模块
    // onStart 等都经 global() 取单例。出厂态 = 未注册（null）。
    ::module::claude::ClaudeService::setGlobal(nullptr);
    // 5) proxy_stats（core/proxy_stats.h）：跨模块共享的流式 token 统计，含
    // setTestClock 注入的假时钟——不重置会把上一个用例的时钟/累计值带进来，速率类
    // 断言随之失真。resetForTest 同时清状态与时钟。
    ::core::proxy_stats::resetForTest();
    // 6) 日志级别（core/logging.h）：用例体内的 setLevel(Trace/Debug) 会影响后续全部
    // 用例的输出量与断言可见性。必须回到命令行基线（默认 Info；--log 时为 Debug），
    // 不能硬编码 Info，否则 --log 会在第一个用例前被冲掉。
    // 控制台输出开关只有 addConsoleOutput、没有关闭 API：--log 打开后本进程内不可撤销
    // （只影响是否往控制台打印，不影响用例隔离）。
    ::core::log::setLevel(g_logLevelBaseline);
    // 7) 路径环境变量：一律拉回「未设置」出厂态。
    // DEVDESK_DATA_ROOT 定向到构建树内临时目录：Windows 安全策略（受控文件夹访问等）
    // 会封掉进程对 %USERPROFILE% 等树外路径的写操作，树内写则不受限。统一走树内根
    // 既配好隔离也绕过平台安全限制。
    // Windows _putenv_s 传空串是「置空值」不是说「删除变量」——空串写入环境块但
    // getenv 读到空串不会走 appDataDir 的 env 分支（dataRootOverride 判 *env != '\0'），
    // 最终回退到 platformDefaultAppDataDir() = USERPROFILE/DevDesk —— 这正好是我们
    // 要避开的。所以必须传一个非空路径。构建树内 temp 目录来承载测试专用数据根。
    {
        const auto testDataRoot = test_temp::tempPath("test_data");
        std::error_code ec;
        std::filesystem::create_directories(testDataRoot, ec);
        setEnvVar("DEVDESK_DATA_ROOT", testDataRoot.string());
        g_testDataRoot = testDataRoot;   // 记下供主循环末尾清理（见 main()）
    }
    setEnvVar("DEVDESK_NOTES_DIR", "");
    setEnvVar("DEVDESK_PETS_DIR", "");
    setEnvVar("DEVDESK_CLAUDE_CONFIG_DIR", "");
    // 8) ServiceLocator 槽位（core/service_locator.h）：用例体内 set<T>(&栈上对象) 后
    // 提前返回会把悬垂指针留在槽里，后续用例经 requireService<T>() 取到已析构对象
    // （生产 main.cpp 注册的是常驻适配器）。resetForTest 类型无关地全清——槽位按类型
    // 实例化，测试本地桩类型（如 test_app_assembly 的 IProbeService）本文件无法点名。
    ::core::ServiceLocator::resetForTest();
    // 9) 装配层 argv 捕获（app_assembly.h）：pendingOpenFilePath 的静态串与「已捕获」
    // 幂等门都是进程级。不复位则后续用例的 captureOpenFileFromArgv 静默 no-op（门已
    // 置位）+ 残留上一用例的路径。出厂态 = 未捕获 + 空路径。
    ::app::assembly::resetArgvCaptureForTest();
    ::app::assembly::pendingOpenFilePath().clear();
    // 10) 应用级通知（app_sidebar.h，收口成 ui_kit::ToastState 单例）：托盘失效入口的
    // 反馈通道，tray 用例在入口就断言「不可见」——上一用例 fail-fast 跳过
    // clearAppNotice 会让断言连锁翻车。出厂态 = 无通知（open=false、内容空）。
    ::app::clearAppNotice();
    // 11) 侧边栏折叠态（app_sidebar.h）：进程内单值。sidebar 用例
    // setSidebarCollapsed(true) 翻折叠态后 fail-fast 提前返回时，残留折叠态让后续拿
    // sidebarWidth()/drawSidebar 做布局断言（展开宽 208）的用例翻车。走独立复位接口，
    // 不经生产写路径 setSidebarCollapsed（persist=true 会带出落库副作用）。
    ::app::resetSidebarGlobals();
    // 12) 退出接线已挂门（app_assembly.h）：armExitHook 的幂等记账位是进程级的——
    // 上一用例挂过后，本用例 exitHookArmed() 断言「装配即挂上」或首次 armExitHook()
    // 幂等返回都会吃到残留已挂态。出厂态 = 未挂（false）。
    ::app::assembly::resetAssemblyGlobals();
    // 13) 启动 splash 测试缝（ui_kit/splash.h）：无头进程跑完不到 splash 的 2.3s
    // 时序，不缝则 compose 期间 drawSplashIfNeeded 恒真、主界面永不绘制。置位后恒返
    // false 且不启动计时/日志/shader；生产 main.cpp 不调此 API，正常 splash 路径零变化。
    ::ui_kit::setSplashDisabledForTests(true);
    // 14) 模块开关桥 handler（module_toggle_bridge.h）：两条桥的注入槽都是进程级函数
    // 指针——用例体内 set 后 fail-fast 提前返回会留桩，后续用例/后续 TU 拿到上一个
    // 用例的 handler。出厂态 = 两条都是 nullptr（默认 no-op + LOG_WARN）。
    ::app::toggle::setToggleHandler(nullptr);
    ::app::toggle::setClaudeAssemblyHandler(nullptr);
    // 15) 余量功能总开关桥（quota_home_bridge.h）：devdesk::quota_bridge 的函数局部
    // static atomic——用例体内 setQuotaFeatureEnabled(true) 后 fail-fast 提前返回不会
    // 还原，后续用例会在「开关开」的污染态里跑。出厂态 = false。
    ::devdesk::quota_bridge::setQuotaFeatureEnabled(false);
    // 16) 系统通知后端注入槽（platform/notification.h）：用例体内
    // setBackendForTest(&栈上 fake) 后 fail-fast 会跳过尾部复位，槽位里的悬垂指针
    // 会让后续用例崩溃；去重/限速/时钟覆盖同样会串味到后续用例。resetForTest
    // 一次清全部（后端槽 + 去重缓存 + 令牌桶 + 时钟 + initialize 幂等标记）。
    ::platform::notify::resetForTest();
}

// 结果先收集、循环结束后统一批量输出：全量 800+ 例时逐条 std::endl 强 flush 是
// ~3min 耗时的主因（每次 flush 一次控制台/管道写 + 上下文切换）。推迟打印顺便
// 拿到 Top-10 慢测试。
struct CaseResult {
    std::string name;
    std::string status;  // "PASS" / "SKIP" / "FAIL"
    long long ms = 0;
    std::string errMsg;
};

} // namespace

// 用法：devdesk_tests.exe [子串] [--log]
//   无参数   → 全量跑
//   子串     → 只跑描述里含该子串的测试（如 "streaming"）
//   --log    → 打开控制台日志 + Debug 级别（排查流式/代理问题时用）
int main(int argc, char** argv) {
    using Clock = std::chrono::steady_clock;
    int passed = 0, failed = 0, skipped = 0, total = 0;
    std::string filter;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--log") {
            core::log::setLevel(core::log::Level::Debug);
            // 必须同时抬基线：否则 resetCaseGlobals 会在每个用例前把级别拉回 Info
            g_logLevelBaseline = core::log::Level::Debug;
            core::log::addConsoleOutput();
        } else if (!arg.empty()) {
            filter = arg;
        }
    }
    std::vector<CaseResult> results;
    results.reserve(1024);
    for (const auto& t : test_registry::registry()) {
        if (!filter.empty() && t.first.find(filter) == std::string::npos) {
            continue;
        }
        ++total;
        bool ok = false;
        std::string errMsg;
        const auto t0 = Clock::now();
        // 单个测试的异常（如 Windows 上 std::filesystem_error）只让本测试失败，
        // 不炸掉整个套件——运行器必须继续跑完剩余用例，否则隔离失败会掩盖后续问题。
        try {
            resetCaseGlobals();
            ok = t.second();
        } catch (const std::exception& e) {
            ok = false;
            errMsg = e.what();
        } catch (...) {
            ok = false;
            errMsg = "unknown exception";
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            Clock::now() - t0)
                            .count();
        // 用例体内 DOCTEST_SKIP(...) 会置跳过标记——单独计 Skipped（不再混进
        // Passed），Skipped 不算失败、不影响退出码。
        const bool skippedCase = test_registry::consumeSkipped();
        // 结果先收集不打印：std::endl 的强 flush 是 800+ 例全量 ~3min 的主瓶颈，
        // 推到循环结束后一次性批量输出。
        if (ok && skippedCase) {
            results.push_back({t.first, "SKIP", ms, ""});
            ++skipped;
        } else if (ok) {
            results.push_back({t.first, "PASS", ms, ""});
            ++passed;
        } else {
            results.push_back({t.first, "FAIL", ms, errMsg});
            ++failed;
        }
        // 清理上一条用例的数据根（resetCaseGlobals 在上一轮建、本轮起点被覆写——旧根已在
        // DEVDESK_DATA_ROOT 外不可达，不在此删即为永久残留）。句柄未放时 remove_all 非零
        // 但不抛，stderr 留一行提示即可（审计 C 项）。
        if (!g_testDataRoot.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(g_testDataRoot, ec);
            if (ec) {
                std::cerr << "[cleanup] 测试数据根残留 " << g_testDataRoot.string()
                          << ": " << ec.message() << std::endl;
            }
            g_testDataRoot.clear();
        }
        // 进度点：每 50 例打一个点到 stderr（不 flush，仅通知还在跑）
        if (total % 50 == 0) {
            std::cerr << ".";
        }
    }
    // 批量输出：\n 不触发 flush，打印完统一 flush 一次
    for (const auto& r : results) {
        std::cout << '[' << r.status << "] " << r.name << " [" << r.ms
                  << "ms]\n";
        if (r.status == "FAIL" && !r.errMsg.empty()) {
            std::cout << "       exception: " << r.errMsg << "\n";
        }
    }
    std::cout << std::flush;
    std::cout << "\n=== Summary ===" << std::endl;
    std::cout << "Total:  " << total << std::endl;
    std::cout << "Passed: " << passed << std::endl;
    std::cout << "Failed: " << failed << std::endl;
    std::cout << "Skipped: " << skipped << std::endl;
    // Top-10 慢测试：方便后续单点优化 I/O 大户
    auto slowest = results;
    std::stable_sort(slowest.begin(), slowest.end(),
                     [](const CaseResult& a, const CaseResult& b) {
                         return a.ms > b.ms;
                     });
    const auto topN = std::min<std::size_t>(slowest.size(), 10);
    if (topN > 0) {
        std::cout << "\nTop-" << topN << " slowest:\n";
        for (std::size_t i = 0; i < topN; ++i) {
            std::cout << "  " << slowest[i].ms << "ms  " << slowest[i].name
                      << '\n';
        }
    }
    // 过滤命中 0 个用例 ≠ 全绿。返回 0 会让上层脚本 grep "Failed: 0" 把「一个用例
    // 都没跑」（如 --no_run 误拼降级成过滤器、过滤词打错）判成 ✅ 全绿——静默绕过
    // 「未运行测试 = 未完成」纪律。
    if (total == 0) {
        std::cout << "[ERROR] 未命中任何用例"
                  << (filter.empty() ? std::string()
                                     : "（过滤词 \"" + filter + "\"）")
                  << "—— 视为失败（退出码 2），防上层脚本误判「全绿」"
                  << std::endl;
        return 2;
    }
    return failed == 0 ? 0 : 1;
}

// 登记清单的复位自证
// 清单的价值全在「复位真的发生」——条目写进去还不够，得证明它真把跨用例的脏态清回
// 出厂态。这里用两件套自证（同一 TU 内用例按声明序执行，① 必先于 ②）：
//   ① 注册定位器槽位 / 写待打开文件 / 弹应用通知，**故意不清理**就返回；
//   ② 断言三者都已回到出厂态——② 内没有任何清理代码，能恢复只可能来自
//      resetCaseGlobals（运行器在每个用例体之前调用）。
// 反向依赖：临时注掉 resetCaseGlobals 里任一条复位，② 立即在对应断言转红。
namespace {

// 本文件私有的桩接口：证明复位是「类型无关」的——它声明在匿名空间，运行器无从按
// 名字逐类型清零（与 test_app_assembly 的 IProbeService 同款形态）。
class ISlotProbe {
public:
    virtual ~ISlotProbe() = default;
    virtual int value() const = 0;
};

class SlotProbe final : public ISlotProbe {
public:
    int value() const override { return 7; }
};

} // namespace

REGISTER_TEST(tin07_probe_leaves_globals_dirty,
              "复位自证①——定位器槽位/待打开文件/应用通知留脏不清理", {
    // 进程级槽位指向的对象必须活过本用例：故意留脏，且只能是 static——栈上对象
    // 在本用例返回时就析构，② 里 requireService 取到的会是已析构对象而非悬垂指针，
    // 证伪力反而更弱。
    static SlotProbe probe;
    ::core::ServiceLocator::set<ISlotProbe>(&probe);
    ::app::assembly::resetArgvCaptureForTest();
    ::app::assembly::captureOpenFileFromArgv({"devdesk_tests.exe", "dirty.md"});
    ::app::showAppNotice("脏态", "留给下一用例");
    // 反向自检：脏态确实留下了——否则 ② 全绿也不能证明复位发生过（可能本来就干净）。
    DOCTEST_CHECK(::core::ServiceLocator::requireService<ISlotProbe>().isOk());
    DOCTEST_CHECK_EQ(::app::assembly::pendingOpenFilePath(), std::string("dirty.md"));
    DOCTEST_CHECK(::app::appNoticeVisible());
    return true;
})

REGISTER_TEST(tin07_reset_clears_globals_between_cases,
              "复位自证②——上一用例的三处脏态被 resetCaseGlobals 清回出厂态", {
    // 本用例体内零清理代码：以下断言全绿 = 复位真的发生
    DOCTEST_CHECK(::core::ServiceLocator::requireService<ISlotProbe>().isFail());
    DOCTEST_CHECK(::app::assembly::pendingOpenFilePath().empty());
    DOCTEST_CHECK_FALSE(::app::appNoticeVisible());
    // 「已捕获」幂等门也复位了——否则本进程内再也捕获不到 argv：空态下捕获应写入。
    ::app::assembly::captureOpenFileFromArgv({"devdesk_tests.exe", "fresh.md"});
    DOCTEST_CHECK_EQ(::app::assembly::pendingOpenFilePath(), std::string("fresh.md"));
    return true;
})
