// src/module_wallpaper/platform/mpv_controller.cpp
// Windows mpv 控制线程实现：CreateProcess + 命名管道 JSON IPC + Job Object kill-on-close。
//
// 进程/管道/Job/读线程句柄全部归控制线程所有；stop() 由投递 Stop 命令的调用线程
// 同步等待完成信号（quit → 1.5s 优雅退出 → TerminateProcess → wait），返回即无残留。
// time-pos 事件由读线程写原子毫秒；其它线程只读。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "module_wallpaper/platform/mpv_controller.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "core/logging.h"
#include "core/paths.h"

namespace module::wallpaper {

namespace {

constexpr DWORD kCreateNoWindow = 0x08000000;
constexpr DWORD kStopGraceMs = 1500;
constexpr DWORD kTerminateWaitMs = 1000;
constexpr int kPipeConnectAttempts = 100;
constexpr int kPipeConnectIntervalMs = 20;

enum class CommandType { Start, Load, Pause, Seek, Rate, Volume, Mute, Stop };

struct StopSignal {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
};

struct Command {
    CommandType type = CommandType::Stop;
    std::uintptr_t windowId = 0;
    std::string path;
    double number = 0.0;   // Load: seek 秒（<0 = 不 seek）；Seek: 秒；Rate/Volume: 数值
    bool flag = false;     // Pause/Mute
    std::shared_ptr<StopSignal> stopSignal;
};

bool devFallbackAllowed() {
#ifdef NDEBUG
    return false;
#else
    return true;
#endif
}

std::string envString(const char* name) {
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

} // namespace

struct MpvController::Impl {
    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::deque<Command> queue;
    bool quit = false;
    std::thread worker;

    std::atomic<std::uint64_t> positionMs{0};
    std::atomic<bool> alive{false};
    std::atomic<std::uint32_t> pid{0};
    std::atomic<std::uint64_t> counter{0};

    std::mutex errorMutex;
    std::string error;

    // ---- 以下只在控制线程访问 ----
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE pipe = nullptr;
    std::thread reader;
    bool paused = true;
    double rate = 1.0;
    double volume = 1.0;
    bool muted = false;

    bool enqueue(Command command) {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (quit || !worker.joinable()) return false;
            queue.push_back(std::move(command));
        }
        queueCv.notify_all();
        return true;
    }

    void recordError(const std::string& message) {
        std::lock_guard<std::mutex> lock(errorMutex);
        error = message;
    }

    void run();
    bool spawn(std::uintptr_t windowId);
    void handleCommand(const Command& command);
    void shutdownProcess();
    bool sendLine(const std::string& line);
    void sendRaw(const std::string& line);
    void readLoop(HANDLE readPipe);
    bool running() const { return process != nullptr && pipe != nullptr; }
};

void MpvController::Impl::run() {
    for (;;) {
        Command command;
        bool hasCommand = false;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCv.wait(lock, [this] { return quit || !queue.empty(); });
            if (!queue.empty()) {
                command = std::move(queue.front());
                queue.pop_front();
                hasCommand = true;
            } else if (quit) {
                break;
            }
        }
        if (hasCommand) handleCommand(command);
    }
    shutdownProcess();
    positionMs.store(0, std::memory_order_relaxed);
    alive.store(false, std::memory_order_relaxed);
}

bool MpvController::Impl::sendLine(const std::string& line) {
    if (pipe == nullptr) return false;
    const std::string text = line + "\n";
    DWORD written = 0;
    if (::WriteFile(pipe, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) == 0 ||
        written != text.size()) {
        recordError("mpv IPC 写入失败（播放命令未送达）");
        return false;
    }
    return true;
}

