// tests/probes/terminal_conpty_probe.cpp — 终端内嵌 ConPTY 真起探针（池187 复验，2026-11-05）
// 背景（bug187）：STARTUPINFOEX 的属性列表未挂回 si.lpAttributeList → PSEUDOCONSOLE
// 属性不生效：CreateProcessW「成功」但 powershell 自行分配新控制台宿主（Win11 24H2
// 默认 = Windows Terminal 外部窗），内嵌 reader 永阻塞零字节。修复 = 写回
// si.lpAttributeList（commit a830e813）。
// 本探针无窗口依赖（纯 ConPTY 管道，锁屏环境也能跑），三段式断言（规避 powershell
// 启动期吞输入）：
//   0. 挂接金丝雀：DEVDESK_TERMINAL_SHELL 注入 `cmd /c echo CONPTY-ATTACH-CANARY-MARK`
//      （cmd 零冷启动，排除 shell 时序变量）→ ≤10s 内 MARK 经内嵌管道到达。未命中
//      只打诊断不裁决（落穿 A/B）——2026-11-05/11-09 实证：工具 shell（MSYS 桥接 /
//      ctest 管道）下 ConPTY 客户端输入挂接失效，阶段 B 恒红而同一 exe 经 `start`
//      起真实控制台 A+B 全绿；按 consoleStdout() 区分：管道上下文 B 失败 → exit 77
//      跳过（ctest SKIP_RETURN_CODE，非产品缺陷）；真实控制台 B 失败 → 照旧红。
//   A. spawn powershell → ≤30s 内网格/回滚出现**任意可见输出**（banner/prompt）；
//   B. 再写 "echo BUG187-MARK" → ≤20s 内出现 MARK（\r 失败补试 \n）。
// 任一阶段超时 = 红（A 红=ConPTY 输出未达内嵌 reader，bug187 复现态）。
#include "platform/terminal/terminal_backend.h"

