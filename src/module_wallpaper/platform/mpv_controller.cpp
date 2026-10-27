// src/module_wallpaper/platform/mpv_controller.cpp
// Windows mpv 控制线程实现：CreateProcess + 命名管道 JSON IPC（OVERLAPPED）+ Job Object kill-on-close。
//
// 死锁防线契约（改动前必读）：
//   - mpv 对每条 IPC 命令回一行 JSON 回执，事件持续推送；读线程必须常驻排空，
//     否则 mpv 出站缓冲积压、其 IPC 线程阻塞反压通道，我方写命令永久阻塞——只写不读 = 死锁。
//   - 管道按 FILE_FLAG_OVERLAPPED 打开：同步句柄的悬挂读会在同一文件对象上把并发写
//     一起串行卡死；因此读写两端都必须走 OVERLAPPED + 事件等待，写等待 ≤1s 有界。
//   - 写超时判定 IPC 通道死亡：就地 TerminateProcess 收尾 + recordError，控制线程绝不永久阻塞。
//
// 进程/管道/Job/读线程句柄全部归控制线程所有；stop() 由投递 Stop 命令的调用线程
// 同步等待完成信号：quit（有界写）→ 800ms 宽限 → TerminateProcess → 500ms →
// 清管道/读线程/句柄，总时延 < 3s，返回即无残留。
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
#include <cstdlib>
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
constexpr DWORD kIpcWriteTimeoutMs = 1000;     // 单条 IPC 写等待上限；超时判通道死亡
constexpr DWORD kIpcQuitWriteTimeoutMs = 300;  // quit 兜底写上限，保 stop 梯子总时延 < 3s
constexpr DWORD kQuitGraceMs = 800;            // quit 后的优雅退出宽限
constexpr DWORD kTerminateWaitMs = 500;        // TerminateProcess 后的退出等待
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
    HANDLE readHandle = nullptr;  // 读端重复句柄：控制线程 CancelIoEx/Close 用；读线程只用值拷贝、绝不关闭
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
    bool writeBounded(const std::string& line, DWORD timeoutMs, bool recordFailure);
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

// OVERLAPPED 写 + 有界事件等待。契约：
//   - 只允许控制线程调用（pipe 上同时至多一个挂起写，ov/缓冲在 IRP 排空前不出作用域）。
//   - 等待超过 timeoutMs 判 IPC 通道死亡：就地 TerminateProcess + 撤销并排空挂起 IRP，
//     控制线程有界返回；返回 false 后不得再信任该通道（后续写会以断管道快速失败）。
//   - recordFailure=false 用于 stop 兜底写：失败不刷错误槽，避免正常收尾留脏错误。
bool MpvController::Impl::writeBounded(const std::string& line, DWORD timeoutMs,
                                       bool recordFailure) {
    if (pipe == nullptr) return false;
    const std::string text = line + "\n";
    OVERLAPPED overlapped{};
    overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
        if (recordFailure) recordError("mpv IPC 写事件创建失败");
        return false;
    }
    const BOOL queued =
        ::WriteFile(pipe, text.data(), static_cast<DWORD>(text.size()), nullptr, &overlapped);
    bool ok = false;
    if (queued == 0 && ::GetLastError() != ERROR_IO_PENDING) {
        if (recordFailure) recordError("mpv IPC 写入失败（通道已断开）");
    } else if (::WaitForSingleObject(overlapped.hEvent, timeoutMs) == WAIT_OBJECT_0) {
        DWORD written = 0;
        ok = ::GetOverlappedResult(pipe, &overlapped, &written, FALSE) != 0 &&
             written == static_cast<DWORD>(text.size());
        if (!ok && recordFailure) recordError("mpv IPC 写入未完成（通道异常）");
    } else {
        // 写超时 = 通道死亡：先强杀再排空 IRP，ov/缓冲出作用域前必须无挂起引用。
        if (process != nullptr) (void)::TerminateProcess(process, 1);
        (void)::CancelIoEx(pipe, &overlapped);
        DWORD dropped = 0;
        (void)::GetOverlappedResult(pipe, &overlapped, &dropped, TRUE);
        if (recordFailure) recordError("mpv IPC 写入超时（通道死亡，已强杀 mpv）");
    }
    ::CloseHandle(overlapped.hEvent);
    return ok;
}

bool MpvController::Impl::sendLine(const std::string& line) {
    return writeBounded(line, kIpcWriteTimeoutMs, true);
}

void MpvController::Impl::sendRaw(const std::string& line) {
    (void)writeBounded(line, kIpcQuitWriteTimeoutMs, false);
}

// 读线程常驻排空 mpv 的回执与事件（含 time-pos → 原子毫秒）。契约：
//   - readPipe 归控制线程所有，本线程绝不 CloseHandle；退出只靠管道断开或控制线程
//     CancelIoEx 打断挂起 IRP（收尾先 Terminate 再 cancel+join），保证 join 有界。
void MpvController::Impl::readLoop(HANDLE readPipe) {
    std::string pending;
    char buffer[4096];
    HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) return;
    for (;;) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = event;
        const BOOL queued = ::ReadFile(readPipe, buffer, sizeof(buffer), nullptr, &overlapped);
        if (queued == 0 && ::GetLastError() != ERROR_IO_PENDING) break;
        DWORD got = 0;
        if (::GetOverlappedResult(readPipe, &overlapped, &got, TRUE) == 0 || got == 0) break;
        pending.append(buffer, got);
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
    ::CloseHandle(event);
}

// stop 梯子（总时延 < 3s）：quit 有界写（≤300ms）→ 800ms 宽限 → TerminateProcess →
// 500ms → 撤销挂起 IRP → join 读线程 → 清句柄。顺序不变量：先让进程死透/管道断开，
// 再撤 IRP + join，读线程等待必然有界返回；控制线程在此函数内不出现无界等待。
void MpvController::Impl::shutdownProcess() {
    if (pipe != nullptr) sendRaw(mpvQuitCommand());
    if (process != nullptr) {
        if (::WaitForSingleObject(process, kQuitGraceMs) != WAIT_OBJECT_0) {
            (void)::TerminateProcess(process, 1);
            (void)::WaitForSingleObject(process, kTerminateWaitMs);
        }
    }
    if (pipe != nullptr) (void)::CancelIoEx(pipe, nullptr);
    if (readHandle != nullptr) (void)::CancelIoEx(readHandle, nullptr);
    if (reader.joinable()) reader.join();
    if (readHandle != nullptr) {
        ::CloseHandle(readHandle);
        readHandle = nullptr;
    }
    if (pipe != nullptr) {
        ::CloseHandle(pipe);
        pipe = nullptr;
    }
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
        // FILE_FLAG_OVERLAPPED 是死锁防线的根：同步句柄上悬挂读会把并发写串行卡死。
        connected = ::CreateFileW(pipeWide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
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
    readHandle = readPipe;
    try {
        reader = std::thread(&MpvController::Impl::readLoop, this, readPipe);
    } catch (const std::system_error& failure) {
        readHandle = nullptr;
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