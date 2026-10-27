// src/module_wallpaper/platform/wallpaper_host_win.cpp
// Win32 桌面宿主：隐藏顶层消息窗 + WorkerW 父窗口 + 宿主/视频/图片子窗口。
//
// 线程模型：
//   - start() 起唯一宿主线程；窗口类注册、WorkerW 查找、CreateWindowEx、DestroyWindow
//     全部只在该线程执行（Win32 窗口有线程亲和性）。
//   - show/hide/rebuild 从任意线程投递命令并同步等待结果（有界超时）；handle() 只读缓存。
//   - stop() 置停止标志、唤醒命令队列、join 宿主线程（线程尾负责销毁全部窗口）。
//
// 尺寸单位：主屏 rcMonitor 物理像素；不做进程 DPI 感知设置（DevDesk/EUI 已设 PerMonitorV2）。
// 图标层可点：宿主 WS_EX_NOACTIVATE | WS_EX_TRANSPARENT，并置于 HWND_BOTTOM。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "module_wallpaper/platform/wallpaper_host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "core/logging.h"

namespace module::wallpaper {
namespace {

constexpr wchar_t kHostClass[] = L"DevDeskWallpaperHost";
constexpr wchar_t kVideoClass[] = L"DevDeskWallpaperVideo";
constexpr wchar_t kImageClass[] = L"DevDeskWallpaperImage";
constexpr wchar_t kMessageClass[] = L"DevDeskWallpaperMessage";
// Progman 私有消息：让 Explorer 在图标层之后生成 WorkerW（经典 Win10 路径）。
constexpr UINT kSpawnWorkerMessage = 0x052C;
constexpr DWORD kControlTimeoutMs = 3000;
constexpr DWORD kStartTimeoutMs = 5000;

void paintBlack(HWND window) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT rect{};
    GetClientRect(window, &rect);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    EndPaint(window, &paint);
}

LRESULT CALLBACK hostProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ERASEBKGND) return 1;  // 自绘黑底，避免闪烁
    if (message == WM_PAINT) {
        paintBlack(window);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK imageProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        paintBlack(window);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

// 视频层交给 mpv 直接渲染（--wid 嵌入），不拦截 WM_PAINT/WM_ERASEBKGND。
LRESULT CALLBACK videoProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

// 隐藏顶层消息窗（非 message-only）：3b 的 watcher 要收 TaskbarCreated/广播消息。
LRESULT CALLBACK messageProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

bool ensureWindowClasses(HINSTANCE instance, std::string* error) {
    struct ClassSpec {
        const wchar_t* name;
        WNDPROC proc;
    };
    const ClassSpec specs[] = {
        {kMessageClass, messageProc},
        {kHostClass, hostProc},
        {kVideoClass, videoProc},
        {kImageClass, imageProc},
    };
    for (const ClassSpec& spec : specs) {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = spec.proc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = spec.name;
        if (RegisterClassW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            *error = "注册壁纸窗口类失败";
            return false;
        }
    }
    return true;
}

bool primaryMonitorRect(RECT* rect, std::string* error) {
    POINT origin{0, 0};
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    if (monitor == nullptr) {
        *error = "找不到主显示器";
        return false;
    }
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        *error = "读取主显示器尺寸失败";
        return false;
    }
    *rect = info.rcMonitor;
    return true;
}

BOOL CALLBACK findClassicWorkerW(HWND window, LPARAM param) {
    if (FindWindowExW(window, nullptr, L"SHELLDLL_DefView", nullptr) == nullptr) return TRUE;
    // 经典布局：DefView 所在顶层窗之后的同级 WorkerW 才是图标层背后那层。
    HWND worker = FindWindowExW(nullptr, window, L"WorkerW", nullptr);
    if (worker == nullptr) return TRUE;
    *reinterpret_cast<HWND*>(param) = worker;
    return FALSE;
}

// 优先级：Progman 直属 WorkerW（Win11）→ 经典顶层 WorkerW → Progman 本身（兜底仍位于图标层后）。
HWND findWorkerW() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman == nullptr) return nullptr;
    DWORD_PTR result = 0;
    SendMessageTimeoutW(progman, kSpawnWorkerMessage, 0xD, 0x1, SMTO_NORMAL, 1000, &result);

    HWND child = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
    if (child != nullptr) return child;

    HWND classic = nullptr;
    EnumWindows(&findClassicWorkerW, reinterpret_cast<LPARAM>(&classic));
    if (classic != nullptr) return classic;
    return progman;
}

struct CommandResult {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    std::string error;
};

class WallpaperHostWin final : public WallpaperHost {
public:
    WallpaperHostWin() = default;
    ~WallpaperHostWin() override { stop(); }

