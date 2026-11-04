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
#include <utility>
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

// 会话共享态：handle（UI/主线程面）与 reader/writer/worker 线程的交界面全部收拢于此，
// 双方仅经 shared_ptr 持有——关停 worker 与 handle 析构互不悬空（P1-2③）。
struct SessionShared {
    std::mutex wqMutex;
    std::condition_variable wqCv;
    std::deque<std::string> wq;
    std::mutex resizeMutex;
    TerminalSize pendingSize;
    std::atomic<bool> resizePending{false};
    std::atomic<bool> writerStop{false};
    std::atomic<bool> readerExit{false}; // P1-2①：置位后 reader 不再重挂 ReadFile
    std::atomic<bool> exited{false};
    std::atomic<int32_t> exitCode{0};
};

// 文件级事件原语（DoneEvent 移出类：worker 与 reader/writer 线程共享）
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

// 关停资源包（P1-2③）：terminate 时把线程/句柄/vterm 全部 move 进来，worker 独占收尾，
// UI 线程立即返回；task 由 shared_ptr 保活，handle 析构零等待、worker 零悬空。
struct ShutdownTask {
    std::thread reader;
    std::thread writer;
    std::shared_ptr<SessionShared> shared;
    std::unique_ptr<VtermCore> vterm;
    HANDLE inputWrite = nullptr;
    HANDLE outputRead = nullptr;
    HANDLE hProcess = nullptr;
    HPCON hpc = nullptr;
    int64_t pid = 0;
    DoneEvent readerDone;
    DoneEvent writerDone;
    std::atomic<bool> readerRetired{false}; // reader 已退出（join/detach 决策）
};

void readerTask(ShutdownTask& t) {
    char buf[65536];
    std::string reply;
    auto* shared = t.shared.get();
    auto* vterm = t.vterm.get();
    for (;;) {
        if (shared->writerStop.load(std::memory_order_relaxed) ||
            shared->readerExit.load(std::memory_order_relaxed))
            break;
        // 坑⑤：无读取在飞窗口内处理 pending resize（latest-wins）
        if (shared->resizePending.exchange(false, std::memory_order_relaxed)) {
            TerminalSize s;
            {
                std::lock_guard<std::mutex> lock(shared->resizeMutex);
                s = shared->pendingSize;
            }
            if (t.hpc != nullptr && conptyApi().ok) {
                COORD c;
                c.X = SHORT(s.cols);
                c.Y = SHORT(s.rows);
                conptyApi().resize(t.hpc, c);
            }
            if (vterm != nullptr) vterm->resize(s.rows, s.cols);
        }
        DWORD n = 0;
        if (!ReadFile(t.outputRead, buf, sizeof(buf), &n, nullptr)) {
            const DWORD err = GetLastError();
            if (err == ERROR_OPERATION_ABORTED && !shared->writerStop.load(std::memory_order_relaxed) &&
                !shared->readerExit.load(std::memory_order_relaxed))
                continue; // resize 唤醒（坑⑤），重回读取循环
            (void)err;    // 断裂（坑⑦）/关停 → 退出排空循环
            break;
        }
        if (n > 0 && vterm != nullptr) {
            reply.clear();
            vterm->feed(reinterpret_cast<const uint8_t*>(buf), int32_t(n), &reply);
            if (!reply.empty()) {
                {
                    std::lock_guard<std::mutex> lock(shared->wqMutex);
                    shared->wq.push_back(std::move(reply));
                }
                shared->wqCv.notify_one();
                reply.clear();
            }
        }
    }
    // 退出码收割（有界；拿不到=约定 0）。P1-2②：收割只发生在本函数内（terminated
    // 与句柄关闭都在 worker 持有句柄期间进行），无悬空面
    DWORD code = 0;
    if (t.hProcess != nullptr &&
        WaitForSingleObject(t.hProcess, kCloseWaitMs) == WAIT_OBJECT_0) {
        GetExitCodeProcess(t.hProcess, &code);
    }
    shared->exitCode.store(int32_t(code), std::memory_order_relaxed);
    shared->exited.store(true, std::memory_order_relaxed); // 坑⑦：断裂=会话结束，宿主不崩
    t.readerDone.set();
}

void writerTask(ShutdownTask& t) {
    auto* shared = t.shared.get();
    for (;;) {
        std::string payload;
        {
            std::unique_lock<std::mutex> lock(shared->wqMutex);
            shared->wqCv.wait(lock, [&shared] {
                return shared->writerStop.load(std::memory_order_relaxed) || !shared->wq.empty();
            });
            if (shared->wq.empty()) break; // 停止且队列空
            payload = std::move(shared->wq.front());
            shared->wq.pop_front();
        }
        DWORD written = 0;
        // 写失败（管道断）静默丢弃：会话即将走断裂→exited 路径
        WriteFile(t.inputWrite, payload.data(), DWORD(payload.size()), &written, nullptr);
    }
    t.writerDone.set();
}

