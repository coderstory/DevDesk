// tests/test_modules/test_wallpaper_platform.cpp
// 壁纸平台层纯逻辑（Batch 3a）：mpv 路径解析优先级、§6.2 参数表、命名管道名、
// JSON 命令构造/转义、time-pos 事件解析、CreateProcess 命令行转义、stub 行为。
//
// 不启动 mpv、不建 WorkerW（真桌面验证在 probes/wallpaper_desktop_probe.cpp）。
// REGISTER_TEST 描述串必须含 ASCII "wallpaper"（--filter 只认 ASCII）。
#include "doctest.h"
#include "test_support.h"

#include "module_wallpaper/platform/mpv_controller.h"
#include "module_wallpaper/platform/mpv_player.h"
#include "module_wallpaper/platform/wallpaper_host.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

namespace wp = module::wallpaper;
namespace fs = std::filesystem;

void makePlaceholderFile(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "";
}

// REGISTER_TEST 的用例体不能内嵌预处理指令（-Wpedantic: directive within macro
// arguments）；平台分支放进普通函数，返回值即用例结果。
bool checkPlatformStubBehavior() {
#if !defined(_WIN32)
    wp::MpvController controller;
    DOCTEST_CHECK(controller.takeError().empty());
    controller.start(1234);
    const std::string error = controller.takeError();
    DOCTEST_CHECK(!error.empty());
    DOCTEST_CHECK_EQ(controller.positionSec(), 0.0f);
    DOCTEST_CHECK_FALSE(controller.processAlive());
    DOCTEST_CHECK_EQ(controller.processId(), 0u);
    controller.stop();

    std::unique_ptr<wp::WallpaperHost> host = wp::createWallpaperHost();
    DOCTEST_REQUIRE(host != nullptr);
    std::string hostError;
    DOCTEST_CHECK_FALSE(host->start(&hostError));
    DOCTEST_CHECK(!hostError.empty());
    DOCTEST_CHECK(host->handle().videoChild == nullptr);
    host->stop();
#else
    wp::MpvPlayer player;
    DOCTEST_CHECK_EQ(player.positionSec(), 0.0f);
    DOCTEST_CHECK_FALSE(player.processAlive());
    DOCTEST_CHECK_EQ(player.processId(), 0u);

    std::unique_ptr<wp::WallpaperHost> host = wp::createWallpaperHost();
    DOCTEST_REQUIRE(host != nullptr);
    // 不调用 start：Windows 上会真的建 WorkerW 宿主，属于桌面探针的职责。
    DOCTEST_CHECK(host->handle().videoChild == nullptr);
#endif
    return true;
}

}  // namespace

// 防：mpv 路径解析不按 §6.2 优先级 / 开发兜底在应禁用的场合仍生效 / 找不到时静默返回
// 一个不存在的路径（后续 spawn 报错却指向错误原因）。
REGISTER_TEST(wallpaper_platform_mpv_path,
              "wallpaper platform: mpv 路径解析（env → assets → 开发兜底）", {
    test_support::TempDir dir("wp_platform_path");
    const fs::path appDir = dir.path / "app";
    const fs::path envMpv = dir.path / "env" / "mpv.exe";
    const fs::path bundled = appDir / "assets" / "mpv" / "mpv.exe";
    const fs::path devMpv = dir.path / "dev" / "mpv.exe";
    makePlaceholderFile(envMpv);
    makePlaceholderFile(bundled);
    makePlaceholderFile(devMpv);

    bool usedDev = true;
    std::string resolved = wp::resolveMpvExecutable(envMpv.string(), appDir.string(),
                                                    devMpv.string(), true, &usedDev);
    DOCTEST_CHECK_EQ(resolved, envMpv.string());
    DOCTEST_CHECK_FALSE(usedDev);

    // env 指向不存在的文件 → 落到 <exeDir>/assets/mpv/mpv.exe
    resolved = wp::resolveMpvExecutable((dir.path / "missing" / "mpv.exe").string(),
                                        appDir.string(), devMpv.string(), true, &usedDev);
    DOCTEST_CHECK_EQ(resolved, bundled.string());
    DOCTEST_CHECK_FALSE(usedDev);

    // 打包路径也没有 → 开发兜底（仅 allowDevFallback=true）
    std::error_code ec;
    fs::remove(bundled, ec);
    resolved = wp::resolveMpvExecutable("", appDir.string(), devMpv.string(), true, &usedDev);
    DOCTEST_CHECK_EQ(resolved, devMpv.string());
    DOCTEST_CHECK(usedDev);

    // Release 构建语义：禁止开发兜底 → 明确返回空串（调用方给中文错误）
    usedDev = true;
    resolved = wp::resolveMpvExecutable("", appDir.string(), devMpv.string(), false, &usedDev);
    DOCTEST_CHECK(resolved.empty());
    DOCTEST_CHECK_FALSE(usedDev);
    return true;
})

