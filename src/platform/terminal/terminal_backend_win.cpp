// src/platform/terminal/terminal_backend_win.cpp — Windows ConPTY 真实现
// （need:127 Phase 1，技术卡 v2 §3.1/§3.2；v1 占位 TU 退役）。
// 平台归属：本 TU 仅 WIN32 编译（本机 macOS 无法构建/运行——编译绿归 CI，运行归真机）。
// ConPTY 已知坑①-⑦对策落点（Q1 坑表）：
//   坑① reader/writer 各自独立线程服务两管道（§readerLoop/writerLoop）；
//   坑② 不启用 INHERIT_CURSOR（光标查询死锁面整体消除）；关闭=树杀→有界排空至断裂
//        →Close（无读取在飞窗口）；坑③ 关闭前等 spawnCompleted（子进程未起完不碰
//        ClosePseudoConsole）；坑④ CreateProcess 成功立即关传入侧句柄；
//   坑⑤ resize 在无读取在飞窗口内同步调 + CancelIoEx 唤醒阻塞读（idle 不冻结）；
//   坑⑥ 老 conhost 吞吐=PRD 已入账残留风险；坑⑦ 管道断裂→exited（会话结束态，宿主不崩）。
// ConPTY 三函数走 GetProcAddress 动态解析（typedef+GetModuleHandleW）——老 OS 缺导出时
// available()=false 给明确文案，不做导入期链接（导入缺失会让进程载入直接失败）。
#include "terminal_backend.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "process_host.h"
#include "terminal_vterm_core.h"

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