// 关停 worker（P1-2③：在后台线程执行，UI 线程 terminate 立即返回）。
// 全链有界（先例口径 5s）：树杀 → 排空至断裂（自然断裂优先，超时才置 readerExit +
// CancelIoEx 强断，reader 不重挂）→ ClosePseudoConsole（无读取在飞）→ 收 writer →
// 关句柄。任何一步超时都不挂死。
void shutdownTaskWorker(std::shared_ptr<ShutdownTask> t) {
    auto* shared = t->shared.get();
    process_host::terminateTree(t->pid); // 坑②：taskkill /T /F 树杀（先例，有界+探活兜底）

    // 坑②排空：优先让 reader 自然读到管道断裂退出
    if (!t->readerDone.wait_for(std::chrono::milliseconds(2000))) {
        shared->readerExit.store(true, std::memory_order_relaxed); // P1-2①：不重挂 ReadFile
        if (t->outputRead != nullptr) CancelIoEx(t->outputRead, nullptr);
        t->readerDone.wait_for(std::chrono::milliseconds(1000)); // 强断兜底窗口
    }

    // ClosePseudoConsole（此时无读取在飞；残余面=句柄拆除兜底口径，宁拆不挂）
    if (t->hpc != nullptr && conptyApi().ok) conptyApi().close(t->hpc);

    shared->writerStop.store(true, std::memory_order_relaxed);
    shared->wqCv.notify_all();
    if (!t->writerDone.wait_for(std::chrono::milliseconds(1000))) { /* 超时走 detach 兜底 */ }
    if (t->writer.joinable()) {
        if (t->writerDone.peek())
            t->writer.join();
        else
            t->writer.detach();
    }
    if (t->reader.joinable()) {
        if (t->readerRetired.load(std::memory_order_relaxed))
            t->reader.join();
        else
            t->reader.detach();
    }

    // P1-2②：句柄关闭只在 worker（独占句柄副本）内进行，收割（readerTask 尾部）与此
    // 互斥于生命周期之外，无悬空面
    if (t->inputWrite != nullptr) CloseHandle(t->inputWrite);
    if (t->outputRead != nullptr) CloseHandle(t->outputRead);
    if (t->hProcess != nullptr) CloseHandle(t->hProcess);
}

class WinSessionHandle : public TerminalSessionHandle {
public:
    WinSessionHandle(HPCON hpc, HANDLE inputWrite, HANDLE outputRead, HANDLE hProcess,
                     int64_t pid, std::unique_ptr<VtermCore> vterm)
        : shared_(std::make_shared<SessionShared>()),
          vterm_(std::move(vterm)) {
        // 线程与运行态装进共享包（terminate 时整体移交关停 worker）
        task_ = std::make_shared<ShutdownTask>();
        task_->shared = shared_;
        task_->vterm = std::move(vterm); // vterm 移交：terminate 后 handle 判空面接管
        task_->inputWrite = inputWrite;
        task_->outputRead = outputRead;
        task_->hProcess = hProcess;
        task_->hpc = hpc;
        task_->pid = pid;
        task_->reader = std::thread([t = task_] { readerTask(*t); });
        task_->writer = std::thread([t = task_] { writerTask(*t); });
        shared_->exited.store(false);
    }

    ~WinSessionHandle() override { terminate(); }

    WinSessionHandle(const WinSessionHandle&) = delete;
    WinSessionHandle& operator=(const WinSessionHandle&) = delete;

    core::Result<void> write(const uint8_t* bytes, int32_t count, bool paste) override {
        if (bytes == nullptr || count <= 0)
            return core::Result<void>::fail(core::ErrorCode::ValidationError, "空写入");
        if (terminated_.load(std::memory_order_relaxed))
            return core::Result<void>::fail(core::ErrorCode::Internal, "会话已终止");
        std::string payload(reinterpret_cast<const char*>(bytes), size_t(count));
        // bracketed paste：仅在子进程已开启 DECSET 2004 时包裹（转译层平行跟踪）
        if (vterm_ && vterm_->bracketedPasteEnabled())
            payload = "\x1b[200~" + payload + "\x1b[201~";
        {
            std::lock_guard<std::mutex> lock(shared_->wqMutex);
            shared_->wq.push_back(std::move(payload));
        }
        shared_->wqCv.notify_one();
        return core::Result<void>::ok();
    }

