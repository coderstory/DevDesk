// tests/probes/wallpaper_rotation_probe.cpp
// 壁纸模块端到端真机探针：扫描 → 入库 → 首播 → 轮换换片 → 停止还原。
// 真 WorkerW 宿主 + 真 mpv（服务默认装配，不注入 player），真 %TEMP% 文件夹入库。
//
// 断言：
//   §1 资源与扫描入库：%TEMP%/devdesk-rotation-probe-<pid> 内拷 clip-a/clip-b，放
//      fake.png（2048B 假图）与 notes.txt（干扰项）。scanVideoLibrary 视频池恰为两支
//      视频（png/txt 扩展名拒绝）；scanImageLibrary 单列 png、拒 mp4/txt；服务快照
//      videoCount==2 且 Video 模式不喂图片池。
//   §2 首播：LoopSingle 恒取清单首条（扫描结果按路径字典序 → clip-a）；IPC path 属性
//      == clip-a 且 time-pos > 0.2s（同 wallpaper_desktop_probe §2 判据）；服务恰
//      spawn 1 个 mpv，宿主消息窗建立。
//   §3 换片（决定性）：service.next()（LoopSingle 下 advanceNow 走清单 index 1）→
//      IPC path 从 clip-a 变为 clip-b；png/txt 全程未被当视频装载；同一 mpv pid 原位
//      换片（loadfile replace），无旧进程残留、无 respawn。
//   §4 停止：service.stop() < 3s（stop 梯子契约）；spawned mpv pid 退出无残留；
//      IPC 管道不可再连；本进程顶层窗口归零（宿主消息窗销毁 = 桌面还原）。
//   §5 续播闭环：LoopSingle stop 落盘 lastPlayedPath==clip-b 且 lastPlayedPosition>0
//      （服务内部 mpv 的真实播放位置）。
// 退出码：0 通过 / 77 非 Windows 跳过 / 非零失败（stderr 中文原因）。
//
// 轮换判据选 mpv IPC path 属性而非快照：快照不含当前播放路径。管道名公开确定
// （makeMpvPipeName(本进程 pid, 首次 spawn counter=1)），本探针作为第二 IPC 客户端
// 连接，读写皆有界（与 mpv_controller.cpp 死锁防线同款纪律：OVERLAPPED、超时撤销 IRP）。
// 数据隔离：设置走 %TEMP% 下独立 settings.json（setSettingsPathForTest），不碰真实
// 数据根；探针不杀任何进程，mpv 由服务自启自收（Job kill-on-close 兜底）。
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
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
#include <tlhelp32.h>

#include "core/paths.h"
#include "module_wallpaper/core/image_library.h"
#include "module_wallpaper/core/settings.h"
#include "module_wallpaper/core/video_library.h"
#include "module_wallpaper/platform/mpv_controller.h"
#include "module_wallpaper/wallpaper_service.h"
#endif

namespace {

constexpr int kSkipExitCode = 77;

#if defined(_WIN32)

using Clock = std::chrono::steady_clock;

bool waitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

bool processStillRunning(std::uint32_t pid) {
    if (pid == 0) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) return false;
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
}

std::string envValue(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

std::wstring utf8ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0) return std::wstring();
    std::vector<wchar_t> buffer(static_cast<std::size_t>(size));
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, buffer.data(), size);
    return std::wstring(buffer.data());
}

// ---- mpv 进程盘点（只观察，不杀；按解析到的 mpv 可执行名匹配） ----

std::vector<std::uint32_t> pidsOfImageName(const std::wstring& imageName) {
    std::vector<std::uint32_t> pids;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, imageName.c_str()) == 0) {
                pids.push_back(entry.th32ProcessID);
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pids;
}

std::vector<std::uint32_t> setDifference(const std::vector<std::uint32_t>& left,
                                         const std::vector<std::uint32_t>& right) {
    std::vector<std::uint32_t> out;
    for (const std::uint32_t pid : left) {
        bool found = false;
        for (const std::uint32_t other : right) {
            if (pid == other) {
                found = true;
                break;
            }
        }
        if (!found) out.push_back(pid);
    }
    return out;
}

