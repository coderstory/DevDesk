// tests/test_main.cpp
// 唯一定义 main() 的文件
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "claude_service.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/proxy_stats.h"
#include "core/router.h"
#include "core/theme.h"
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

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

// ==================== per-case 全局态重置（测试隔离） ====================
// 全量 ctest 偶发失败（editor_font:83 / theme_hierarchy:207/208）的根因：
// 用例体内 fail-fast（DOCTEST_CHECK/REQUIRE 直接 return false）会跳过尾部
// 恢复代码，core::theme 全局值与模块视图状态跨用例/跨进程泄漏——
// markdown/todo 模块的视图状态（三态模式/选中笔记）持久化在
// markdownViewStateFile()/todoViewStateFile() 指向的文件里，未设
// DEVDESK_*_VIEW_STATE_PATH 的用例直接读写真实 %APPDATA% 文件，三态模式
// 在多次运行间轮换，断言「onStart 后默认 Edit 态」的用例约 1/3 概率翻车。
// 治本：每个用例执行前把进程级全局态拉回出厂默认（clean-room），用例体
// 内的显式 setLight/setFontScale 等不受影响（它们在重置之后执行）。
//
// ============================ 登记清单（TIN-07） ============================
// **新增进程级全局必须在此登记**——本函数是手工清单，漏登记的全局会跨用例泄漏
// （一个失败用例把日志级别留在 Trace、或留下悬垂单例，后续用例全跑在污染态里
// 且无任何告警）。逐项写明清的是什么、来自哪、出厂值是什么：
//   1. core::theme          全局主题/字号/缩放          ← core/theme.h
//   2. core::appDatabase    共享 DB 连接                ← core/database.h
//   3. core::appRouter      进程级 Router               ← core/router.h
//   4. ClaudeService 单例                                ← module_claude_proxy/claude_service.h
//   5. core::proxy_stats    流式统计 + 注入的假时钟      ← core/proxy_stats.h
//   6. core::log 级别       日志级别（回到命令行基线）      ← core/logging.h
//   7. 路径环境变量          DEVDESK_* 系列              ← 本文件（setEnvVar）
// ==========================================================================
// 日志级别的「出厂态」= 命令行选定的基线：--log → Debug，否则 Info。重置必须回到
// 这个基线而不是硬编码 Info——硬编码会把 --log 的 Debug 在第一个用例前冲掉，
// 命令行开关（文档承诺「打开 Debug 级控制台日志」）静默失效。
core::log::Level g_logLevelBaseline = core::log::Level::Info;