// 防：mpv 启动参数漏项/拼错导致嵌入失败或 OSD/输入劫持桌面（§6.2 是逐字契约）。
REGISTER_TEST(wallpaper_platform_mpv_arguments,
              "wallpaper platform: mpv §6.2 参数表与命名管道名", {
    const std::string pipe = wp::makeMpvPipeName(4321, 7);
    DOCTEST_CHECK_EQ(pipe, std::string("\\\\.\\pipe\\devdesk-wallpaper-mpv-4321-7"));
    DOCTEST_CHECK(pipe.find("devdesk-wallpaper-mpv-") != std::string::npos);

    const std::vector<std::string> args = wp::buildMpvArguments(4660, pipe);
    const std::vector<std::string> expected = {
        "--no-config",
        "--idle=yes",
        "--wid=4660",
        "--input-ipc-server=" + pipe,
        "--loop-file=inf",
        "--hwdec=auto-safe",
        "--no-osc",
        "--no-input-default-bindings",
        "--no-osd-bar",
        "--osd-level=0",
        "--force-window=yes",
        "--vo=gpu",
        "--panscan=1.0",
        "--audio-pitch-correction=yes",
        "--no-terminal",
        "--keep-open=no",
        "--pause=yes",
        "--sub-auto=no",
    };
    DOCTEST_CHECK_EQ(args.size(), expected.size());
    for (std::size_t index = 0; index < expected.size() && index < args.size(); ++index) {
        DOCTEST_CHECK_EQ(args[index], expected[index]);
    }
    return true;
})

// 防：JSON 命令拼接未转义（Windows 反斜杠路径被当转义序列 → loadfile 失败）/
// time-pos 观察字段名/编号不一致导致位置永远 0。
REGISTER_TEST(wallpaper_platform_mpv_commands,
              "wallpaper platform: mpv JSON 命令与 time-pos 事件解析", {
    DOCTEST_CHECK_EQ(wp::mpvJsonEscape("C:\\videos\\a.mp4"),
                     std::string("C:\\\\videos\\\\a.mp4"));
    DOCTEST_CHECK_EQ(wp::mpvJsonEscape("say \"hi\""), std::string("say \\\"hi\\\""));
    DOCTEST_CHECK_EQ(wp::mpvLoadFileCommand("C:\\videos\\a.mp4"),
                     std::string("{\"command\":[\"loadfile\",\"C:\\\\videos\\\\a.mp4\","
                                 "\"replace\"]}"));
    DOCTEST_CHECK_EQ(wp::mpvSetPropertyCommand("pause", true),
                     std::string("{\"command\":[\"set_property\",\"pause\",true]}"));
    DOCTEST_CHECK_EQ(wp::mpvSetPropertyCommand("pause", false),
                     std::string("{\"command\":[\"set_property\",\"pause\",false]}"));
    DOCTEST_CHECK_EQ(wp::mpvSetPropertyCommand("speed", 1.25),
                     std::string("{\"command\":[\"set_property\",\"speed\",1.25]}"));
    DOCTEST_CHECK_EQ(wp::mpvSetPropertyCommand("volume", 100.0),
                     std::string("{\"command\":[\"set_property\",\"volume\",100]}"));
    DOCTEST_CHECK_EQ(wp::mpvSeekCommand(1.0),
                     std::string("{\"command\":[\"seek\",1,\"absolute+exact\"]}"));
    DOCTEST_CHECK_EQ(wp::mpvObservePropertyCommand(1, "time-pos"),
                     std::string("{\"command\":[\"observe_property\",1,\"time-pos\"]}"));
    DOCTEST_CHECK_EQ(wp::mpvQuitCommand(), std::string("{\"command\":[\"quit\"]}"));

    double seconds = -1.0;
    DOCTEST_CHECK(wp::parseMpvTimePosEvent(
        R"({"event":"property-change","id":1,"name":"time-pos","data":1.25})", &seconds));
    DOCTEST_CHECK(std::fabs(seconds - 1.25) < 1e-6);
    // mpv 不同版本可能带空格
    DOCTEST_CHECK(wp::parseMpvTimePosEvent(
        R"({ "event": "property-change", "name": "time-pos", "data": 2.5 })", &seconds));
    DOCTEST_CHECK(std::fabs(seconds - 2.5) < 1e-6);
    // data 为 null（未装载）→ 不更新位置
    DOCTEST_CHECK_FALSE(wp::parseMpvTimePosEvent(
        R"({"event":"property-change","id":1,"name":"time-pos","data":null})", &seconds));
    // 命令回执 / 其它事件不得被误判为位置
    DOCTEST_CHECK_FALSE(wp::parseMpvTimePosEvent(
        R"({"request_id":1,"error":"success"})", &seconds));
    DOCTEST_CHECK_FALSE(wp::parseMpvTimePosEvent(
        R"({"event":"end-file","name":"time-pos","data":1.0})", &seconds));
    return true;
})