void MpvController::Impl::sendRaw(const std::string& line) {
    if (pipe == nullptr) return;
    const std::string text = line + "\n";
    DWORD written = 0;
    (void)::WriteFile(pipe, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

void MpvController::Impl::readLoop(HANDLE readPipe) {
    std::string pending;
    char buffer[4096];
    for (;;) {
        DWORD read = 0;
        if (::ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) == 0 || read == 0) break;
        pending.append(buffer, read);
        std::size_t newline = 0;
        while ((newline = pending.find('\n')) != std::string::npos) {
            const std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            double seconds = 0.0;
            if (parseMpvTimePosEvent(line, &seconds)) {
                const double clamped = seconds > 0.0 ? seconds : 0.0;
                positionMs.store(static_cast<std::uint64_t>(clamped * 1000.0),
                                 std::memory_order_relaxed);
            }
        }
    }
    ::CloseHandle(readPipe);
}

void MpvController::Impl::shutdownProcess() {
    if (pipe != nullptr) sendRaw(mpvQuitCommand());
    if (process != nullptr) {
        if (::WaitForSingleObject(process, kStopGraceMs) != WAIT_OBJECT_0) {
            (void)::TerminateProcess(process, 1);
            (void)::WaitForSingleObject(process, kTerminateWaitMs);
        }
    }
    if (pipe != nullptr) {
        ::CloseHandle(pipe);
        pipe = nullptr;
    }
    if (reader.joinable()) reader.join();
    if (process != nullptr) {
        ::CloseHandle(process);
        process = nullptr;
    }
    if (job != nullptr) {
        // kill-on-close：即便还有子进程泄漏，关闭 Job 也会被内核杀掉。
        ::CloseHandle(job);
        job = nullptr;
    }
    alive.store(false, std::memory_order_relaxed);
}

bool MpvController::Impl::spawn(std::uintptr_t windowId) {
    shutdownProcess();
    counter.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t pipeCounter = counter.load(std::memory_order_relaxed);
    const std::string pipeName = makeMpvPipeName(::GetCurrentProcessId(), pipeCounter);

    bool usedDevFallback = false;
    const std::string mpvPath =
        resolveMpvExecutable(envString(kMpvPathEnvVar), core::paths::executableDir().string(),
                             kMpvDevFallbackPath, devFallbackAllowed(), &usedDevFallback);
    if (mpvPath.empty()) {
        recordError(
            "找不到 mpv.exe：请设置 DEVDESK_MPV_PATH，或把 mpv.exe 放到 "
            "<DevDesk 目录>/assets/mpv/ 下");
        return false;
    }
    if (usedDevFallback) {
        LOG_WARN("壁纸: 使用开发兜底 mpv 路径（仅非 Release 构建）: " + mpvPath);
    }

    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE nul = ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                               OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) nul = nullptr;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = nul;
    startup.hStdError = nul;
    startup.hStdInput = nullptr;

    PROCESS_INFORMATION info{};
    const std::wstring application = utf8ToWide(mpvPath);
    const std::wstring commandLine =
        utf8ToWide(buildWindowsCommandLine(mpvPath, buildMpvArguments(windowId, pipeName)));
    BOOL created = FALSE;
    if (!application.empty() && !commandLine.empty()) {
        std::vector<wchar_t> mutableLine(commandLine.begin(), commandLine.end());
        mutableLine.push_back(L'\0');
        created = ::CreateProcessW(application.c_str(), mutableLine.data(), nullptr, nullptr, TRUE,
                                   kCreateNoWindow, nullptr, nullptr, &startup, &info);
    }
    // CloseHandle 可能覆盖 GetLastError，失败码必须先抓。
    const DWORD createError = created != 0 ? 0 : ::GetLastError();
    if (nul != nullptr) ::CloseHandle(nul);
    if (created == 0) {
        recordError("启动 mpv 失败（Windows 错误码 " + std::to_string(createError) + "）");
        return false;
    }

    ::CloseHandle(info.hThread);
    process = info.hProcess;
    pid.store(info.dwProcessId, std::memory_order_relaxed);
    alive.store(true, std::memory_order_relaxed);
    (void)::SetPriorityClass(process, BELOW_NORMAL_PRIORITY_CLASS);

    job = ::CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                      sizeof(limits)) == 0) {
            ::CloseHandle(job);
            job = nullptr;
        } else if (::AssignProcessToJobObject(job, process) == 0) {
            LOG_WARN("壁纸: mpv 加入 Job Object 失败（仍用 quit/terminate 显式回收）");
        }
    }

    const std::wstring pipeWide = utf8ToWide(pipeName);
    HANDLE connected = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < kPipeConnectAttempts; ++attempt) {
        connected = ::CreateFileW(pipeWide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (connected != INVALID_HANDLE_VALUE) break;
        if (::WaitForSingleObject(process, 0) == WAIT_OBJECT_0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(kPipeConnectIntervalMs));
    }
    if (connected == INVALID_HANDLE_VALUE) {
        shutdownProcess();
        recordError("连接 mpv IPC 命名管道超时（mpv 未启动或无响应）");
        return false;
    }
    pipe = connected;

    HANDLE readPipe = INVALID_HANDLE_VALUE;
    if (::DuplicateHandle(::GetCurrentProcess(), pipe, ::GetCurrentProcess(), &readPipe, 0, FALSE,
                          DUPLICATE_SAME_ACCESS) == 0) {
        shutdownProcess();
        recordError("创建 mpv IPC 读取通道失败");
        return false;
    }
    try {
        reader = std::thread(&MpvController::Impl::readLoop, this, readPipe);
    } catch (const std::system_error& failure) {
        ::CloseHandle(readPipe);
        shutdownProcess();
        recordError(std::string("创建 mpv IPC 读取线程失败: ") + failure.what());
        return false;
    }

    if (!sendLine(mpvSetPropertyCommand("pause", paused))) {
        shutdownProcess();
        recordError("mpv IPC 初始化命令发送失败");
        return false;
    }
    (void)sendLine(mpvSetPropertyCommand("speed", rate));
    (void)sendLine(mpvSetPropertyCommand("volume", volume * 100.0));
    (void)sendLine(mpvSetPropertyCommand("mute", muted));
    (void)sendLine(mpvObservePropertyCommand(1, "time-pos"));
    return true;
}