void resetCaseGlobals() {
    // 1) core::theme：默认深色 / 编辑器字号 15 / 缩放 1.0（与 theme.h 契约一致）。
    // 先摘 change handler 再切主题：泄漏的 handler 可能捕获已析构的用例栈
    // 变量（test_theme 用 [&] 捕获），后切会让它摸到野指针
    core::theme::setChangeHandler(nullptr);
    core::theme::setDark();
    core::theme::setEditorFontSize(15.0f);
    core::theme::setFontScale(1.0f);
    // 2) 共享 DB 连接（core/database.h）：用例提前返回时手工 register 的可能悬垂
    // （DbEnv RAII 的已自注销；这里防非 RAII 写法），需要 DB 的用例都在体内自行注册
    ::core::registerAppDatabase(nullptr);
    // 3) 进程级 Router（core/router.h 的 registerAppRouter/appRouter）：用例把
    // 自己的栈上 Router 注册进去后提前返回，会让 appRouter() 指向已析构对象；
    // 无 Router 假设的用例（home 降级渲染等）依赖此处为 null
    ::core::registerAppRouter(nullptr);
    // 4) ClaudeService 单例（claude_service.h 的 setGlobal/global）：与 3 同理，
    // 用例体内 setGlobal(&栈上 svc) 后提前返回会留悬垂指针（模块 onStart 等
    // 都经 global() 取单例）；出厂态 = 未注册（null）
    ::module::claude::ClaudeService::setGlobal(nullptr);
    // 5) proxy_stats（core/proxy_stats.h）：跨模块共享的流式 token 统计，含
    // setTestClock 注入的假时钟——不重置会把上一个用例的时钟/累计值带进来，
    // 速率类断言随之失真。resetForTest 同时清状态与时钟
    ::core::proxy_stats::resetForTest();
    // 6) 日志级别（core/logging.h）：用例体内的 setLevel(Trace/Debug) 会一路
    // 影响后续全部用例的输出量与断言可见性，这里拉回命令行基线（默认 Info；
    // --log 时为 Debug——不能硬编码 Info，否则 --log 会在第一个用例前被冲掉）。
    // 注：控制台输出开关只有 addConsoleOutput、没有关闭 API，--log 打开后
    // 本进程内不可撤销（只影响是否往控制台打印，不影响用例隔离）
    ::core::log::setLevel(g_logLevelBaseline);
    // 7) 路径环境变量：一律拉回「未设置」出厂态；两个视图状态例外——重置为
    // 指向 scratch 的新文件并删除，让未显式隔离的用例 onStart 也拿默认态
    setEnvVar("DEVDESK_NOTES_DIR", "");
    setEnvVar("DEVDESK_CONFIG_PATH", "");
    setEnvVar("DEVDESK_PETS_DIR", "");
    setEnvVar("DEVDESK_CLAUDE_CONFIG_DIR", "");
    std::error_code ec;
    // 审计 TIN-02：临时路径带 pid + 进程内自增后缀——并发 ctest（多 worktree
    // 同时跑同一 exe）不会互删 / 互覆写这份视图状态草案文件
    const auto scratch = test_temp::tempPath("devdesk_test_env_scratch");
    const std::string mdVs = (scratch / "md_view_state.json").string();
    const std::string todoVs = (scratch / "todo_view_state.json").string();
    std::filesystem::remove(mdVs, ec);
    std::filesystem::remove(todoVs, ec);
    setEnvVar("DEVDESK_MARKDOWN_VIEW_STATE_PATH", mdVs);
    setEnvVar("DEVDESK_TODO_VIEW_STATE_PATH", todoVs);
}

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
            // 同时抬基线：否则 resetCaseGlobals 会在每个用例前把级别拉回 Info
            g_logLevelBaseline = core::log::Level::Debug;
            core::log::addConsoleOutput();
        } else if (!arg.empty()) {
            filter = arg;
        }
    }
    for (const auto& t : test_registry::registry()) {
        if (!filter.empty() && t.first.find(filter) == std::string::npos) {
            continue;
        }
        ++total;
        bool ok = false;
        std::string errMsg;
        const auto t0 = Clock::now();
        // 单个测试的异常（如 Windows 上 std::filesystem_error）只让本测试
        // 失败，不炸掉整个套件
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
        // 审计 TIN-03：用例体内 DOCTEST_SKIP(...) 会置跳过标记——单独计
        // Skipped（不再混进 Passed），Skipped 不算失败、不影响退出码
        const bool skippedCase = test_registry::consumeSkipped();
        // std::endl 强制 flush，重定向到文件时结果实时可见
        if (ok && skippedCase) {
            std::cout << "[SKIP] " << t.first << " [" << ms << "ms]"
                      << std::endl;
            ++skipped;
        } else if (ok) {
            std::cout << "[PASS] " << t.first << " [" << ms << "ms]"
                      << std::endl;
            ++passed;
        } else {
            std::cout << "[FAIL] " << t.first << " [" << ms << "ms]"
                      << std::endl;
            if (!errMsg.empty()) {
                std::cout << "       exception: " << errMsg << std::endl;
            }
            ++failed;
        }
    }
    std::cout << "\n=== Summary ===" << std::endl;
    std::cout << "Total:  " << total << std::endl;
    std::cout << "Passed: " << passed << std::endl;
    std::cout << "Failed: " << failed << std::endl;
    std::cout << "Skipped: " << skipped << std::endl;
    // 审计 TIN-03：过滤命中 0 个用例 ≠ 全绿。旧口径在此返回 0，上层脚本
    // grep "Failed: 0" 会把「一个用例都没跑」（如 --no_run 误拼降级成过滤器、
    // 过滤词打错）判成 ✅ 全绿——静默绕过「未运行测试 = 未完成」纪律。
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
