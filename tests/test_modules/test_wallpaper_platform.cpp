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

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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