// ---- 本进程顶层窗口计数：壁纸宿主消息窗（WS_POPUP，wallpaper_host_win.cpp）是本
//      进程唯一顶层窗口来源；计数归零 = 宿主已销毁 = 桌面还原。 ----

struct WindowCountContext {
    DWORD pid = 0;
    int count = 0;
};

BOOL CALLBACK countWindowProc(HWND window, LPARAM lParam) {
    WindowCountContext* context = reinterpret_cast<WindowCountContext*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == context->pid) ++context->count;
    return TRUE;
}

int countOwnTopLevelWindows() {
    WindowCountContext context;
    context.pid = GetCurrentProcessId();
    EnumWindows(countWindowProc, reinterpret_cast<LPARAM>(&context));
    return context.count;
}

// ---- mpv IPC 第二客户端 ----

// 一次有界 OVERLAPPED 传输；超时撤销 IRP 并排空后返回 false（不遗留悬挂 IO）。
bool overlappedTransfer(HANDLE handle, bool isWrite, void* buffer, DWORD size, DWORD timeoutMs,
                        DWORD* transferred) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) return false;
    const BOOL queued =
        isWrite ? WriteFile(handle, buffer, size, nullptr, &overlapped)
                : ReadFile(handle, buffer, size, nullptr, &overlapped);
    bool ok = false;
    if (queued == 0 && GetLastError() != ERROR_IO_PENDING) {
        // 立即失败（断管等）：无 IRP 可等。
    } else if (WaitForSingleObject(overlapped.hEvent, timeoutMs) == WAIT_OBJECT_0) {
        DWORD got = 0;
        ok = GetOverlappedResult(handle, &overlapped, &got, FALSE) != 0;
        if (isWrite) ok = ok && got == size;
        if (transferred != nullptr) *transferred = got;
    } else {
        (void)CancelIoEx(handle, &overlapped);
        DWORD dropped = 0;
        (void)GetOverlappedResult(handle, &overlapped, &dropped, TRUE);
    }
    CloseHandle(overlapped.hEvent);
    return ok;
}

// 从 compact 后的回施行提取 "data" 字段：字符串解转义，数字原样截取。
bool extractReplyData(const std::string& line, int requestId, std::string* value) {
    std::string compact;
    compact.reserve(line.size());
    for (const char raw : line) {
        if (raw == ' ' || raw == '\t' || raw == '\r' || raw == '\n') continue;
        compact += raw;
    }
    const std::string idKey = "\"request_id\":" + std::to_string(requestId);
    const std::size_t idAt = compact.find(idKey);
    if (idAt == std::string::npos) return false;
    const std::size_t idEnd = idAt + idKey.size();
    // request_id 防前缀撞号（2001 不许匹配 20011）：后随 ',' 或 '}' 才认。
    if (idEnd >= compact.size() || (compact[idEnd] != ',' && compact[idEnd] != '}')) return false;
    if (compact.find("\"error\":\"success\"") == std::string::npos) return false;
    const std::size_t dataAt = compact.find("\"data\":");
    if (dataAt == std::string::npos) return false;
    const std::size_t valueStart = dataAt + 7;
    if (valueStart >= compact.size()) return false;
    if (compact[valueStart] != '"') {
        const std::size_t numberEnd = compact.find_first_of(",}", valueStart);
        if (numberEnd == std::string::npos) return false;
        if (value != nullptr) *value = compact.substr(valueStart, numberEnd - valueStart);
        return true;
    }
    std::string decoded;
    std::size_t index = valueStart + 1;
    while (index < compact.size()) {
        const char ch = compact[index];
        if (ch == '"') {
            if (value != nullptr) *value = decoded;
            return true;
        }
        if (ch != '\\') {
            decoded += ch;
            ++index;
            continue;
        }
        if (++index >= compact.size()) return false;
        switch (compact[index]) {
            case '"': decoded += '"'; break;
            case '\\': decoded += '\\'; break;
            case '/': decoded += '/'; break;
            case 'b': decoded += '\b'; break;
            case 'f': decoded += '\f'; break;
            case 'n': decoded += '\n'; break;
            case 'r': decoded += '\r'; break;
            case 't': decoded += '\t'; break;
            case 'u':
                if (index + 4 >= compact.size()) return false;
                index += 4;  // 路径判据只依赖 ASCII 基名，非 ASCII 逐字节跳过即可
                break;
            default: return false;
        }
        ++index;
    }
    return false;
}

