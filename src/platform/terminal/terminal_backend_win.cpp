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
// ConPTY 三函数走 GetProcAddress 动态解析（typedef）——三级查找（debt246）：
// DEVDESK_CONPTY_DIR → exe 旁 conpty\（conpty.dll + OpenConsole.exe 成对，v2 redist）
// → 系统 kernel32 inbox；redist 不可用不崩、WARN 回落 inbox，来源入启动日志一条。
// 老 OS 缺导出时 available()=false 给明确文案，不做导入期链接（导入缺失会让进程
// 载入直接失败）。
#include "terminal_backend.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cwchar>
#include <utility>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/logging.h"
#include "process_host.h"
#include "terminal_conpty_lookup.h"
#include "terminal_vterm_core.h"

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

namespace platform {

namespace {

// ---- ConPTY API 动态解析（debt246 三级查找：DEVDESK_CONPTY_DIR → exe 旁 conpty\ →
//      系统 kernel32 inbox；redist 缺文件/缺导出不崩，明确降级回落 inbox）----

using FnCreatePseudoConsole = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
using FnResizePseudoConsole = HRESULT(WINAPI*)(HPCON, COORD);
using FnClosePseudoConsole = void(WINAPI*)(HPCON);
// v2 新增（microsoft/terminal PR#14544）：拆除 pseudoconsole 不等待客户端退出——
// 关停链先树杀后调用，正好规避 inbox ClosePseudoConsole 挂死类（terminal#17688）。
// typedef + GetProcAddress 动态取，不硬编码任何 v2 专属符号；v1 无此导出 → 回落 close。
using FnReleasePseudoConsole = void(WINAPI*)(HPCON);

struct ConPtyApi {
    FnCreatePseudoConsole create = nullptr;
    FnResizePseudoConsole resize = nullptr;
    FnClosePseudoConsole close = nullptr;
    FnReleasePseudoConsole release = nullptr; // 装载完成时恒非空（缺失=close）
    bool ok = false;
};

// UTF-16 → UTF-8（本 TU 局部版，与 file_open_win 同实现口径；本 TU 惯例 = 匿名空间本地助手）
std::string wideToUtf8(const wchar_t* wstr, int wlen) {
    if (wstr == nullptr || wlen <= 0) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(size_t(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, out.data(), needed, nullptr, nullptr);
    return out;
}

// 文件面校验：目录下 conpty.dll + OpenConsole.exe **成对**存在才可用（只带 dll 不带
// OpenConsole 会静默回落 inbox conhost——wtmux 实证坑，加载前必须拦下，clue105）
bool conptyPairPresent(const std::wstring& dir) {
    const auto present = [&dir](const wchar_t* name) {
        const DWORD attr = GetFileAttributesW((dir + L"\\" + name).c_str());
        return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
    };
    return present(L"conpty.dll") && present(L"OpenConsole.exe");
}

// conpty.dll 版本资源 → "x.y.z…"（启动日志一条，供 TermPerf/验收区分 pin 版本）
std::string conptyFileVersion(const std::wstring& dllPath) {
    const DWORD size = GetFileVersionInfoSizeW(dllPath.c_str(), nullptr);
    if (size == 0) return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(dllPath.c_str(), 0, size, data.data())) return {};
    struct LangCodePage { WORD lang; WORD codePage; };
    LangCodePage* trans = nullptr;
    UINT transLen = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&trans), &transLen) ||
        trans == nullptr || transLen < sizeof(LangCodePage))
        return {};
    wchar_t sub[64];
    swprintf(sub, 64, L"\\StringFileInfo\\%04x%04x\\FileVersion", trans[0].lang,
             trans[0].codePage);
    wchar_t* value = nullptr;
    UINT valueLen = 0;
    if (VerQueryValueW(data.data(), sub, reinterpret_cast<void**>(&value), &valueLen) &&
        value != nullptr && valueLen > 1)
        return wideToUtf8(value, int(valueLen) - 1);
    return {};
}