    bool start(std::string* error) override {
        std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
        if (thread_.joinable()) return true;
        {
            std::lock_guard<std::mutex> lock(initMutex_);
            initDone_ = false;
            initOk_ = false;
            initError_.clear();
        }
        stopRequested_.store(false);
        try {
            thread_ = std::thread(&WallpaperHostWin::threadMain, this);
        } catch (const std::system_error& failure) {
            if (error != nullptr) {
                *error = std::string("创建壁纸宿主线程失败: ") + failure.what();
            }
            return false;
        }

        std::string initError;
        bool ok = false;
        {
            std::unique_lock<std::mutex> lock(initMutex_);
            initCv_.wait_for(lock, std::chrono::milliseconds(kStartTimeoutMs),
                             [this] { return initDone_; });
            ok = initDone_ && initOk_;
            initError = initError_;
        }
        if (ok) return true;

        // 初始化失败或超时：收线程（宿主线程尾会销毁已建的一半资源）。
        stopRequested_.store(true);
        queueCv_.notify_all();
        if (thread_.joinable()) thread_.join();
        if (error != nullptr) {
            *error = initError.empty() ? "壁纸宿主初始化超时" : initError;
        }
        return false;
    }

    void stop() override {
        std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
        if (!thread_.joinable()) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            handle_ = HostHandle{};
            return;
        }
        stopRequested_.store(true);
        queueCv_.notify_all();
        thread_.join();
        std::lock_guard<std::mutex> lock(stateMutex_);
        handle_ = HostHandle{};
    }

    bool show(std::string* error) override { return postAndWait(CommandType::Show, error); }

    void hide() override { (void)postAndWait(CommandType::Hide, nullptr); }

    HostHandle handle() const override {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return handle_;
    }

    void rebuild(std::string* error) override {
        (void)postAndWait(CommandType::Rebuild, error);
    }