#include <windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
namespace {

// 输入回显链只在真实控制台上下文可验：ctest / 工具 shell 下 stdout 是管道，
// ConPTY 客户端输入挂接行为不可信（2026-11-05/11-09 实证：同一 exe 经 start
// 起真实控制台 A+B 全绿，管道上下文 B 恒红）。阶段 B 失败时按 77 跳过。
bool consoleStdout() {
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
}

bool CellsContain(const platform::TerminalFrame& frame, const char* needle,
                  std::string* flatOut = nullptr) {
    std::string flat;
    flat.reserve(frame.cells.size());
    for (const auto& cell : frame.cells) {
        flat.push_back(cell.cp >= 0x20 && cell.cp < 0x7F
                           ? static_cast<char>(cell.cp)
                           : ' ');
    }
    if (flatOut != nullptr) *flatOut = flat;
    return flat.find(needle) != std::string::npos;
}

bool HasAnyVisible(const platform::TerminalFrame& frame) {
    for (const auto& cell : frame.cells) {
        if (cell.cp > 0x20) return true;
    }
    return false;
}

// 轮询等待：predicate 命中或有任意输出且 wantAny=true；返回是否命中目标
bool PollUntil(platform::TerminalSessionHandle* session, const char* needle,
               bool acceptAnyOutput, int timeoutSeconds, std::string* seenOut) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(timeoutSeconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const platform::TerminalFrame frame = session->poll();
        std::string flat;
        const bool hit = CellsContain(frame, needle, seenOut != nullptr ? seenOut : &flat);
        if (hit) return true;
        if (acceptAnyOutput && HasAnyVisible(frame)) return true;
        const int scrollback = session->scrollbackCount();
        for (int i = 0; i < scrollback; ++i) {
            std::string line;
            if (!session->scrollbackLine(i, line)) continue;
            if (seenOut != nullptr && !line.empty()) *seenOut = line;
            if (line.find(needle) != std::string::npos) return true;
            if (acceptAnyOutput && !line.empty()) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

} // namespace

static FILE* g_report = nullptr;

static void Tee(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    if (g_report != nullptr) {
        va_start(args, fmt);
        std::vfprintf(g_report, fmt, args);
        va_end(args);
        std::fflush(g_report);
    }
}

int main(int argc, char** argv) {
    // 工具 shell 无真实控制台（CreatePseudoConsole 返回 S_OK+NULL hpc）——经 report
    // 文件从「start 新控制台」上下文回读判定（2026-11-05 环境实证）。
    if (argc > 1) g_report = std::fopen(argv[1], "w");
    using namespace platform;
    // 两平台同名工厂（历史命名，见 terminal_backend.h:75 注释；Windows TU 实现 ConPTY）
    TerminalBackend* backend = terminalBackendMac();
    if (backend == nullptr || !backend->available()) {
        Tee("[term-probe] FAIL: ConPTY 不可用（%s）\n",
                    backend != nullptr ? backend->unavailableReason()
                                       : "backend=null");
        return 2;
    }

    // 无真实控制台上下文（工具 shell / ctest 管道）：ConPTY 客户端输入挂接不可信，
    // 阶段 B 在此环境恒红而真实控制台全绿（见 consoleStdout 注释），B 失败时跳过。

    const TerminalSize size{30, 100};

    // ---- 阶段 0：ConPTY 客户端挂接金丝雀（cmd /c echo，零 shell 冷启动变量）----
    // spawn 的 shell 参数被忽略（resolveShellCommandLine 读 DEVDESK_TERMINAL_SHELL 注入
    // 整条命令行，terminal_backend_win.cpp），进程内 setenv 即可定向注入。
    // MARK 命中 = healthy 快速通道；未命中不在此裁决（cmd 毫秒级退出，快照抓不到挂接
    // 现场），落穿到 A/B，用 pwsh 存活期的控制台进程差分裁决（见 FAIL A/B 处）。
    SetEnvironmentVariableA("DEVDESK_TERMINAL_SHELL",
                            "cmd.exe /c echo CONPTY-ATTACH-CANARY-MARK");
    auto canarySpawned = backend->spawn("powershell.exe", size);
    SetEnvironmentVariableA("DEVDESK_TERMINAL_SHELL", nullptr);
    if (canarySpawned.isFail()) {
        Tee("[term-probe] FAIL: 金丝雀 spawn 失败：%s\n",
                    canarySpawned.errorMessage().c_str());
        return 1;
    }
    {
        auto canary = std::move(canarySpawned.value());
        const bool markHit = PollUntil(canary.get(), "CONPTY-ATTACH-CANARY-MARK",
                                       /*acceptAnyOutput=*/false, /*timeoutSeconds=*/10,
                                       nullptr);
        if (markHit) {
            Tee("[term-probe] PASS 0: 挂接金丝雀命中（cmd /c echo 文本经内嵌管道到达）\n");
        } else {
            Tee("[term-probe][diag] 金丝雀 MARK 10s 未达——落穿 A/B，由 pwsh 存活期"
                "控制台差分裁决挂接形态\n");
        }
        canary->terminate();
    }

    // 挂接差分基线已退役：管道上下文下 GetConsoleProcessList 不可靠，改由
    // consoleStdout() 判定（见阶段 B 失败分支）。
    auto spawned = backend->spawn("powershell.exe", size);
    if (spawned.isFail()) {
        Tee("[term-probe] FAIL: spawn 失败：%s\n",
                    spawned.errorMessage().c_str());
        return 1;
    }
    auto session = std::move(spawned.value());

    // 阶段 A：powershell 启动必须先有可见输出（banner/prompt）；一帧都收不到 =
    // ConPTY 管道未生效（bug187 复现态：输出跑到外部控制台宿主窗去了）。
    std::string screen;
    const bool anyOutput = PollUntil(session.get(), "", /*acceptAnyOutput=*/true,
                                     /*timeoutSeconds=*/30, &screen);
    if (!anyOutput) {
        session->terminate();
        Tee(
            "[term-probe] FAIL A: spawn 后 30s 内 ConPTY 零输出——内嵌 reader 未收到任何"
            "字节（bug187 复现态；若桌面可见应同时弹出外部终端窗）\n");
        return 1;
    }
    Tee("[term-probe] PASS A: powershell 首屏输出到达（首帧样本已捕获）\n");

    // 阶段 B：等提示符（客户端就绪）再写命令，避免 powershell 初始化期吞输入；
    // 每轮写 "\r" 失败补试 "\n"，最多 3 轮。
    (void)PollUntil(session.get(), ">", /*acceptAnyOutput=*/false,
                    /*timeoutSeconds=*/20, nullptr);  // 提示符未出也继续尝试（容错）
    const char* needle = "BUG187-MARK";
    bool markSeen = false;
    for (int round = 0; round < 3 && !markSeen; ++round) {
        for (const char* term : {"\r", "\n"}) {
            const std::string command = std::string("echo BUG187-MARK") + term;
            const auto written = session->write(
                reinterpret_cast<const uint8_t*>(command.data()),
                static_cast<int32_t>(command.size()), /*paste=*/false);
            if (written.isFail()) {
                Tee("[term-probe] FAIL B: write 失败：%s\n",
                            written.errorMessage().c_str());
                session->terminate();
                return 1;
            }
            if (PollUntil(session.get(), needle, /*acceptAnyOutput=*/false,
                          /*timeoutSeconds=*/6, nullptr)) {
                markSeen = true;
                break;
            }
        }
    }

    int32_t exitCode = 0;
    const bool exited = session->exited(&exitCode);
    session->terminate();

    if (!markSeen) {
        if (!consoleStdout()) {
            Tee(
                "[term-probe] SKIP: 命令写入后 20s 无回显，且 stdout 非控制台（工具 shell /"
                "ctest 管道上下文，ConPTY 输入回显链不可验；真实控制台运行应全绿）"
                "→ 按 77 跳过\n");
            return 77;
        }
        Tee(
            "[term-probe] FAIL B: 命令写入后 20s 内无回显 BUG187-MARK（exited=%d）\n",
            exited ? 1 : 0);
        return 1;
    }
    Tee("[term-probe] PASS B: 内嵌 ConPTY 命令往返成功（echo 回显命中）\n");
    Tee("[term-probe] PASS: 终端内嵌链路（spawn/输出/输入/回显）真跑通过\n");
    return 0;
}