const ConPtyApi& conptyApi() {
    static const ConPtyApi api = [] {
        ConPtyApi a;
        // 三核心导出齐才 ok；release 缺失回落 close。半残 redist 卸载并清位，
        // 由调用方落 inbox——不带病绑定（函数指针悬在已 FreeLibrary 的模块上）。
        auto finalize = [&a](HMODULE redist) {
            if (a.create != nullptr && a.resize != nullptr && a.close != nullptr) {
                if (a.release == nullptr) a.release = a.close;
                a.ok = true;
                return true;
            }
            if (redist != nullptr) FreeLibrary(redist);
            a = ConPtyApi{};
            return false;
        };

        // exe 目录（②级候选基底）
        wchar_t exePath[MAX_PATH];
        std::wstring exeDir;
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
            const std::wstring dir(exePath);
            const size_t slash = dir.find_last_of(L"\\/");
            if (slash != std::wstring::npos) exeDir = dir.substr(0, slash);
        }
        // ①级候选：DEVDESK_CONPTY_DIR（顺序判定在 terminal_conpty_lookup.h 纯函数，单测面）
        wchar_t envBuf[MAX_PATH];
        const DWORD envLen = GetEnvironmentVariableW(L"DEVDESK_CONPTY_DIR", envBuf, MAX_PATH);
        const std::wstring envRaw =
            (envLen > 0 && envLen < MAX_PATH) ? std::wstring(envBuf) : std::wstring();
        const bool envSet = envRaw.find_first_not_of(L" \t\"'") != std::wstring::npos;
        const std::wstring candidate = terminal::conptyDirCandidate(envRaw, exeDir);

        bool fromRedist = false;
        std::string redistVer;
        std::wstring redistDir;
        if (!candidate.empty() && conptyPairPresent(candidate)) {
            const std::wstring dllPath = candidate + L"\\conpty.dll";
            // 全路径 LoadLibrary（裸名会走系统搜索序，可能绑到陌生同名 dll）；
            // 句柄刻意不卸——函数指针被各会话线程长期持有，进程生命周期 = 模块生命周期
            HMODULE redist = LoadLibraryW(dllPath.c_str());
            if (redist != nullptr) {
                a.create = reinterpret_cast<FnCreatePseudoConsole>(
                    reinterpret_cast<void*>(GetProcAddress(redist, "CreatePseudoConsole")));
                a.resize = reinterpret_cast<FnResizePseudoConsole>(
                    reinterpret_cast<void*>(GetProcAddress(redist, "ResizePseudoConsole")));
                a.close = reinterpret_cast<FnClosePseudoConsole>(
                    reinterpret_cast<void*>(GetProcAddress(redist, "ClosePseudoConsole")));
                a.release = reinterpret_cast<FnReleasePseudoConsole>(
                    reinterpret_cast<void*>(GetProcAddress(redist, "ReleasePseudoConsole")));
                if (finalize(redist)) {
                    fromRedist = true;
                    redistVer = conptyFileVersion(dllPath);
                    redistDir = candidate;
                }
            }
        }
        if (!fromRedist) {
            // 显式配置（env）或目录存在但不可用 → WARN；exe 旁目录整体缺席 = 常态
            //（开发树未放资产），静默走 inbox。wtmux 坑：缺 OpenConsole 的半目录在
            // conptyPairPresent 已拦，不会静默半绑。
            if (envSet ||
                (!candidate.empty() &&
                 GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)) {
                LOG_WARN("ConPTY: conpty redist 不可用，回落 inbox：" +
                         wideToUtf8(candidate.c_str(), int(candidate.size())) +
                         "（需 conpty.dll + OpenConsole.exe 成对且导出完整）");
            }
            HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
            if (k32 == nullptr) return a;
            a.create = reinterpret_cast<FnCreatePseudoConsole>(
                reinterpret_cast<void*>(GetProcAddress(k32, "CreatePseudoConsole")));
            a.resize = reinterpret_cast<FnResizePseudoConsole>(
                reinterpret_cast<void*>(GetProcAddress(k32, "ResizePseudoConsole")));
            a.close = reinterpret_cast<FnClosePseudoConsole>(
                reinterpret_cast<void*>(GetProcAddress(k32, "ClosePseudoConsole")));
            finalize(nullptr);
        }
        if (fromRedist) {
            LOG_INFO("ConPTY: v2 redist " +
                     (redistVer.empty() ? std::string("(版本资源缺失)") : redistVer) + " @ " +
                     wideToUtf8(redistDir.c_str(), int(redistDir.size())));
        } else if (a.ok) {
            LOG_INFO("ConPTY: inbox kernel32 CreatePseudoConsole（未发现可用 conpty/ redist）");
        }
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
// need250：shellChoice 非空 = 设置面板选名（pwsh/powershell/cmd/wsl），映射后探测
// 不可得（如未装 wsl）→ 回默认链（设计 §4.3「无效值回退平台默认」；启动失败呈现
// 走既有 Failed 卡，面板不预校验）。
std::wstring defaultShellCommandLine() {
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

std::wstring resolveShellCommandLine(const std::string& shellChoice) {
    if (const char* injected = std::getenv("DEVDESK_TERMINAL_SHELL")) {
        if (injected[0] != '\0') return utf8ToWide(injected);
    }
    wchar_t path[MAX_PATH];
    if (shellChoice == "powershell") {
        if (SearchPathW(nullptr, L"powershell.exe", nullptr, MAX_PATH, path, nullptr) > 0)
            return quoted(path) + L" -NoLogo";
        return defaultShellCommandLine();
    }
    if (shellChoice == "cmd") {
        wchar_t comspec[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"COMSPEC", comspec, MAX_PATH);
        if (n > 0 && n < MAX_PATH) return quoted(comspec);
        return defaultShellCommandLine();
    }
    if (shellChoice == "wsl") {
        if (SearchPathW(nullptr, L"wsl.exe", nullptr, MAX_PATH, path, nullptr) > 0)
            return quoted(path);
        return defaultShellCommandLine();
    }
    return defaultShellCommandLine(); // ""/pwsh/未知 → 默认链
}

// spawn 工作目录（need250）：设置面板空值 = 用户主目录（%USERPROFILE%，与
// terminal_launch_win 同源口径）；未设环境变量的罕见态回退 nullptr（继承进程 cwd）。
std::wstring resolveWorkingDirectory(const std::string& workDir) {
    if (!workDir.empty()) return utf8ToWide(workDir);
    const char* profile = std::getenv("USERPROFILE");
    if (profile != nullptr && profile[0] != '\0') return utf8ToWide(profile);
    return L"";
}

// 有界关闭等待（技术卡 §3.2：排空/Close/join 全链有界，先例口径 5s）
constexpr DWORD kCloseWaitMs = 5000;
// P1-3：reader 收割上界（进程退出等待）须 ≤ worker 关停链的 reader 兜底窗口——
// worker 先析构 task 而 reader 仍在 WaitForSingleObject → vterm/句柄悬空触取
constexpr DWORD kHarvestWaitMs = 1000;

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
    // ConPTY resize 门控（need241 真机实证 2026-11-05）：首字节到达前调
    // ResizePseudoConsole 会**永久卡死输出通道**（实证：early resize 后 generation
    // 冻结、可见 cell 恒 0、进程不退；晚于首输出则正常——早版装机未触雷只因用户
    // 进页晚于 pwsh 冷启动）。pending resize 由 reader 门控到首输出后再发。
    std::atomic<bool> firstOutput{false};
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
    // ---- TermPerf 诊断（bug248 临时，删诊断时整块移除）：只累加计数，每 5s 一条
    static std::atomic<uint64_t> perfBytes{0};
    static std::atomic<uint64_t> perfFeedNs{0};
    static std::atomic<uint64_t> perfFeedMaxNs{0};
    static std::atomic<uint64_t> perfAborted{0};
    static std::atomic<uint64_t> perfResizes{0};
    static std::atomic<int64_t> perfLastLogMs{0};
    auto perfFlush = [] {
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
        int64_t last = perfLastLogMs.load(std::memory_order_relaxed);
        if (last == 0) {
            perfLastLogMs.store(nowMs, std::memory_order_relaxed);
            return;
        }
        if (nowMs - last < 5000 ||
            !perfLastLogMs.compare_exchange_strong(last, nowMs, std::memory_order_relaxed))
            return;
        const uint64_t b = perfBytes.exchange(0, std::memory_order_relaxed);
        const uint64_t f = perfFeedNs.exchange(0, std::memory_order_relaxed);
        const uint64_t fm = perfFeedMaxNs.exchange(0, std::memory_order_relaxed);
        LOG_INFO("TermPerf: reader bytes=" + std::to_string(b) +
                 " feedMs=" + std::to_string(f / 1000000ull) +
                 " maxFeedMs=" + std::to_string(fm / 1000000ull) +
                 " aborted=" + std::to_string(perfAborted.exchange(0, std::memory_order_relaxed)) +
                 " resizes=" + std::to_string(perfResizes.exchange(0, std::memory_order_relaxed)));
    };
    for (;;) {
        perfFlush();
        if (shared->writerStop.load(std::memory_order_relaxed) ||
            shared->readerExit.load(std::memory_order_relaxed))
            break;
        // 坑⑤：无读取在飞窗口内处理 pending resize（latest-wins）。
        // need241 门控：ResizePseudoConsole 必须晚于首字节（早发=输出通道永久卡死，
        // 见 SessionShared::firstOutput 注）——未见到首输出则保持 pending 不 exchange，
        // 由后续循环重试。
        if (shared->firstOutput.load(std::memory_order_relaxed) &&
            shared->resizePending.exchange(false, std::memory_order_relaxed)) {
            perfResizes.fetch_add(1, std::memory_order_relaxed); // TermPerf（bug248）
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
                !shared->readerExit.load(std::memory_order_relaxed)) {
                perfAborted.fetch_add(1, std::memory_order_relaxed); // TermPerf（bug248）
                continue; // resize 唤醒（坑⑤），重回读取循环
            }
            (void)err;    // 断裂（坑⑦）/关停 → 退出排空循环
            break;
        }
        if (n > 0) {
            // resize 门控解锁点：客户端已连入（首字节过管道），此后 ResizePseudoConsole 安全
            shared->firstOutput.store(true, std::memory_order_relaxed);
            perfBytes.fetch_add(n, std::memory_order_relaxed); // TermPerf（bug248）
        }
        if (n > 0 && vterm != nullptr) {
            reply.clear();
            const auto feedT0 = std::chrono::steady_clock::now();
            vterm->feed(reinterpret_cast<const uint8_t*>(buf), int32_t(n), &reply);
            const uint64_t feedNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - feedT0)
                    .count());
            perfFeedNs.fetch_add(feedNs, std::memory_order_relaxed); // TermPerf（bug248）
            uint64_t prevMax = perfFeedMaxNs.load(std::memory_order_relaxed);
            while (feedNs > prevMax &&
                   !perfFeedMaxNs.compare_exchange_weak(prevMax, feedNs,
                                                        std::memory_order_relaxed)) {
            }
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
    // 退出码收割（有界 kHarvestWaitMs ≤ worker 兜底窗口，P1-3；拿不到=约定 0）。
    // P1-2②：收割只发生在本函数内（terminated 与句柄关闭都在 worker 持有句柄期间
    // 进行），无悬空面
    DWORD code = 0;
    if (t.hProcess != nullptr &&
        WaitForSingleObject(t.hProcess, kHarvestWaitMs) == WAIT_OBJECT_0) {
        GetExitCodeProcess(t.hProcess, &code);
    }
    shared->exitCode.store(int32_t(code), std::memory_order_relaxed);
    shared->exited.store(true, std::memory_order_relaxed); // 坑⑦：断裂=会话结束，宿主不崩
    // P1-3：先置 retired 再 set done——worker 见 done 即可见 retired（DoneEvent 内
    // mutex 建 happens-before），join 分支由此可达，join 后才析构 task
    t.readerRetired.store(true, std::memory_order_relaxed);
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
        // P1（Win 专项）配套：关停中不再发起新写——writerDone 超时路径 CancelIoEx
        // 强断后若回环重写会再次阻塞，worker 随后的 CloseHandle(inputWrite) 就打在
        // 阻塞 I/O 上（UB 面）；正常路径 writerStop 恒 false，零影响
        if (shared->writerStop.load(std::memory_order_relaxed)) break;
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
    // P2（复审）：树杀前判存活——进程已退出后 taskkill /T /F 按 pid 匹配，pid 复用
    // 窗口会误杀无关进程树。hProcess 已退出（含断裂自然收割）→ 跳过；仍 STILL_ACTIVE
    // 才树杀。（shared->exited 在 terminate() 即置真，不构成存活判据）
    DWORD liveCode = 0;
    const bool alive = t->hProcess != nullptr &&
                       GetExitCodeProcess(t->hProcess, &liveCode) != FALSE &&
                       liveCode == STILL_ACTIVE;
    if (alive) process_host::terminateTree(t->pid); // 坑②：taskkill /T /F 树杀（先例，有界+探活兜底）

    // 坑②排空：优先让 reader 自然读到管道断裂退出。P1-3：等待上界=kCloseWaitMs
    // （对齐 reader 全链上界，原 2s+1s=3s < reader 收割 5s → task 先析构、reader
    // 仍触碰 vterm/句柄 = UAF）
    if (!t->readerDone.wait_for(std::chrono::milliseconds(kCloseWaitMs))) {
        shared->readerExit.store(true, std::memory_order_relaxed); // P1-2①：不重挂 ReadFile
        if (t->outputRead != nullptr) CancelIoEx(t->outputRead, nullptr);
        t->readerDone.wait_for(std::chrono::milliseconds(kHarvestWaitMs)); // 强断兜底窗口（≥收割上界）
    }

    // 拆 pseudoconsole（此时无读取在飞；残余面=句柄拆除兜底口径，宁拆不挂）。
    // release 优先（v2：不等客户端退出即拆除，terminal#17688 挂死类已由树杀+排空
    // 兜住；inbox 无此导出，api.release 装载时已回落 close，语义不变）
    if (t->hpc != nullptr && conptyApi().ok) conptyApi().release(t->hpc);

    shared->writerStop.store(true, std::memory_order_relaxed);
    shared->wqCv.notify_all();
    if (!t->writerDone.wait_for(std::chrono::milliseconds(1000))) {
        // P1（Win 专项）：writer 可能仍阻塞在 WriteFile(inputWrite)——直接走
        // join/detach 决策后 worker 会在下方 CloseHandle(inputWrite)，对阻塞 I/O 中
        // 的句柄拆除 = UB。先强断挂起写（writerTask 见 writerStop 不再回环重写），
        // 再给收割窗口，然后才决策。
        if (t->inputWrite != nullptr) CancelIoEx(t->inputWrite, nullptr);
        t->writerDone.wait_for(std::chrono::milliseconds(kHarvestWaitMs));
    }
    if (t->writer.joinable()) {
        if (t->writerDone.peek())
            t->writer.join();
        else
            t->writer.detach();
    }
    if (t->reader.joinable()) {
        // P1-3：retired 未置位（强断路径刚唤醒）再给一个收割窗口，让 join 分支在
        // reader 正常退出时必达——join 后 worker 才关句柄/析构 task，悬空面闭合；
        // 全窗口耗尽仍 wedge 才 detach（既有兜底语义）
        if (!t->readerRetired.load(std::memory_order_relaxed))
            t->readerDone.wait_for(std::chrono::milliseconds(kHarvestWaitMs));
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
        : shared_(std::make_shared<SessionShared>()) {
        // 线程与运行态装进共享包（terminate 时整体移交关停 worker）
        task_ = std::make_shared<ShutdownTask>();
        task_->shared = shared_;
        // vterm 所有权只归 task_（reader 线程与关停 worker 都经 task_ 保活）；
        // handle 只持非拥有指针——**不可先移入成员再 move 参数**：第二次 move
        // 得到空指针，reader 拿到 nullptr 后 feed 永不执行 → 帧恒空（2026-11-05 根因）。
        task_->vterm = std::move(vterm);
        vterm_ = task_->vterm.get();
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
        // bracketed paste：只有**真正的粘贴**（paste=true）且子进程已开启 DECSET 2004
        // 才包裹。普通键入/按键/ESC 序列必须直发——全量包裹会让 TUI 把每个按键当
        // 粘贴文本（Claude Code 等启用 ?2004 的 TUI 进入后任何输入失效，真机实测根因）。
        if (paste && vterm_ && vterm_->bracketedPasteEnabled())
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
        // 在无读取在飞窗口处理。P2（复审）：CancelIoEx 前复核 terminated_——
        // terminate() 已发起时 worker 可能正关闭句柄链，此时取消会打在已关闭/
        // 复用的句柄上（误伤无关句柄）。
        if (terminated_.load(std::memory_order_relaxed))
            return core::Result<void>::fail(core::ErrorCode::Internal, "会话已终止");
        CancelIoEx(task_->outputRead, nullptr);
        return core::Result<void>::ok();
    }

    // 关停（P1-2③）：UI 线程只做移交与触发，全链有界序列（树杀→排空→Close→join→
    // 关句柄，技术卡 §3.2 坑②③⑦）在后台 worker 执行，立即返回不冻结。
    void terminate() override {
        bool already = terminated_.exchange(true);
        if (already) return;
        shared_->exited.store(true, std::memory_order_relaxed); // 终止语义即时生效
        vterm_ = nullptr; // 终止后 handle 面不再读 vterm（实体内存在 task_）
        auto t = task_;   // shared_ptr 保活：handle 立即放手，worker 自足收尾
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

    // need250 回滚上限：转发 VtermCore（deque 裁最旧）；终止后 vterm_ 为空即忽略
    void setScrollbackLimit(int lines) override {
        if (vterm_ && lines > 0) vterm_->setScrollbackLimit(static_cast<size_t>(lines));
    }

private:
    std::shared_ptr<SessionShared> shared_;
    std::shared_ptr<ShutdownTask> task_;
    VtermCore* vterm_ = nullptr; // 非拥有：实体内存在 task_->vterm（task_ 保活）
    std::atomic<bool> terminated_{false};
};

namespace {

class WinTerminalBackend : public TerminalBackend {
public:
    core::Result<std::unique_ptr<TerminalSessionHandle>> spawn(
        const std::string& shell, const TerminalSize& size,
        const std::string& workDir) override {
        (void)shell; // 选名映射在 resolveShellCommandLine（need250；默认链为空串语义）
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
        // P2（复审）：hpc==nullptr 也按失败走 failSpawn——后续路径（CreateProcess
        // 属性挂接、!procOk 幂等 close）都默认 hpc 非空
        if (FAILED(hr) || hpc == nullptr)
            return failSpawn("CreatePseudoConsole", FAILED(hr) ? DWORD(hr) : DWORD(E_FAIL),
                             inputRead, inputWrite, outputRead, outputWrite);

        // ③ 属性列表挂 PSEUDOCONSOLE + CreateProcess
        std::wstring cmd = resolveShellCommandLine(shell);
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');
        const std::wstring workDirW = resolveWorkingDirectory(workDir);
        LPCWSTR workDirPtr = workDirW.empty() ? nullptr : workDirW.c_str();
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
                                    EXTENDED_STARTUPINFO_PRESENT, nullptr, workDirPtr,
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