// 防：CreateProcess 命令行拼接不转义空格/尾反斜杠/引号 → mpv 收到被截断的参数。
REGISTER_TEST(wallpaper_platform_command_line,
              "wallpaper platform: mpv 命令行参数转义", {
    DOCTEST_CHECK_EQ(wp::quoteWindowsCommandLineArg("plain"), std::string("plain"));
    DOCTEST_CHECK_EQ(wp::quoteWindowsCommandLineArg("a b"), std::string("\"a b\""));
    DOCTEST_CHECK_EQ(wp::quoteWindowsCommandLineArg("a b\\"), std::string("\"a b\\\\\""));
    DOCTEST_CHECK_EQ(wp::quoteWindowsCommandLineArg("say\"hi"), std::string("\"say\\\"hi\""));
    // 契约同 quoteWindowsCommandLineArg 头注（MS 规则：反斜杠只在配对引号前
    // 翻倍）：路径引号内的非尾部反斜杠保持单份——翻倍会让 mpv 的 argv 收到
    // 逐字双反斜杠路径。
    DOCTEST_CHECK_EQ(
        wp::buildWindowsCommandLine("C:\\Program Files\\mpv\\mpv.exe", {"--wid=1"}),
        std::string("\"C:\\Program Files\\mpv\\mpv.exe\" --wid=1"));
    return true;
})

// 防：非 Windows 平台调用真实控制器/宿主时静默成功或崩溃（必须明确失败并可展示原因）。
// Windows 分支只验证空态（构造不 spawn 进程），真进程行为由桌面探针覆盖。
REGISTER_TEST(wallpaper_platform_stub_behavior,
              "wallpaper platform: 非 Windows stub 行为与空态", {
    return checkPlatformStubBehavior();
})

