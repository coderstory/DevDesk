// tests/probes/wallpaper_image_rotation_probe.cpp
// 壁纸「图片播放 + 服务端定时轮换」端到端真机探针：混合池扫描 → 首播视频 →
// 间隔到点自动切图（无手动 next）→ 再到点自动回视频 → 图片态安全语义 → 停止还原。
// 真 WorkerW 宿主 + 真 mpv（服务默认装配，不注入 player），真 %TEMP% 文件夹入库。
//
// 断言（每条一行 PASS/FAIL）：
//   §1 资源与扫描：%TEMP%/devdesk-image-rotation-probe-<pid> 内 clip-a.mp4 + libpng
//      生成 2560×1440 合法 PNG（恰过默认 2K 档：像素档过滤落地后小图会被服务侧
//      WIC 探针滤出池）+ 干扰 txt。直查真扫描器：视频池恰 1（png/txt 扩展名拒绝）、
//      图片池恰 1（mp4/txt 拒绝）。
//   §2 混合池入库：服务快照 poolCount==2（videoCount==1 + poolImageCount==1）、
//      可用性 Playing；首播 IPC path == clip-a（池全局字典序首条）；快照 currentItemImage
//      == false。
//   §3 定时轮换（决定性）：interval 设为代码允许最小档 300s，注入虚拟时钟推进一个
//      间隔后（无任何手动 next），有界窗口内 IPC path 自动变为 PNG——实证服务端
//      计时线程到点自动换片；快照 currentItemImage 翻真。
//   §4 静图常驻：PNG 显示后实等 2.5s，path 仍是 PNG 且 mpv 存活——mpv 默认
//      image-display-duration=1s 会自行切走，只有 spawn 全局 inf 才能常驻。
//   §5 再等一个 interval 自动回到 clip-a；再切回 PNG 后图片态语义：IPC time-pos
//      查询成功且为 0（不报错）；service 手动暂停/恢复全程 PNG 不掉、mpv 存活
//      （pause 对静图为安全 no-op）。
//   §6 停止：service.stop() < 3s；mpv pid 退出无残留；IPC 管道不可再连；本进程
//      顶层窗口归零（宿主消息窗销毁 = 桌面还原）。
// 退出码：0 通过 / 77 非 Windows 跳过 / 非零失败（stderr 中文原因）。
//
// 时钟：轮换间隔吸附下限 300s，真等不可行；经 setClockForTest 注入虚拟时钟
// （tick 线程每 250ms 真实轮询一次 nowLocked()），推进虚拟时间即触发到点，
// 实证的是同一条「计时线程 → rotation.tick → loadfile」定时换片路径。
// 数据隔离：设置走 %TEMP% 独立 settings.json；探针不杀任何进程，mpv 由服务自启自收。
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
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

#include <png.h>

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

// 虚拟时钟：服务计时线程的 nowLocked() 全走这里；advance 只被探针主线程调用。
struct VirtualClock {
    std::mutex mutex;
    Clock::time_point base = Clock::now();
    Clock::duration offset{0};

    Clock::time_point now() {
        std::lock_guard<std::mutex> lock(mutex);
        return base + offset;
    }
    void advance(std::chrono::seconds delta) {
        std::lock_guard<std::mutex> lock(mutex);
        offset += delta;
    }
};

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

// ---- mpv IPC 第二客户端（与 wallpaper_rotation_probe 同款有界读写纪律） ----

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
                index += 4;
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

    // get_property 一次问答；false = 超时/通道死亡。data 为原始 JSON 片段
    //（数字文本或 "null"）或解转义字符串。
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

// 内嵌测试图改为探针运行期用 libpng 生成：2560×1440 = 3,686,400 px 恰好过默认
// 2K 档（3686400）——像素档过滤落地后，服务侧真实 WIC 头解码探针按「宽×高 ≥ 档位
// 线」放行，photo.png 才能进轮换池（64×64 会被过滤，池断言必红）。
constexpr int kProbePngWidth = 2560;
constexpr int kProbePngHeight = 1440;

bool writeProbePng(const std::filesystem::path& path) {
    std::vector<unsigned char> rgba(static_cast<std::size_t>(kProbePngWidth) *
                                        static_cast<std::size_t>(kProbePngHeight) * 4,
                                    0x30);  // 纯色深灰，mpv 真解码显示
    png_image image{};  // libpng 要求先整体清零再填字段（version 必设）
    image.version = PNG_IMAGE_VERSION;
    image.width = kProbePngWidth;
    image.height = kProbePngHeight;
    image.format = PNG_FORMAT_RGBA;
    const bool ok =
        png_image_write_to_file(&image, path.string().c_str(), 0, rgba.data(), 0, nullptr);
    if (!ok) {
        // message 是 png_image 内固定 char 数组（永不为 NULL），判空只看首字符。
        std::cerr << "wallpaper_image_rotation_probe: libpng 写测试图失败: "
                  << (image.message[0] != '\0' ? image.message : "unknown") << "\n";
    }
    png_image_free(&image);
    return ok;
}