namespace platform {

namespace {

// ---- ConPTY API 动态解析（老 OS 探测：任一缺失即不支持）----

using FnCreatePseudoConsole = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
using FnResizePseudoConsole = HRESULT(WINAPI*)(HPCON, COORD);
using FnClosePseudoConsole = void(WINAPI*)(HPCON);

struct ConPtyApi {
    FnCreatePseudoConsole create = nullptr;
    FnResizePseudoConsole resize = nullptr;
    FnClosePseudoConsole close = nullptr;
    bool ok = false;
};

const ConPtyApi& conptyApi() {
    static const ConPtyApi api = [] {
        ConPtyApi a;
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        if (k32 == nullptr) return a;
        a.create = reinterpret_cast<FnCreatePseudoConsole>(
            reinterpret_cast<void*>(GetProcAddress(k32, "CreatePseudoConsole")));
        a.resize = reinterpret_cast<FnResizePseudoConsole>(
            reinterpret_cast<void*>(GetProcAddress(k32, "ResizePseudoConsole")));
        a.close = reinterpret_cast<FnClosePseudoConsole>(
            reinterpret_cast<void*>(GetProcAddress(k32, "ClosePseudoConsole")));
        a.ok = a.create != nullptr && a.resize != nullptr && a.close != nullptr;
        return a;
    }();
    return api;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

std::wstring quoted(const std::wstring& path) { return L"\"" + path + L"\""; }

// 默认 shell：DEVDESK_TERMINAL_SHELL（失败注入口，与 mac 对齐）→ pwsh.exe →
// powershell.exe → %COMSPEC%（技术卡决策 3）。TERM 不注入（Windows Terminal 同口径，
// SSH terminfo 差异记 F-05 已知差异清单）。
std::wstring resolveShellCommandLine() {
    if (const char* injected = std::getenv("DEVDESK_TERMINAL_SHELL")) {
        if (injected[0] != '\0') return utf8ToWide(injected);
    }
    wchar_t path[MAX_PATH];
    if (SearchPathW(nullptr, L"pwsh.exe", nullptr, MAX_PATH, path, nullptr) > 0)
        return quoted(path) + L" -NoLogo";
    if (SearchPathW(nullptr, L"powershell.exe", nullptr, MAX_PATH, path, nullptr) > 0)
        return quoted(path) + L" -NoLogo";
    wchar_t comspec[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"COMSPEC", comspec, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return quoted(comspec);
    return L"cmd.exe";
}

// 有界关闭等待（技术卡 §3.2：排空/Close/join 全链有界，先例口径 5s）
constexpr DWORD kCloseWaitMs = 5000;

} // namespace

class WinSessionHandle : public TerminalSessionHandle {
public:
    WinSessionHandle(HPCON hpc, HANDLE inputWrite, HANDLE outputRead, HANDLE hProcess,
                     int64_t pid, std::unique_ptr<VtermCore> vterm)
        : hpc_(hpc),
          inputWrite_(inputWrite),
          outputRead_(outputRead),
          hProcess_(hProcess),
          pid_(pid),
          vterm_(std::move(vterm)) {
        reader_ = std::thread([this] { readerLoop(); });
        writer_ = std::thread([this] { writerLoop(); });
    }

    ~WinSessionHandle() override { terminate(); }

    WinSessionHandle(const WinSessionHandle&) = delete;
    WinSessionHandle& operator=(const WinSessionHandle&) = delete;

    core::Result<void> write(const uint8_t* bytes, int32_t count, bool paste) override {
        if (bytes == nullptr || count <= 0)
            return core::Result<void>::fail(core::ErrorCode::ValidationError, "空写入");
        std::string payload(reinterpret_cast<const char*>(bytes), size_t(count));
        // bracketed paste：仅在子进程已开启 DECSET 2004 时包裹（转译层平行跟踪）
        if (paste && vterm_ && vterm_->bracketedPasteEnabled())
            payload = "\x1b[200~" + payload + "\x1b[201~";
        {
            std::lock_guard<std::mutex> lock(wqMutex_);
            if (stopping_.load(std::memory_order_relaxed))
                return core::Result<void>::fail(core::ErrorCode::Internal, "会话已终止");
            wq_.push_back(std::move(payload));
        }
        wqCv_.notify_one();
        return core::Result<void>::ok();
    }

    core::Result<void> resize(const TerminalSize& size) override {
        if (size.rows <= 0 || size.cols <= 0)
            return core::Result<void>::fail(core::ErrorCode::ValidationError, "非法尺寸");
        {
            std::lock_guard<std::mutex> lock(resizeMutex_);
            pendingSize_ = size;
            resizePending_.store(true, std::memory_order_relaxed);
        }
        // 坑⑤ idle 唤醒：reader 阻塞在无输出的 ReadFile 时 pending resize 永不处理
        //（shell 不重绘→无输出→reader 不醒），CancelIoEx 打断阻塞读后由 reader
        // 在无读取在飞窗口处理。
        if (outputRead_ != nullptr) CancelIoEx(outputRead_, nullptr);
        return core::Result<void>::ok();
    }

    // 关闭时序（技术卡 §3.2，全链有界无挂死面）：
    // ① 等 spawnCompleted（坑③）→ ② taskkill /T /F 树杀（process_host 先例，快照式，
    // 竞态逃逸由下一步兜底）→ ③ 有界等 reader 自然排空至管道断裂（坑②）→ 超时
    // CancelIoEx 强断 → ④ ClosePseudoConsole（此时无读取在飞）→ ⑤ 有界收 writer →
    // ⑥ 关全部句柄。任何一步超时都不挂死 UI。
    void terminate() override {
        bool already = terminated_.exchange(true);
        if (already) return;

        if (spawnCompleted_.wait_for(std::chrono::milliseconds(kCloseWaitMs))) {
            // ② 树杀：先例 process_host_win.cpp:172-174（taskkill /T /F + 有界 + 探活兜底）
            ::platform::terminateTree(pid_);
        }

        // ③ 有界等 reader 排空至断裂
        if (!readerDone_.wait_for(std::chrono::milliseconds(kCloseWaitMs))) {
            if (outputRead_ != nullptr) CancelIoEx(outputRead_, nullptr);
            readerDone_.wait_for(std::chrono::milliseconds(1000)); // 强断兜底窗口
        }

        // ④ Close（读侧已停=无读取在飞；超时残余=句柄拆除兜底口径，宁拆不挂）
        {
            std::lock_guard<std::mutex> lock(apiMutex_);
            if (hpc_ != nullptr && conptyApi().ok) conptyApi().close(hpc_);
            hpc_ = nullptr;
        }

        // ⑤ 收 writer（线程收尾硬纪律：joinable 线程必须 join 或 detach，否则析构 std::terminate）
        stopping_.store(true, std::memory_order_relaxed);
        wqCv_.notify_all();
        if (!writerDone_.wait_for(std::chrono::milliseconds(1000))) { /* 超时走 detach 兜底 */ }
        if (writer_.joinable()) {
            if (writerDone_.peek())
                writer_.join();
            else
                writer_.detach(); // 句柄拆除后 WriteFile 必败，线程随即自然退出
        }
        if (reader_.joinable()) {
            if (readerDoneEpoch_.load() != 0)
                reader_.join(); // 已自然退出（排空至断裂或强断兜底后）
            else
                reader_.detach(); // 超时兜底：句柄拆除后 ReadFile 必败退出
        }

        // ⑥ 关句柄
        if (inputWrite_ != nullptr) {
            CloseHandle(inputWrite_);
            inputWrite_ = nullptr;
        }
        if (outputRead_ != nullptr) {
            CloseHandle(outputRead_);
            outputRead_ = nullptr;
        }
        if (hProcess_ != nullptr) {
            CloseHandle(hProcess_);
            hProcess_ = nullptr;
        }
        exited_.store(true, std::memory_order_relaxed);
    }

    bool exited(int32_t* exitCode) override {
        if (!exited_.load(std::memory_order_relaxed)) return false;
        if (exitCode != nullptr) *exitCode = exitCode_.load(std::memory_order_relaxed);
        return true;
    }

    TerminalFrame poll() override { return vterm_ ? vterm_->snapshot() : TerminalFrame{}; }

    int pendingGeneration() override { return vterm_ ? vterm_->pendingGeneration() : 0; }

    int scrollbackCount() override { return vterm_ ? vterm_->scrollbackCount() : 0; }

    bool scrollbackLine(int index, std::string& out) override {
        return vterm_ ? vterm_->scrollbackLine(index, out) : false;
    }

    bool selectionText(int rowStart, int colStart, int rowEnd, int colEnd,
                       std::string& out) override {
        return vterm_ ? vterm_->selectionText(rowStart, colStart, rowEnd, colEnd, out) : false;
    }

    void markSpawnCompleted() { spawnCompleted_.set(); }

private:
    // ---- 线程面（同步原语走 std，等价 Win32 事件语义的有界等待）----

    class DoneEvent {
    public:
        void set() {
            std::lock_guard<std::mutex> lock(m_);
            done_.store(true);
            cv_.notify_all();
        }
        bool peek() const { return done_.load(std::memory_order_relaxed); }
        bool wait_for(std::chrono::milliseconds ms) {
            std::unique_lock<std::mutex> lock(m_);
            return cv_.wait_for(lock, ms, [this] { return done_.load(); });
        }

    private:
        std::mutex m_;
        std::condition_variable cv_;
        std::atomic<bool> done_{false};
    };

    void enqueueWriteLocked(std::string payload) {
        {
            std::lock_guard<std::mutex> lock(wqMutex_);
            wq_.push_back(std::move(payload));
        }
        wqCv_.notify_one();
    }

    void readerLoop() {
        char buf[65536];
        std::string reply;
        for (;;) {
            if (stopping_.load(std::memory_order_relaxed)) break;
            // 坑⑤：无读取在飞窗口内处理 pending resize（latest-wins）
            if (resizePending_.exchange(false, std::memory_order_relaxed)) {
                TerminalSize s;
                {
                    std::lock_guard<std::mutex> lock(resizeMutex_);
                    s = pendingSize_;
                }
                std::lock_guard<std::mutex> lock(apiMutex_);
                if (hpc_ != nullptr && conptyApi().ok) {
                    COORD c;
                    c.X = SHORT(s.cols);
                    c.Y = SHORT(s.rows);
                    conptyApi().resize(hpc_, c);
                }
                if (vterm_) vterm_->resize(s.rows, s.cols);
            }
            DWORD n = 0;
            if (!ReadFile(outputRead_, buf, sizeof(buf), &n, nullptr)) {
                const DWORD err = GetLastError();
                if (err == ERROR_OPERATION_ABORTED && !stopping_.load(std::memory_order_relaxed))
                    continue; // resize 唤醒（P2-2），重回读取循环
                (void)err; // 断裂（坑⑦）/stopping → 退出排空循环
                break;
            }
            if (n > 0 && vterm_) {
                reply.clear();
                vterm_->feed(reinterpret_cast<const uint8_t*>(buf), int32_t(n), &reply);
                if (!reply.empty()) enqueueWriteLocked(std::move(reply)); // DSR/DA 应答回写
            }
        }
        // 退出码收割（有界；拿不到=约定 0）
        DWORD code = 0;
        if (hProcess_ != nullptr &&
            WaitForSingleObject(hProcess_, kCloseWaitMs) == WAIT_OBJECT_0) {
            GetExitCodeProcess(hProcess_, &code);
        }
        exitCode_.store(int32_t(code), std::memory_order_relaxed);
        exited_.store(true, std::memory_order_relaxed); // 坑⑦：断裂=会话结束，宿主不崩
        readerDoneEpoch_.store(1, std::memory_order_relaxed);
        readerDone_.set();
    }

    void writerLoop() {
        for (;;) {
            std::string payload;
            {
                std::unique_lock<std::mutex> lock(wqMutex_);
                wqCv_.wait(lock, [this] {
                    return stopping_.load(std::memory_order_relaxed) || !wq_.empty();
                });
                if (wq_.empty()) break; // stopping 且队列空
                payload = std::move(wq_.front());
                wq_.pop_front();
            }
            DWORD written = 0;
            // 写失败（管道断）静默丢弃：会话即将走断裂→exited 路径
            WriteFile(inputWrite_, payload.data(), DWORD(payload.size()), &written, nullptr);
        }
        writerDone_.set();
    }

    HPCON hpc_ = nullptr;
    HANDLE inputWrite_ = nullptr;
    HANDLE outputRead_ = nullptr;
    HANDLE hProcess_ = nullptr;
    int64_t pid_ = 0;
    std::unique_ptr<VtermCore> vterm_;

    std::mutex apiMutex_; // hpc_ 生命周期守护（reader resize vs terminate Close）

    std::thread reader_;
    std::thread writer_;
    DoneEvent spawnCompleted_;
    DoneEvent readerDone_;
    DoneEvent writerDone_;
    std::atomic<bool> readerDoneEpoch_{0}; // reader 是否已自然退出（detach 决策）
    std::atomic<bool> stopping_{false};
    std::atomic<bool> terminated_{false};
    std::atomic<bool> exited_{false};
    std::atomic<int32_t> exitCode_{0};

    std::mutex wqMutex_;
    std::condition_variable wqCv_;
    std::deque<std::string> wq_;

    std::mutex resizeMutex_;
    TerminalSize pendingSize_;
    std::atomic<bool> resizePending_{false};
};

namespace {

class WinTerminalBackend : public TerminalBackend {
public:
    core::Result<std::unique_ptr<TerminalSessionHandle>> spawn(
        const std::string& shell, const TerminalSize& size) override {
        (void)shell; // 默认 shell 由本后端解析（技术卡决策 3）；显式 shell 经 env 注入口
        if (!conptyApi().ok)
            return core::Result<std::unique_ptr<TerminalSessionHandle>>::fail(
                core::ErrorCode::NotImplemented, "终端需要 Windows 10 1809+（ConPTY 不可用）");
        if (size.rows <= 0 || size.cols <= 0 || size.rows > 200 || size.cols > 500)
            return core::Result<std::unique_ptr<TerminalSessionHandle>>::fail(
                core::ErrorCode::ValidationError, "非法终端尺寸");

        // ① 同步管道 ×2（禁 OVERLAPPED，坑①官方口径）
        HANDLE inputRead = nullptr, inputWrite = nullptr;
        HANDLE outputRead = nullptr, outputWrite = nullptr;
        SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
        if (!CreatePipe(&inputRead, &inputWrite, &sa, 0) ||
            !CreatePipe(&outputRead, &outputWrite, &sa, 0))
            return failSpawn("CreatePipe", GetLastError());

        // ② CreatePseudoConsole
        HPCON hpc = nullptr;
        COORD c;
        c.X = SHORT(size.cols);
        c.Y = SHORT(size.rows);
        const HRESULT hr = conptyApi().create(c, inputRead, outputWrite, 0, &hpc);
        if (FAILED(hr))
            return failSpawn("CreatePseudoConsole", DWORD(hr), inputRead, inputWrite,
                             outputRead, outputWrite);

        // ③ 属性列表挂 PSEUDOCONSOLE + CreateProcess
        std::wstring cmd = resolveShellCommandLine();
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        SIZE_T attrSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
        auto attrList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(
            GetProcessHeap(), 0, attrSize));
        HRESULT attrHr = E_FAIL;
        PROCESS_INFORMATION pi{};
        bool procOk = false;
        if (attrList != nullptr &&
            InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize) &&
            UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc,
                                      sizeof(HPCON), nullptr, nullptr)) {
            procOk = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                                    EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                    &si.StartupInfo, &pi) != FALSE;
            if (!procOk) attrHr = HRESULT_FROM_WIN32(GetLastError());
        }
        if (attrList != nullptr) DeleteProcThreadAttributeList(attrList);
        if (attrList != nullptr) HeapFree(GetProcessHeap(), 0, attrList);