class MpvIpcClient {
public:
    MpvIpcClient() = default;
    ~MpvIpcClient() { disconnect(); }

    MpvIpcClient(const MpvIpcClient&) = delete;
    MpvIpcClient& operator=(const MpvIpcClient&) = delete;

    // 有界重试连接（mpv spawn + 建管为异步，需等待窗口）。
    bool connect(const std::string& pipeName, std::chrono::milliseconds timeout) {
        const std::wstring wide = utf8ToWide(pipeName);
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                pipe_ = handle;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }

    void disconnect() {
        if (pipe_ == nullptr) return;
        (void)CancelIoEx(pipe_, nullptr);
        CloseHandle(pipe_);
        pipe_ = nullptr;
    }

    // get_property 一次问答：写命令（1s 有界）→ 读回施行流直到命中本请求 id。
    bool getProperty(const std::string& name, int requestId, std::string* value,
                     std::chrono::milliseconds timeout) {
        if (pipe_ == nullptr) return false;
        const std::string command = "{\"command\":[\"get_property\",\"" +
                                    module::wallpaper::mpvJsonEscape(name) +
                                    "\"],\"request_id\":" + std::to_string(requestId) + "}\n";
        if (!overlappedTransfer(pipe_, true, const_cast<char*>(command.c_str()),
                                static_cast<DWORD>(command.size()), 1000, nullptr)) {
            return false;
        }
        const auto deadline = Clock::now() + timeout;
        std::string pending;
        char buffer[4096];
        while (Clock::now() < deadline) {
            DWORD got = 0;
            if (!overlappedTransfer(pipe_, false, buffer, sizeof(buffer), 200, &got)) continue;
            if (got == 0) continue;
            pending.append(buffer, got);
            std::size_t newline = pending.find('\n');
            while (newline != std::string::npos) {
                const std::string line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                std::string data;
                if (extractReplyData(line, requestId, &data)) {
                    if (value != nullptr) *value = data;
                    return true;
                }
                newline = pending.find('\n');
            }
        }
        return false;
    }

private:
    HANDLE pipe_ = nullptr;
};

// ---- 路径判据：mpv 对 Windows 路径的斜杠/大小写不做保证，统一归一后比基名 ----

std::string normalizePathKey(const std::string& path) {
    std::string key;
    key.reserve(path.size());
    for (const char raw : path) {
        char ch = raw;
        if (ch == '\\') ch = '/';
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        key += ch;
    }
    return key;
}

bool pathContains(const std::string& value, const std::string& basename) {
    return normalizePathKey(value).find(normalizePathKey(basename)) != std::string::npos;
}

void writePatternFile(const std::filesystem::path& path, std::size_t size) {
    std::ofstream out(path, std::ios::binary);
    for (std::size_t index = 0; index < size; ++index) {
        out.put(static_cast<char>((index * 31 + 17) & 0xFF));
    }
}

#endif  // _WIN32

}  // namespace