// 防（审计 F-0）：OVERLAPPED 有界写死锁修复（eec88dbb）无自动回归网——回退成同步
// 句柄写后全部单测仍绿，唯一防线是需真机 + mpv 的手动探针。本用例在进程内造
// 「管道对端连上后故意不读」的场景（真命名管道 + 伪进程句柄，不 spawn 真进程），
// 把该路径钉成自动红：
//   ① 写超时必须按 kIpcWriteTimeoutMs（1000ms）有界返回且错误槽出现"写入超时"；
//   ② stop 梯子全程 < 3s（quit 300ms + 伪进程宽限 800 + 收尾 500）；
//   ③ stop 兜底写不得刷错误槽（recordFailure=false 契约）。
// 回退实现下 worker 卡死在无界 WriteFile：①超时不出现、②stop 等满 5s 信号——
// 两条判据先后判红；此时控制器/管道/线程故意泄漏（不 delete、不 join），避免
// 析构 join 把失败报告吞成挂死，进程退出由 OS 回收。
bool checkIpcWriteBoundedRegression() {
#if !defined(_WIN32)
    // 命名管道 / OVERLAPPED 仅 Windows：死锁回归网只在 Windows 编译。
    return true;
#else
    namespace wp = module::wallpaper;

    const std::string pipeName = wp::makeMpvPipeName(::GetCurrentProcessId(), 0x5EC1);
    const std::wstring pipeWide(pipeName.begin(), pipeName.end());
    // 服务端：只建不读。首次 4096 写占满配额后，后续写必然挂起——writeBounded
    // 超时路径的前提。
    HANDLE server = ::CreateNamedPipeW(pipeWide.c_str(), PIPE_ACCESS_INBOUND,
                                       PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096,
                                       0, nullptr);
    DOCTEST_REQUIRE(server != INVALID_HANDLE_VALUE);
    // 客户端必须 OVERLAPPED：控制器在其上做 OVERLAPPED 写（死锁防线的根）。
    HANDLE client =
        ::CreateFileW(pipeWide.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                      FILE_FLAG_OVERLAPPED, nullptr);
    DOCTEST_REQUIRE(client != INVALID_HANDLE_VALUE);
    // 伪进程句柄：未复位的手动复位事件。TerminateProcess 对其失败被忽略、wait
    // 永不信号——stop 梯子走满 300+800+500ms 有界路径，不引入真子进程。
    HANDLE fakeProcess = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DOCTEST_REQUIRE(fakeProcess != nullptr);

    // 预填管道到「剩余配额 < 单条命令长度」（42B）：先 256KB 大块写制造挂起
    //（远超配额，必挂起）后取消——取消写是否落盘不确定，所以落了多少以
    // PeekNamedPipe 为准，再逐字节补齐。完成态写真实消耗配额，无不确定性；
    // free = quota - avail 严格成立（服务端只建不读、只有本线程在写）。
    DWORD actualInBuffer = 0;
    DWORD actualOutBuffer = 0;
    DOCTEST_REQUIRE(::GetNamedPipeInfo(server, nullptr, &actualOutBuffer,
                                       &actualInBuffer, nullptr) != 0);
    DOCTEST_REQUIRE(actualInBuffer >= 64);
    {
        OVERLAPPED overlapped{};
        overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DOCTEST_REQUIRE(overlapped.hEvent != nullptr);
        std::vector<char> chunk(256 * 1024, 'x');
        const BOOL queued =
            ::WriteFile(client, chunk.data(), static_cast<DWORD>(chunk.size()),
                        nullptr, &overlapped);
        if (queued == 0 && ::GetLastError() == ERROR_IO_PENDING) {
            (void)::WaitForSingleObject(overlapped.hEvent, 1500);
            (void)::CancelIoEx(client, &overlapped);
        }
        DWORD dropped = 0;
        (void)::GetOverlappedResult(client, &overlapped, &dropped, TRUE);
        ::CloseHandle(overlapped.hEvent);
    }
    auto availBytes = [&server]() -> DWORD {
        DWORD avail = 0;
        return ::PeekNamedPipe(server, nullptr, 0, nullptr, &avail, nullptr) != 0
                   ? avail
                   : 0;
    };
    const DWORD quota = actualInBuffer;
    bool fillBroken = false;
    while (availBytes() + 41 < quota) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DOCTEST_REQUIRE(overlapped.hEvent != nullptr);
        char one = 'y';
        const BOOL queued = ::WriteFile(client, &one, 1, nullptr, &overlapped);
        if (queued == 0 && ::GetLastError() == ERROR_IO_PENDING) {
            // 配额簿记与 PeekNamedPipe 不一致才会走到这里：放弃补齐，让下方
            // 前置条件断言显式红，不带着未证实的场景继续。
            (void)::CancelIoEx(client, &overlapped);
            DWORD dropped = 0;
            (void)::GetOverlappedResult(client, &overlapped, &dropped, TRUE);
            ::CloseHandle(overlapped.hEvent);
            fillBroken = true;
            break;
        }
        ::CloseHandle(overlapped.hEvent);
    }
    DOCTEST_REQUIRE_MESSAGE(
        !fillBroken && quota - availBytes() <= 41,
        "测试管道剩余配额应 < 单条命令长度（42B），前置条件不成立");

    wp::MpvController* controller = new wp::MpvController();
    wp::MpvControllerTestAccess::attachIpcForTest(*controller, client, fakeProcess);

    // ① 命令写走 sendLine（kIpcWriteTimeoutMs 有界写）。主线程只轮询错误槽——
    // 实现回退成同步写时 worker 卡死，主线程不跟着挂，超时后在此判红。
    controller->pause(true);
    const auto writeStarted = std::chrono::steady_clock::now();
    std::string error;
    while (std::chrono::steady_clock::now() < writeStarted + std::chrono::milliseconds(5500)) {
        error = controller->takeError();
        if (!error.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto writeElapsed = std::chrono::steady_clock::now() - writeStarted;
    if (error.empty()) {
        ::CloseHandle(server);
        return false;  // 回退形态：错误槽永不出现 → 红（controller 故意泄漏）
    }
    DOCTEST_CHECK_MESSAGE(error.find("写入超时") != std::string::npos,
                          "错误文案应说明写超时（通道死亡）: " + error);
    // 下界钉住超时档（1000ms）；上界容忍调度抖动。
    DOCTEST_CHECK(writeElapsed >= std::chrono::milliseconds(900));
    DOCTEST_CHECK(writeElapsed <= std::chrono::milliseconds(5500));

    // ② stop 梯子全程 < 3s；③ 兜底写不刷错误槽。
    const auto stopStarted = std::chrono::steady_clock::now();
    controller->stop();
    const auto stopElapsed = std::chrono::steady_clock::now() - stopStarted;
    if (stopElapsed >= std::chrono::seconds(3)) {
        ::CloseHandle(server);
        return false;  // 回退形态：worker 卡死、stop 等满 5s → 红（同上故意泄漏）
    }
    DOCTEST_CHECK(stopElapsed < std::chrono::seconds(3));
    DOCTEST_CHECK(controller->takeError().empty());

    delete controller;  // 绿路径正常收尾（客户端句柄与伪进程事件已随 shutdown 关闭）
    ::CloseHandle(server);
    return true;
#endif
}

REGISTER_TEST(wallpaper_platform_ipc_write_bounded,
              "wallpaper platform: 管道对端拒绝读时写有界超时且 stop 梯子 <3s（死锁回归网）", {
    return checkIpcWriteBoundedRegression();
})