        if (!procOk) {
            conptyApi().close(hpc);
            return failSpawn("CreateProcessW", DWORD(attrHr), inputRead, inputWrite,
                             outputRead, outputWrite);
        }

        // 坑④：CreateProcess 成功后立即关传入侧句柄（引用计数/断管检测）
        CloseHandle(inputRead);
        CloseHandle(outputWrite);

        auto vterm = std::make_unique<VtermCore>(size.rows, size.cols);
        auto handle = std::unique_ptr<WinSessionHandle>(new WinSessionHandle(
            hpc, inputWrite, outputRead, pi.hProcess, int64_t(pi.dwProcessId),
            std::move(vterm)));
        handle->markSpawnCompleted(); // 坑③：起完才允许关闭路径碰 ConPTY
        CloseHandle(pi.hThread);
        return core::Result<std::unique_ptr<TerminalSessionHandle>>::ok(
            std::unique_ptr<TerminalSessionHandle>(std::move(handle)));
    }

    bool available() const override { return conptyApi().ok; }

    const char* unavailableReason() const override {
        return "终端需要 Windows 10 1809 及以上版本（本系统缺少 ConPTY）";
    }

private:
    static core::Result<std::unique_ptr<TerminalSessionHandle>> failSpawn(
        const char* api, DWORD err, HANDLE a = nullptr, HANDLE b = nullptr,
        HANDLE c = nullptr, HANDLE d = nullptr) {
        for (HANDLE h : {a, b, c, d})
            if (h != nullptr) CloseHandle(h);
        std::string msg = std::string("PowerShell 启动失败（") + api +
                          " err=" + std::to_string(uint64_t(err)) + "）";
        return core::Result<std::unique_ptr<TerminalSessionHandle>>::fail(
            core::ErrorCode::Internal, msg.c_str());
    }
};

} // namespace

TerminalBackend* terminalBackendMac() {
    // Windows 编译单元：同名工厂返回 ConPTY 真后端（接口名保持中立，CMake 选源）
    static WinTerminalBackend backend;
    return &backend;
}

// 剪贴板 Phase 2 接线（need:127 Phase 2：Win32 clipboard；Phase 1 保持诚实 false）
bool clipboardCopyText(const uint8_t*, int32_t) { return false; }

} // namespace