int main(int argc, char* argv[]) {
#if !defined(_WIN32)
    (void)argc;
    (void)argv;
    std::cout << "wallpaper_rotation_probe: 非 Windows 平台，跳过（WorkerW/mpv 仅 Windows）"
              << " exit=" << kSkipExitCode << "\n";
    return kSkipExitCode;
#else
    (void)argc;
    (void)argv;

    // 探针独立进程：对齐 DevDesk 的 PerMonitorV2（同 wallpaper_desktop_probe）。
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    bool passed = true;
    auto check = [&passed](bool condition, const std::string& what) {
        if (condition) {
            std::cout << "[PASS] " << what << "\n";
        } else {
            std::cerr << "[FAIL] " << what << "\n";
            passed = false;
        }
    };

    const std::filesystem::path fixtureDir =
        "D:/Data/Downloads/dynamic-wallpaper-master/fixtures";
    const std::filesystem::path clipA = fixtureDir / "clip-a.mp4";
    const std::filesystem::path clipB = fixtureDir / "clip-b.mp4";
    if (!std::filesystem::is_regular_file(clipA) || !std::filesystem::is_regular_file(clipB)) {
        std::cerr << "wallpaper_rotation_probe: 测试视频不存在: " << fixtureDir.string() << "\n";
        return 3;
    }

    // mpv 预检（服务内部同款解析优先级）；可执行名供进程盘点用。
    bool usedDevFallback = false;
    const std::string mpvPath = module::wallpaper::resolveMpvExecutable(
        envValue(module::wallpaper::kMpvPathEnvVar), core::paths::executableDir().string(),
        module::wallpaper::kMpvDevFallbackPath, true, &usedDevFallback);
    if (mpvPath.empty()) {
        std::cerr << "wallpaper_rotation_probe: 找不到 mpv.exe（DEVDESK_MPV_PATH / "
                     "<exeDir>/assets/mpv/mpv.exe / 开发兜底）\n";
        return 4;
    }
    if (usedDevFallback) {
        std::cout << "[INFO] 使用开发兜底 mpv: " << mpvPath << "\n";
    }
    const std::wstring mpvImageName =
        utf8ToWide(std::filesystem::path(mpvPath).filename().string());

    // ── §1 资源：%TEMP% 真文件夹（2 视频 + 假图 + 干扰文本） ──
    const std::filesystem::path tempDir = std::filesystem::temp_directory_path() /
                                          ("devdesk-rotation-probe-" +
                                           std::to_string(GetCurrentProcessId()));
    {
        std::error_code ec;
        std::filesystem::remove_all(tempDir, ec);
    }
    std::error_code fsEc;
    std::filesystem::create_directories(tempDir, fsEc);
    const std::filesystem::path tempClipA = tempDir / "clip-a.mp4";
    const std::filesystem::path tempClipB = tempDir / "clip-b.mp4";
    const std::filesystem::path tempFakePng = tempDir / "fake.png";
    const std::filesystem::path tempNotes = tempDir / "notes.txt";
    std::filesystem::copy_file(clipA, tempClipA, std::filesystem::copy_options::overwrite_existing,
                               fsEc);
    std::filesystem::copy_file(clipB, tempClipB, std::filesystem::copy_options::overwrite_existing,
                               fsEc);
    writePatternFile(tempFakePng, 2048);
    {
        std::ofstream notes(tempNotes, std::ios::binary);
        for (int index = 0; index < 8; ++index) {
            notes << "devdesk rotation probe decoy entry (not a media file).\n";
        }
    }
    const bool resourcesOk = std::filesystem::is_regular_file(tempClipA) &&
                             std::filesystem::is_regular_file(tempClipB) &&
                             std::filesystem::file_size(tempFakePng, fsEc) >= 512 &&
                             std::filesystem::is_regular_file(tempNotes);
    check(resourcesOk, "§1 资源：%TEMP% 真文件夹就绪（clip-a/clip-b 拷贝 + fake.png 2048B + "
                       "notes.txt 干扰项）: " + tempDir.string());
    if (!resourcesOk) {
        std::error_code cleanupEc;
        std::filesystem::remove_all(tempDir, cleanupEc);
        return 6;
    }

    // ── §1 扫描语义（真扫描器直查：png 单列图片池 / txt 两池皆拒） ──
    const module::wallpaper::VideoScanResult videoScan =
        module::wallpaper::scanVideoLibrary(tempDir);
    check(videoScan.status == module::wallpaper::LibraryScanStatus::Ok &&
              videoScan.files.size() == 2 &&
              pathContains(videoScan.files[0], "clip-a.mp4") &&
              pathContains(videoScan.files[1], "clip-b.mp4") &&
              videoScan.extensionRejected == 2,
          "§1 扫描：视频池恰 2 支 clip-a/clip-b（字典序），png/txt 被扩展名拒绝（实测拒 " +
              std::to_string(videoScan.extensionRejected) + "）");

    const module::wallpaper::ImageScanResult imageScan =
        module::wallpaper::scanImageLibrary(tempDir, 0);
    check(imageScan.status == module::wallpaper::LibraryScanStatus::Ok &&
              imageScan.files.size() == 1 &&
              pathContains(imageScan.files[0], "fake.png") &&
              imageScan.total == 1 && imageScan.extensionRejected == 3,
          "§1 扫描：png 单列图片池（total=1），mp4×2 与 txt 均被拒（图片侧实测拒 " +
              std::to_string(imageScan.extensionRejected) + "）");

    // 盘点基线（服务启动前）与顶层窗口基线。
    const std::vector<std::uint32_t> mpvBaseline = pidsOfImageName(mpvImageName);
    const int windowsBefore = countOwnTopLevelWindows();

    // ── 服务：真装配（不注入 player → 真 WorkerW 宿主 + 真 mpv；设置隔离到 %TEMP%） ──
    module::wallpaper::WallpaperService service;
    service.setSettingsPathForTest((tempDir / "settings.json").string());
    std::string folderError;
    if (!service.selectFolder(tempDir.string(), &folderError)) {
        std::cerr << "wallpaper_rotation_probe: selectFolder 失败: " << folderError << "\n";
        std::error_code cleanupEc;
        std::filesystem::remove_all(tempDir, cleanupEc);
        return 5;
    }
    service.start();

    auto cleanup = [&service, &tempDir]() {
        service.stop();
        std::error_code ec;
        std::filesystem::remove_all(tempDir, ec);
    };

    // ── §1 服务入库 ──
    const bool ingested = waitUntil(
        [&service]() {
            const module::wallpaper::WallpaperSnapshot snap = service.snapshot();
            return !snap.scanning && snap.availability == module::wallpaper::Availability::Playing &&
                   snap.videoCount == 2;
        },
        std::chrono::milliseconds(10000));
    const module::wallpaper::WallpaperSnapshot ingestedSnap = service.snapshot();
    check(ingested && ingestedSnap.videoCount == 2,
          "§1 入库：服务快照 videoCount == 2（实测 " +
              std::to_string(ingestedSnap.videoCount) + "，可用性 Playing、扫描结束）");
    check(ingestedSnap.imageTotal == 0 && ingestedSnap.imagePassing == 0,
          "§1 入库：Video 模式图片池不受 png 干扰（imageTotal 实测 " +
              std::to_string(ingestedSnap.imageTotal) + "）");
    if (!ingested) {
        cleanup();
        return 7;
    }

    // ── §2 首播：IPC path == clip-a，position 增长（同 desktop_probe §2 判据） ──
    MpvIpcClient ipc;
    // 首次 spawn 的管道 counter=1（控制器 spawn 前 fetch_add），进程号 = 本探针进程。
    const std::string pipeName =
        module::wallpaper::makeMpvPipeName(GetCurrentProcessId(), 1);
    const bool pipeUp = ipc.connect(pipeName, std::chrono::milliseconds(10000));
    check(pipeUp, "§2 首播：mpv IPC 管道可连（" + pipeName + "）");
    if (!pipeUp) {
        cleanup();
        return 8;
    }

    std::string pathBefore;
    const bool firstIsClipA = waitUntil(
        [&]() {
            return ipc.getProperty("path", 2001, &pathBefore, std::chrono::milliseconds(800)) &&
                   pathContains(pathBefore, "clip-a.mp4");
        },
        std::chrono::milliseconds(8000));
    check(firstIsClipA,
          "§2 首播：当前播放文件 == clip-a（IPC path 实测 \"" + pathBefore +
              "\"；png/txt 未被当视频装载）");

    double positionAfterStart = 0.0;
    const bool grew = waitUntil(
        [&]() {
            std::string text;
            if (!ipc.getProperty("time-pos", 2002, &text, std::chrono::milliseconds(800))) {
                return false;
            }
            positionAfterStart = std::strtod(text.c_str(), nullptr);
            return positionAfterStart > 0.2;
        },
        std::chrono::milliseconds(8000));
    check(grew, "§2 首播：time-pos > 0.2s（实测 " + std::to_string(positionAfterStart) +
                    "s，同 wallpaper_desktop_probe §2 判据）");

    const std::vector<std::uint32_t> spawned =
        setDifference(pidsOfImageName(mpvImageName), mpvBaseline);
    check(spawned.size() == 1, "§2 首播：服务恰好 spawn 1 个 mpv（实测 " +
                                   std::to_string(spawned.size()) + " 个，基线 " +
                                   std::to_string(mpvBaseline.size()) + " 个）");
    const std::uint32_t mpvPid = spawned.empty() ? 0 : spawned[0];
    const int windowsDuringPlay = countOwnTopLevelWindows();
    check(spawned.size() == 1 && processStillRunning(mpvPid) && windowsDuringPlay > windowsBefore,
          "§2 首播：mpv pid " + std::to_string(mpvPid) + " 存活，宿主消息窗已建立（本进程顶层窗口 " +
              std::to_string(windowsDuringPlay) + " > 启动前 " + std::to_string(windowsBefore) + "）");
    if (!firstIsClipA || !grew || spawned.size() != 1) {
        cleanup();
        return 8;
    }

    // ── §3 轮换换片（决定性）：手动 next → IPC path clip-a → clip-b ──
    service.next();
    std::string pathAfter;
    const bool switched = waitUntil(
        [&]() {
            return ipc.getProperty("path", 2003, &pathAfter, std::chrono::milliseconds(800)) &&
                   pathContains(pathAfter, "clip-b.mp4");
        },
        std::chrono::milliseconds(8000));
    check(switched, "§3 换片：当前播放文件 clip-a → clip-b（IPC path 实测 \"" + pathAfter +
                        "\"；png/txt 仍不在播放位）");

    const std::vector<std::uint32_t> afterSwitch =
        setDifference(pidsOfImageName(mpvImageName), mpvBaseline);
    const bool noRespawn = afterSwitch.size() == 1 && afterSwitch[0] == mpvPid &&
                           processStillRunning(mpvPid);
    check(noRespawn, "§3 换片：原位换片（loadfile replace）无旧进程残留（同一 mpv pid " +
                         std::to_string(mpvPid) + " 仍存活，无 respawn）");
    if (!switched) {
        cleanup();
        return 9;
    }

    // 让 clip-b 的 time-pos 走起来（§5 落盘 position>0 需要真实播放进度）。
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // ── §4 停止：service.stop < 3s，mpv 退出无残留，管道关，桌面还原 ──
    const auto stopStarted = Clock::now();
    service.stop();
    const double stopElapsed =
        std::chrono::duration<double>(Clock::now() - stopStarted).count();
    check(stopElapsed < 3.0, "§4 停止：service.stop() 耗时 " + std::to_string(stopElapsed) +
                                 "s < 3s（stop 梯子契约）");

    const bool exited = waitUntil([&]() { return !processStillRunning(mpvPid); },
                                  std::chrono::milliseconds(3000));
    const bool orphan = processStillRunning(mpvPid);
    check(exited && !orphan, "§4 停止：mpv pid " + std::to_string(mpvPid) +
                                 " 已退出且无残留（≤3s）");

    ipc.disconnect();
    bool pipeGone = true;
    {
        HANDLE probe = CreateFileW(utf8ToWide(pipeName).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (probe != INVALID_HANDLE_VALUE) {
            CloseHandle(probe);
            pipeGone = false;
        }
    }
    check(pipeGone, "§4 停止：IPC 管道不可再连（服务端已随 mpv 退出）");

    const int windowsAfterStop = countOwnTopLevelWindows();
    check(windowsAfterStop == windowsBefore,
          "§4 停止：桌面还原（本进程顶层窗口 " + std::to_string(windowsAfterStop) +
              " == 启动前 " + std::to_string(windowsBefore) + "，宿主消息窗已销毁）");

    // ── §5 续播闭环：LoopSingle stop 落盘 lastPlayedPath==clip-b 且 position>0 ──
    const module::wallpaper::WallpaperSettings persisted = module::wallpaper::loadWallpaperSettings(
        tempDir / "settings.json");
    check(normalizePathKey(persisted.lastPlayedPath) ==
                  normalizePathKey(videoScan.files[1]) &&
              persisted.lastPlayedPosition > 0.0,
          "§5 续播：lastPlayedPath == clip-b（实测 \"" + persisted.lastPlayedPath +
              "\"）且 lastPlayedPosition > 0（实测 " +
              std::to_string(persisted.lastPlayedPosition) + "s）");

    {
        std::error_code ec;
        std::filesystem::remove_all(tempDir, ec);
    }
    if (!passed) {
        std::cerr << "wallpaper_rotation_probe: FAIL\n";
        return 1;
    }
    std::cout << "wallpaper_rotation_probe: PASS\n";
    return 0;
#endif
}