#endif  // _WIN32

}  // namespace

int main(int argc, char* argv[]) {
#if !defined(_WIN32)
    (void)argc;
    (void)argv;
    std::cout << "wallpaper_image_rotation_probe: 非 Windows 平台，跳过（WorkerW/mpv 仅 Windows）"
              << " exit=" << kSkipExitCode << "\n";
    return kSkipExitCode;
#else
    (void)argc;
    (void)argv;

    // 探针独立进程：对齐 DevDesk 的 PerMonitorV2（同 wallpaper_rotation_probe）。
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
    if (!std::filesystem::is_regular_file(clipA)) {
        std::cerr << "wallpaper_image_rotation_probe: 测试视频不存在: " << fixtureDir.string()
                  << "\n";
        return 3;
    }

    // mpv 预检（服务内部同款解析优先级）；可执行名供进程盘点用。
    bool usedDevFallback = false;
    const std::string mpvPath = module::wallpaper::resolveMpvExecutable(
        envValue(module::wallpaper::kMpvPathEnvVar), core::paths::executableDir().string(),
        module::wallpaper::kMpvDevFallbackPath, true, &usedDevFallback);
    if (mpvPath.empty()) {
        std::cerr << "wallpaper_image_rotation_probe: 找不到 mpv.exe（DEVDESK_MPV_PATH / "
                     "<exeDir>/assets/mpv/mpv.exe / 开发兜底）\n";
        return 4;
    }
    const std::wstring mpvImageName =
        utf8ToWide(std::filesystem::path(mpvPath).filename().string());

    // ── §1 资源：%TEMP% 真文件夹（1 视频 + 内嵌合法 PNG + 干扰文本） ──
    const std::filesystem::path tempDir = std::filesystem::temp_directory_path() /
                                          ("devdesk-image-rotation-probe-" +
                                           std::to_string(GetCurrentProcessId()));
    {
        std::error_code ec;
        std::filesystem::remove_all(tempDir, ec);
    }
    std::error_code fsEc;
    std::filesystem::create_directories(tempDir, fsEc);
    const std::filesystem::path tempClipA = tempDir / "clip-a.mp4";
    const std::filesystem::path tempPng = tempDir / "photo.png";
    const std::filesystem::path tempNotes = tempDir / "notes.txt";
    std::filesystem::copy_file(clipA, tempClipA,
                               std::filesystem::copy_options::overwrite_existing, fsEc);
    writeProbePng(tempPng);
    {
        std::ofstream notes(tempNotes, std::ios::binary);
        for (int index = 0; index < 8; ++index) {
            notes << "devdesk image rotation probe decoy entry (not a media file).\n";
        }
    }
    const bool resourcesOk = std::filesystem::is_regular_file(tempClipA) &&
                             std::filesystem::file_size(tempPng, fsEc) > 0 &&
                             std::filesystem::is_regular_file(tempNotes);
    check(resourcesOk, "§1 资源：%TEMP% 真文件夹就绪（clip-a 拷贝 + libpng 生成 " +
                           std::to_string(kProbePngWidth) + "×" +
                           std::to_string(kProbePngHeight) + " PNG（恰过 2K 档）+ notes.txt " +
                           "干扰项）: " + tempDir.string());
    if (!resourcesOk) {
        std::error_code cleanupEc;
        std::filesystem::remove_all(tempDir, cleanupEc);
        return 6;
    }
    const unsigned long long pngBytesOnDisk =
        static_cast<unsigned long long>(std::filesystem::file_size(tempPng, fsEc));

    // ── §1 扫描语义（真扫描器直查：png 不进视频池 / mp4、txt 不进图片池） ──
    const module::wallpaper::VideoScanResult videoScan =
        module::wallpaper::scanVideoLibrary(tempDir);
    check(videoScan.status == module::wallpaper::LibraryScanStatus::Ok &&
              videoScan.files.size() == 1 && pathContains(videoScan.files[0], "clip-a.mp4") &&
              videoScan.extensionRejected == 2,
          "§1 扫描：视频池恰 1 支 clip-a（png/txt 被扩展名拒绝，实测拒 " +
              std::to_string(videoScan.extensionRejected) + "）");
    const module::wallpaper::ImageScanResult imageScan =
        module::wallpaper::scanImageLibrary(tempDir, 0);
    check(imageScan.status == module::wallpaper::LibraryScanStatus::Ok &&
              imageScan.files.size() == 1 && pathContains(imageScan.files[0], "photo.png") &&
              imageScan.total == 1 && imageScan.extensionRejected == 2,
          "§1 扫描：图片池恰 1 张 photo.png（" + std::to_string(pngBytesOnDisk) +
              "B 合法 PNG；mp4/txt 被拒，实测拒 " + std::to_string(imageScan.extensionRejected) +
              "）");

    // 盘点基线（服务启动前）与顶层窗口基线。
    const std::vector<std::uint32_t> mpvBaseline = pidsOfImageName(mpvImageName);
    const int windowsBefore = countOwnTopLevelWindows();

    // ── 服务：真装配 + 虚拟时钟（间隔下限 300s，真等不可行；推进虚拟时间触发同一条
    //    计时线程 → rotation.tick → loadfile 路径）。列表循环让到点必然换片。 ──
    VirtualClock virtualClock;
    module::wallpaper::WallpaperService service;
    service.setSettingsPathForTest((tempDir / "settings.json").string());
    service.setClockForTest([&virtualClock] { return virtualClock.now(); });
    service.setMode(module::wallpaper::PlayMode::LoopList);
    service.setRotationIntervalSec(300);  // 代码允许最小轮换档（kRotationChoiceSeconds[0]）
    std::string folderError;
    if (!service.selectFolder(tempDir.string(), &folderError)) {
        std::cerr << "wallpaper_image_rotation_probe: selectFolder 失败: " << folderError << "\n";
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

    // ── §2 混合池入库 ──
    const bool ingested = waitUntil(
        [&service]() {
            const module::wallpaper::WallpaperSnapshot snap = service.snapshot();
            return !snap.scanning &&
                   snap.availability == module::wallpaper::Availability::Playing &&
                   snap.poolCount == 2;
        },
        std::chrono::milliseconds(10000));
    const module::wallpaper::WallpaperSnapshot ingestedSnap = service.snapshot();
    check(ingested && ingestedSnap.poolCount == 2 && ingestedSnap.videoCount == 1 &&
              ingestedSnap.poolImageCount == 1,
          "§2 入库：混合池 poolCount==2（实测 " + std::to_string(ingestedSnap.poolCount) +
              " = 视频 " + std::to_string(ingestedSnap.videoCount) + " + 图片 " +
              std::to_string(ingestedSnap.poolImageCount) + "，可用性 Playing）");

    MpvIpcClient ipc;
    // 首次 spawn 的管道 counter=1（控制器 spawn 前 fetch_add），进程号 = 本探针进程。
    const std::string pipeName = module::wallpaper::makeMpvPipeName(GetCurrentProcessId(), 1);
    const bool pipeUp = ipc.connect(pipeName, std::chrono::milliseconds(10000));
    check(pipeUp, "§2 首播：mpv IPC 管道可连（" + pipeName + "）");
    if (!pipeUp || !ingested) {
        cleanup();
        return 8;
    }

    std::string pathNow;
    const bool firstIsClipA = waitUntil(
        [&]() {
            return ipc.getProperty("path", 3001, &pathNow, std::chrono::milliseconds(800)) &&
                   pathContains(pathNow, "clip-a.mp4");
        },
        std::chrono::milliseconds(8000));
    check(firstIsClipA, "§2 首播：池字典序首条为视频（IPC path 实测 \"" + pathNow + "\"）");
    check(!service.snapshot().currentItemImage,
          "§2 首播：快照 currentItemImage == false（视频项）");
    const std::vector<std::uint32_t> spawned =
        setDifference(pidsOfImageName(mpvImageName), mpvBaseline);
    check(spawned.size() == 1, "§2 首播：服务恰好 spawn 1 个 mpv（实测 " +
                                   std::to_string(spawned.size()) + " 个）");
    const std::uint32_t mpvPid = spawned.empty() ? 0 : spawned[0];
    if (!firstIsClipA || spawned.size() != 1) {
        cleanup();
        return 8;
    }

    // ── §3 定时轮换（决定性）：推进一个最小间隔，无任何手动 next，自动切到 PNG ──
    virtualClock.advance(std::chrono::seconds(310));
    const bool autoToImage = waitUntil(
        [&]() {
            return ipc.getProperty("path", 3002, &pathNow, std::chrono::milliseconds(800)) &&
                   pathContains(pathNow, "photo.png");
        },
        std::chrono::milliseconds(10000));
    check(autoToImage, "§3 定时轮换：interval(300s) 到点自动切到图片（IPC path 实测 \"" +
                           pathNow + "\"，全程未调 next()）");
    check(autoToImage && service.snapshot().currentItemImage,
          "§3 定时轮换：快照 currentItemImage == true（UI 图片态标记）");
    if (!autoToImage) {
        cleanup();
        return 9;
    }

    // ── §4 静图常驻：显示后实等 2.5s，path 不变且 mpv 存活（默认 1s 会自行切走） ──
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    std::string pathStill;
    const bool stillImage =
        ipc.getProperty("path", 3003, &pathStill, std::chrono::milliseconds(800)) &&
        pathContains(pathStill, "photo.png");
    check(stillImage && processStillRunning(mpvPid),
          "§4 静图常驻：2.5s 后仍是 photo.png 且 mpv 存活（--image-display-duration=inf，"
          "换片只由轮换驱动）");

    // ── §5 再等一个 interval 自动回到视频，再切回图片后验证图片态安全语义 ──
    virtualClock.advance(std::chrono::seconds(310));
    const bool autoBackToVideo = waitUntil(
        [&]() {
            return ipc.getProperty("path", 3004, &pathNow, std::chrono::milliseconds(800)) &&
                   pathContains(pathNow, "clip-a.mp4");
        },
        std::chrono::milliseconds(10000));
    check(autoBackToVideo, "§5 轮换闭环：再等一个 interval 自动回到 clip-a（IPC path 实测 \"" +
                               pathNow + "\"）");

    virtualClock.advance(std::chrono::seconds(310));
    const bool imageAgain = waitUntil(
        [&]() {
            return ipc.getProperty("path", 3005, &pathNow, std::chrono::milliseconds(800)) &&
                   pathContains(pathNow, "photo.png");
        },
        std::chrono::milliseconds(10000));
    if (!imageAgain) {
        check(false, "§5 图片态：重新轮换到 photo.png（IPC path 实测 \"" + pathNow + "\"）");
        cleanup();
        return 10;
    }

    // position 查询：图片态 time-pos 问答成功且为 0（不报错）。loadfile 起步阶段
    // mpv 短暂回 property unavailable，有界重试等装载完成；数值判据不放宽。
    std::string timePosText;
    int timePosRequest = 3006;
    const bool timePosQueried = waitUntil(
        [&]() {
            std::string text;
            if (!ipc.getProperty("time-pos", timePosRequest, &text,
                                 std::chrono::milliseconds(700))) {
                ++timePosRequest;
                return false;
            }
            timePosText = text;
            return true;
        },
        std::chrono::milliseconds(6000));
    const double timePosValue = std::strtod(timePosText.c_str(), nullptr);
    check(timePosQueried && timePosValue < 5.0,
          "§5 图片态：time-pos 查询成功且为 0（实测 \"" + timePosText + "\"，静图无播放位置）");

    // pause 对图片安全 no-op：手动暂停/恢复全程 PNG 不掉、mpv 存活。
    service.toggleManualPause();
    const bool pausedFlag = service.snapshot().paused;
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    std::string pathDuringPause;
    const bool imageHeld =
        ipc.getProperty("path", 3007, &pathDuringPause, std::chrono::milliseconds(800)) &&
        pathContains(pathDuringPause, "photo.png");
    check(pausedFlag && imageHeld && processStillRunning(mpvPid),
          "§5 图片态：手动暂停期间 photo.png 保持显示、mpv 存活（pause 为安全 no-op）");
    service.toggleManualPause();
    check(!service.snapshot().paused, "§5 图片态：恢复后 paused 解除（轮换计时重新排程）");
    if (!autoBackToVideo || !stillImage) {
        cleanup();
        return 10;
    }

    // ── §6 停止：service.stop < 3s，mpv 退出无残留，管道关，桌面还原 ──
    const auto stopStarted = Clock::now();
    service.stop();
    const double stopElapsed = std::chrono::duration<double>(Clock::now() - stopStarted).count();
    check(stopElapsed < 3.0, "§6 停止：service.stop() 耗时 " + std::to_string(stopElapsed) +
                                 "s < 3s（stop 梯子契约）");

    const bool exited = waitUntil([&]() { return !processStillRunning(mpvPid); },
                                  std::chrono::milliseconds(3000));
    check(exited && !processStillRunning(mpvPid),
          "§6 停止：mpv pid " + std::to_string(mpvPid) + " 已退出且无残留（≤3s）");

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
    check(pipeGone, "§6 停止：IPC 管道不可再连（服务端已随 mpv 退出）");

    const int windowsAfterStop = countOwnTopLevelWindows();
    check(windowsAfterStop == windowsBefore,
          "§6 停止：桌面还原（本进程顶层窗口 " + std::to_string(windowsAfterStop) +
              " == 启动前 " + std::to_string(windowsBefore) + "，宿主消息窗已销毁）");

    {
        std::error_code ec;
        std::filesystem::remove_all(tempDir, ec);
    }
    if (!passed) {
        std::cerr << "wallpaper_image_rotation_probe: FAIL\n";
        return 1;
    }
    std::cout << "wallpaper_image_rotation_probe: PASS\n";
    return 0;
#endif
}