void MpvController::Impl::handleCommand(const Command& command) {
    switch (command.type) {
        case CommandType::Start:
            (void)spawn(command.windowId);
            break;
        case CommandType::Load:
            if (!running()) break;
            positionMs.store(0, std::memory_order_relaxed);
            if (!sendLine(mpvLoadFileCommand(command.path))) break;
            (void)sendLine(mpvSetPropertyCommand("pause", paused));
            if (command.number >= 0.0) (void)sendLine(mpvSeekCommand(command.number));
            break;
        case CommandType::Pause:
            paused = command.flag;
            if (running()) (void)sendLine(mpvSetPropertyCommand("pause", paused));
            break;
        case CommandType::Seek:
            if (running()) (void)sendLine(mpvSeekCommand(command.number));
            break;
        case CommandType::Rate:
            rate = command.number;
            if (running()) (void)sendLine(mpvSetPropertyCommand("speed", rate));
            break;
        case CommandType::Volume:
            volume = command.number;
            if (running()) (void)sendLine(mpvSetPropertyCommand("volume", volume * 100.0));
            break;
        case CommandType::Mute:
            muted = command.flag;
            if (running()) (void)sendLine(mpvSetPropertyCommand("mute", muted));
            break;
        case CommandType::Stop:
            shutdownProcess();
            positionMs.store(0, std::memory_order_relaxed);
            if (command.stopSignal != nullptr) {
                {
                    std::lock_guard<std::mutex> lock(command.stopSignal->mutex);
                    command.stopSignal->done = true;
                }
                command.stopSignal->cv.notify_all();
            }
            break;
    }
}

MpvController::MpvController() : impl_(std::make_unique<Impl>()) {
    try {
        impl_->worker = std::thread(&MpvController::Impl::run, impl_.get());
    } catch (const std::system_error& failure) {
        impl_->recordError(std::string("创建 mpv 控制线程失败: ") + failure.what());
    }
}

MpvController::~MpvController() {
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lock(impl_->queueMutex);
        impl_->quit = true;
    }
    impl_->queueCv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}

void MpvController::start(std::uintptr_t windowId) {
    Command command;
    command.type = CommandType::Start;
    command.windowId = windowId;
    (void)impl_->enqueue(std::move(command));
}

void MpvController::stop() {
    if (!impl_) return;
    auto signal = std::make_shared<StopSignal>();
    Command command;
    command.type = CommandType::Stop;
    command.stopSignal = signal;
    if (!impl_->enqueue(std::move(command))) return;
    std::unique_lock<std::mutex> lock(signal->mutex);
    (void)signal->cv.wait_for(lock, std::chrono::seconds(5), [&signal] { return signal->done; });
}

void MpvController::load(const std::string& path, float seekSeconds) {
    Command command;
    command.type = CommandType::Load;
    command.path = path;
    command.number = static_cast<double>(seekSeconds);
    (void)impl_->enqueue(std::move(command));
}

void MpvController::pause(bool paused) {
    Command command;
    command.type = CommandType::Pause;
    command.flag = paused;
    (void)impl_->enqueue(std::move(command));
}

void MpvController::seek(float seconds) {
    Command command;
    command.type = CommandType::Seek;
    command.number = static_cast<double>(seconds);
    (void)impl_->enqueue(std::move(command));
}

void MpvController::setRate(float rate) {
    Command command;
    command.type = CommandType::Rate;
    command.number = static_cast<double>(rate);
    (void)impl_->enqueue(std::move(command));
}

void MpvController::setVolume(float volume) {
    Command command;
    command.type = CommandType::Volume;
    command.number = static_cast<double>(volume);
    (void)impl_->enqueue(std::move(command));
}

void MpvController::setMute(bool muted) {
    Command command;
    command.type = CommandType::Mute;
    command.flag = muted;
    (void)impl_->enqueue(std::move(command));
}

float MpvController::positionSec() const {
    if (!impl_) return 0.0f;
    return static_cast<float>(impl_->positionMs.load(std::memory_order_relaxed)) / 1000.0f;
}

bool MpvController::processAlive() const {
    return impl_ != nullptr && impl_->alive.load(std::memory_order_relaxed);
}

std::uint32_t MpvController::processId() const {
    return impl_ != nullptr ? impl_->pid.load(std::memory_order_relaxed) : 0;
}

std::string MpvController::takeError() {
    if (!impl_) return std::string();
    std::lock_guard<std::mutex> lock(impl_->errorMutex);
    std::string out = impl_->error;
    impl_->error.clear();
    return out;
}

} // namespace module::wallpaper