    core::Result<void> resize(const TerminalSize& size) override {
        if (size.rows <= 0 || size.cols <= 0)
            return core::Result<void>::fail(core::ErrorCode::ValidationError, "非法尺寸");
        if (terminated_.load(std::memory_order_relaxed))
            return core::Result<void>::fail(core::ErrorCode::Internal, "会话已终止");
        {
            std::lock_guard<std::mutex> lock(shared_->resizeMutex);
            shared_->pendingSize = size;
            shared_->resizePending.store(true, std::memory_order_relaxed);
        }
        // 坑⑤ idle 唤醒：reader 阻塞在无输出的 ReadFile 时 pending resize 永不处理
        //（shell 不重绘→无输出→reader 不醒），CancelIoEx 打断阻塞读后由 reader
        // 在无读取在飞窗口处理。
        CancelIoEx(task_->outputRead, nullptr);
        return core::Result<void>::ok();
    }

    // 关停（P1-2③）：UI 线程只做移交与触发，全链有界序列（树杀→排空→Close→join→
    // 关句柄，技术卡 §3.2 坑②③⑦）在后台 worker 执行，立即返回不冻结。
    void terminate() override {
        bool already = terminated_.exchange(true);
        if (already) return;
        shared_->exited.store(true, std::memory_order_relaxed); // 终止语义即时生效
        auto t = task_; // shared_ptr 保活：handle 立即放手（vterm 已移交），worker 自足收尾
        std::thread(shutdownTaskWorker, t).detach();
    }

    bool exited(int32_t* exitCode) override {
        if (!shared_->exited.load(std::memory_order_relaxed)) return false;
        if (exitCode != nullptr) *exitCode = shared_->exitCode.load(std::memory_order_relaxed);
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

private:
    std::shared_ptr<SessionShared> shared_;
    std::shared_ptr<ShutdownTask> task_;
    std::unique_ptr<VtermCore> vterm_; // terminate 后移入 task_（判空面接管）
    std::atomic<bool> terminated_{false};
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
        SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE}; // P2-1：对齐官方样例
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
            // bug187（2026-11-04 老板实测打回）：属性列表必须写回 si.AttributeList——
            // 只在局部变量上建好不挂进 STARTUPINFOEX，CreateProcessW 会「成功」但
            // PSEUDOCONSOLE 属性不生效：powershell 自行分配新控制台宿主窗（Win11
            // 24H2 默认=Windows Terminal）→ 点按钮弹外部终端窗、内嵌 reader 永阻塞
            // 零字节。官方样例 siEx.lpAttributeList 同位写回。
            si.lpAttributeList = attrList;
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
        // 坑③守卫天然成立：terminate 只能经 handle 指针调（构造已完成），关停 worker
        // 不再需要 spawnCompleted 事件等待
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
        std::string msg = std::string("终端启动失败（") + api +
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

// 剪贴板：选区文本写系统剪贴板（F-03 选区复制链路闭环）。UTF-8→UTF-16 后
// CF_UNICODETEXT；空串/失败面诚实返回 false，不伪造成功
bool clipboardCopyText(const uint8_t* bytes, int32_t count) {
    if (bytes == nullptr || count <= 0) return false;
    const std::string utf8(reinterpret_cast<const char*>(bytes), size_t(count));
    const std::wstring wide = utf8ToWide(utf8);
    if (wide.empty()) return false;
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard(); // P1-1：Win32 契约——打开后立即清空，粘贴板与我们的写入原子成对
    const SIZE_T bytesN = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytesN);
    if (mem == nullptr) {
        CloseClipboard();
        return false;
    }
    const void* locked = GlobalLock(mem); // P3-2：锁失败显式失败路径，不 memcpy 到 nullptr
    if (locked == nullptr) {
        GlobalFree(mem);
        CloseClipboard();
        return false;
    }
    memcpy(const_cast<void*>(locked), wide.data(), bytesN);
    GlobalUnlock(mem);
    const BOOL ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
    // SetClipboardData 成功后 mem 归系统所有，失败才由我们释放
    if (!ok) GlobalFree(mem);
    CloseClipboard();
    return ok != FALSE;
}

// win 键位惯例（技术卡 v2 §5）：Ctrl+C 二象性（分派在 encodeCtrlC）+ Ctrl+V 粘贴主键
TerminalKeyConventions terminalKeyConventions() {
    return TerminalKeyConventions{.ctrlCIsCopy = true, .ctrlVIsPaste = true};
}

} // namespace