private:
    enum class CommandType { Show, Hide, Rebuild };

    struct Command {
        CommandType type = CommandType::Show;
        std::shared_ptr<CommandResult> result;
    };

    void threadMain() {
        std::string error;
        const bool ok = initialize(&error);
        {
            std::lock_guard<std::mutex> lock(initMutex_);
            initDone_ = true;
            initOk_ = ok;
            initError_ = error;
        }
        initCv_.notify_all();
        if (!ok) {
            // 初始化可能已建出消息窗/宿主窗口的一半：必须仍在宿主线程销毁。
            destroyWindows();
            return;
        }

        while (true) {
            Command command;
            bool hasCommand = false;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueCv_.wait_for(lock, std::chrono::milliseconds(20), [this] {
                    return stopRequested_.load() || !queue_.empty();
                });
                if (stopRequested_.load()) break;
                if (!queue_.empty()) {
                    command = std::move(queue_.front());
                    queue_.pop_front();
                    hasCommand = true;
                }
            }
            if (hasCommand) execute(command);
            pumpMessages();
        }
        destroyWindows();
        failPendingCommands();
    }

    bool initialize(std::string* error) {
        HINSTANCE instance = GetModuleHandleW(nullptr);
        if (!ensureWindowClasses(instance, error)) return false;

        messageWindow_ = CreateWindowExW(WS_EX_TOOLWINDOW, kMessageClass,
                                         L"DevDeskWallpaperMessage", WS_POPUP, 0, 0, 0, 0,
                                         nullptr, nullptr, instance, nullptr);
        if (messageWindow_ == nullptr) {
            *error = "创建壁纸消息窗失败";
            return false;
        }
        if (!createHostWindows(error)) return false;
        // 先隐藏：扫描出可播条目后由 show() 显示（启动即 hold 时不得闪现）。
        ShowWindow(hostWindow_, SW_HIDE);
        return true;
    }

    bool createHostWindows(std::string* error) {
        HWND worker = findWorkerW();
        if (worker == nullptr) {
            *error = "找不到桌面 WorkerW 窗口（Progman 不可用）";
            return false;
        }
        RECT rect{};
        if (!primaryMonitorRect(&rect, error)) return false;
        const int width = std::max<int>(1, rect.right - rect.left);
        const int height = std::max<int>(1, rect.bottom - rect.top);
        HINSTANCE instance = GetModuleHandleW(nullptr);

        hostWindow_ = CreateWindowExW(
            WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, kHostClass, L"DevDeskWallpaperHost",
            WS_CHILD | WS_VISIBLE, rect.left, rect.top, width, height, worker, nullptr, instance,
            nullptr);
        if (hostWindow_ == nullptr) {
            *error = "创建壁纸宿主窗口失败";
            return false;
        }
        videoWindow_ = CreateWindowExW(0, kVideoClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, width,
                                       height, hostWindow_, nullptr, instance, nullptr);
        if (videoWindow_ == nullptr) {
            destroyHostWindows();
            *error = "创建视频层窗口失败";
            return false;
        }
        imageWindow_ = CreateWindowExW(0, kImageClass, L"", WS_CHILD, 0, 0, width, height,
                                       hostWindow_, nullptr, instance, nullptr);
        if (imageWindow_ == nullptr) {
            destroyHostWindows();
            *error = "创建图片层窗口失败";
            return false;
        }
        SetWindowPos(hostWindow_, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        updateHandle(width, height);
        return true;
    }

    void destroyHostWindows() {
        if (videoWindow_ != nullptr) {
            DestroyWindow(videoWindow_);
            videoWindow_ = nullptr;
        }
        if (imageWindow_ != nullptr) {
            DestroyWindow(imageWindow_);
            imageWindow_ = nullptr;
        }
        if (hostWindow_ != nullptr) {
            DestroyWindow(hostWindow_);
            hostWindow_ = nullptr;
        }
        std::lock_guard<std::mutex> lock(stateMutex_);
        handle_ = HostHandle{};
    }

    void destroyWindows() {
        destroyHostWindows();
        if (messageWindow_ != nullptr) {
            DestroyWindow(messageWindow_);
            messageWindow_ = nullptr;
        }
    }

    void updateHandle(int width, int height) {
        HostHandle next;
        next.videoChild = static_cast<void*>(videoWindow_);
        next.imageChild = static_cast<void*>(imageWindow_);
        next.width = width;
        next.height = height;
        std::lock_guard<std::mutex> lock(stateMutex_);
        handle_ = next;
    }

    void pumpMessages() {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    void execute(Command& command) {
        switch (command.type) {
            case CommandType::Show: {
                if (hostWindow_ == nullptr) {
                    finish(command, false, "壁纸宿主窗口不存在");
                    break;
                }
                ShowWindow(hostWindow_, SW_SHOW);
                SetWindowPos(hostWindow_, HWND_BOTTOM, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                finish(command, true, std::string());
                break;
            }
            case CommandType::Hide: {
                if (hostWindow_ != nullptr) ShowWindow(hostWindow_, SW_HIDE);
                finish(command, true, std::string());
                break;
            }
            case CommandType::Rebuild: {
                const bool wasVisible = hostWindow_ != nullptr && IsWindowVisible(hostWindow_) != 0;
                destroyHostWindows();
                std::string error;
                const bool ok = createHostWindows(&error);
                if (ok) {
                    if (wasVisible) ShowWindow(hostWindow_, SW_SHOW);
                    SetWindowPos(hostWindow_, HWND_BOTTOM, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
                finish(command, ok, error);
                break;
            }
        }
    }

    void finish(Command& command, bool ok, std::string error) {
        if (command.result == nullptr) return;
        {
            std::lock_guard<std::mutex> lock(command.result->mutex);
            command.result->ok = ok;
            command.result->error = std::move(error);
            command.result->done = true;
        }
        command.result->cv.notify_all();
    }

    void failPendingCommands() {
        std::deque<Command> pending;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            pending.swap(queue_);
        }
        for (Command& command : pending) {
            finish(command, false, "壁纸宿主已停止");
        }
    }

    bool postAndWait(CommandType type, std::string* error) {
        std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
        if (!thread_.joinable() || stopRequested_.load()) {
            if (error != nullptr) *error = "壁纸宿主未启动";
            return false;
        }
        auto result = std::make_shared<CommandResult>();
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            Command command;
            command.type = type;
            command.result = result;
            queue_.push_back(std::move(command));
        }
        queueCv_.notify_all();

        std::unique_lock<std::mutex> lock(result->mutex);
        if (!result->cv.wait_for(lock, std::chrono::milliseconds(kControlTimeoutMs),
                                 [&result] { return result->done; })) {
            if (error != nullptr) *error = "壁纸宿主线程无响应";
            return false;
        }
        if (!result->ok && error != nullptr) *error = result->error;
        return result->ok;
    }

    std::mutex lifecycleMutex_;  // 串行化 start/stop/show/hide/rebuild 的生命周期判断
    std::thread thread_;
    std::atomic<bool> stopRequested_{false};

    std::mutex initMutex_;
    std::condition_variable initCv_;
    bool initDone_ = false;
    bool initOk_ = false;
    std::string initError_;

    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<Command> queue_;

    mutable std::mutex stateMutex_;
    HostHandle handle_;

    // 以下句柄只在宿主线程读写。
    HWND messageWindow_ = nullptr;
    HWND hostWindow_ = nullptr;
    HWND videoWindow_ = nullptr;
    HWND imageWindow_ = nullptr;
};

} // namespace

std::unique_ptr<WallpaperHost> createWallpaperHost() {
    return std::make_unique<WallpaperHostWin>();
}

} // namespace module::wallpaper