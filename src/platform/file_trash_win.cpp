// src/platform/file_trash_win.cpp
// Windows：IFileOperation + FOFX_RECYCLEONDELETE 移入回收站（SHFileOperationW 已弃用）。
// 两个必须记住的坑：
//   ① IFileOperation 是 STA 对象，调用必须在本线程的 STA 套间里；上层调用方可能是
//      任意线程（甚至已 MTA）→ 本函数自建 STA 线程跑完再 join，调用方零 COM 前提。
//   ② PerformOperations 返回 S_OK **不代表删除成功**（进度回调里才知道单项结果）→
//      取严三级：PerformOperations HRESULT + GetAnyOperationsAborted + 自定义
//      sink 的 PostDeleteItem(hrDelete)。
// 本文件单独含 initguid.h：TU 内定义 CLSID_FileOperation 等 GUID（先例
// src/module_wallpaper/platform/image_probe_win.cpp:11），避免依赖 libuuid 导出。
#include "file_trash.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <initguid.h>  // 必须在所有 shell 头之前：让 DEFINE_GUID 变为本 TU 定义
#include <objbase.h>
#include <shellapi.h>  // FOF_ALLOWUNDO / FOF_NOCONFIRMATION / FOF_SILENT / FOF_NOERRORUI
#include <shlobj.h>
#include <shobjidl.h>  // IFileOperation / FOFX_RECYCLEONDELETE（并带上 sherrors.h 的 COPYENGINE_E_*）

#include <atomic>
#include <string>
#include <system_error>  // std::system_error（std::thread 构造失败，runOnStaThread 捕获）
#include <thread>

#include "core/logging.h"
#include "file_open_common.h"  // platform::detail::utf8ToWide（UTF-8 → UTF-16 唯一实现）

namespace platform::file_trash {
namespace {

// 把 shell 层的 HRESULT 归到五档。default 取 TrashUnavailable 而非 Busy：无法识别的
// 错误多半是「系统拒收/卷不支持」，误报成 Busy 会让用户以为重试就好。
TrashFailure mapHresult(HRESULT hr) {
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
        hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) ||
        hr == COPYENGINE_E_PATH_NOT_FOUND_SRC ||
        hr == COPYENGINE_E_RECYCLE_BIN_NOT_FOUND) {
        return TrashFailure::NotFound;
    }
    if (hr == COPYENGINE_E_ACCESS_DENIED_SRC ||
        hr == COPYENGINE_E_REQUIRES_ELEVATION ||
        hr == COPYENGINE_E_ACCESSDENIED_READONLY ||
        hr == E_ACCESSDENIED) {
        return TrashFailure::PermissionDenied;
    }
    if (hr == COPYENGINE_E_RECYCLE_FORCE_NUKE ||
        hr == COPYENGINE_E_RECYCLE_SIZE_TOO_BIG ||
        hr == COPYENGINE_E_RECYCLE_PATH_TOO_LONG) {
        return TrashFailure::TrashUnavailable;
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) ||
        hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION) ||
        hr == COPYENGINE_E_SHARING_VIOLATION_SRC) {
        return TrashFailure::Busy;
    }
    return TrashFailure::TrashUnavailable;
}

// 进度回调：只为捞到 PostDeleteItem 的逐项 HRESULT（PerformOperations 的返回值
// 覆盖不到单项失败）。引用计数走 Interlocked，任一非本期方法一律 S_OK 放行。
class DeleteSink final : public IFileOperationProgressSink {
public:
    std::atomic<HRESULT> deleteHr{S_OK};
    // PostDeleteItem 是否真的被调用过。deleteHr 初值 S_OK，无法区分「回调说成功」与
    // 「回调根本没来」，故此位用于上层判定「不确定」。
    std::atomic<bool> deleteCalled{false};

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IFileOperationProgressSink) {
            *ppv = static_cast<IFileOperationProgressSink*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(::InterlockedIncrement(&refs_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG remaining = ::InterlockedDecrement(&refs_);
        if (remaining == 0) delete this;
        return static_cast<ULONG>(remaining);
    }

    HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT,
                                             IShellItem*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR,
                                           HRESULT, IShellItem*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR,
                                           HRESULT, IShellItem*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem*, HRESULT hrDelete,
                                             IShellItem*) override {
        deleteHr.store(hrDelete);
        deleteCalled.store(true);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD,
                                          HRESULT, IShellItem*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }

private:
    // IUnknown 无虚析构，故不加 override；Release() 归零时按静态类型 delete 安全
    ~DeleteSink() = default;
    LONG refs_ = 1;
};

// 单次删除的各阶段结果（全部在 STA 线程内写，join 后读）
struct Attempt {
    HRESULT createOp = E_FAIL;
    HRESULT setFlags = E_FAIL;   // SetOperationFlags 结果（失败即禁止进 DeleteItem）
    HRESULT createItem = E_FAIL;
    HRESULT hrDelete = E_FAIL;   // DeleteItem 入队结果
    HRESULT perform = E_FAIL;    // PerformOperations 结果
    HRESULT itemDelete = S_OK;   // PostDeleteItem 回填的逐项结果
    bool itemDeleteCalled = false;  // PostDeleteItem 是否真的回调过
    bool aborted = false;
};

Attempt runOnStaThread(const std::wstring& widePath) {
    Attempt attempt;
    // std::thread 构造会抛 std::system_error（资源紧张时）。本函数跑在删除工作线程里，
    // 上层 moveToTrash 的契约是「返回 TrashOutcome」——异常逃出去就是 std::terminate
    // 杀进程（MinGW 上线程创建间歇失败是仓内已记录过的现象）。就地捕获、按能力不可用降级。
    try {
        std::thread worker([&attempt, &widePath] {
            // 套间按调用进出配对；RPC_E_CHANGED_MODE 说明该线程已被别人初始化成别的
            // 模式（不配对 Uninitialize），照 folder_picker_win.cpp:18-25 的容忍逻辑走。
            const HRESULT initHr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            const bool comInitialized = SUCCEEDED(initHr);
            if (!comInitialized && initHr != RPC_E_CHANGED_MODE) {
                attempt.createOp = initHr;
                return;
            }

            IFileOperation* op = nullptr;
            IFileOperationProgressSink* sinkRef = static_cast<IFileOperationProgressSink*>(
                new DeleteSink());
            HRESULT hr = ::CoCreateInstance(CLSID_FileOperation, nullptr,
                                            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&op));
            attempt.createOp = hr;
            if (SUCCEEDED(hr) && op != nullptr) {
                // 标志位没生效（FOFX_RECYCLEONDELETE / FOF_ALLOWUNDO 任一没设上）就等于
                // 可能绕开回收站走永久删除，宁可不删也不冒险：失败即归失败档，绝不进
                // DeleteItem。此处必须释放 op 与 sinkRef 并保持 COM 配对（CoUninitialize
                // 由本函数尾部统一收尾）。
                attempt.setFlags = op->SetOperationFlags(
                    FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI |
                    FOFX_RECYCLEONDELETE);
                if (SUCCEEDED(attempt.setFlags)) {
                    IShellItem* item = nullptr;
                    hr = ::SHCreateItemFromParsingName(widePath.c_str(), nullptr,
                                                       IID_PPV_ARGS(&item));
                    attempt.createItem = hr;
                    if (SUCCEEDED(hr) && item != nullptr) {
                        attempt.hrDelete = op->DeleteItem(item, sinkRef);
                        if (SUCCEEDED(attempt.hrDelete)) {
                            attempt.perform = op->PerformOperations();
                            WINBOOL aborted = FALSE;
                            if (SUCCEEDED(op->GetAnyOperationsAborted(&aborted))) {
                                attempt.aborted = aborted != FALSE;
                            }
                        }
                        item->Release();
                    }
                }
                op->Release();
            }
            attempt.itemDelete = static_cast<DeleteSink*>(sinkRef)->deleteHr.load();
            attempt.itemDeleteCalled =
                static_cast<DeleteSink*>(sinkRef)->deleteCalled.load();
            sinkRef->Release();

            if (comInitialized) ::CoUninitialize();
        });
        worker.join();
    } catch (const std::system_error&) {
        attempt.createOp = E_FAIL;  // 线程创建失败：按「能力不可用」降级，不杀进程
    }
    return attempt;
}

// 卷是否支持回收站。Windows 在无回收站的卷（网络盘/UNC、CD、FAT/exFAT）上，
// FOFX_RECYCLEONDELETE 会被 Shell 静默降级为**永久删除**——叠加 FOF_NOCONFIRMATION
// + FOF_SILENT + FOF_NOERRORUI 后连「确定要永久删除吗」都不弹，用户以为文件在回收站
// 可恢复，实际已丢。PRD 非目标 1 与 R5 明写「绝不降级为直删」，故先判卷能力：
// 不支持回收站的卷直接如实拒绝，交上层提示「系统废纸篓当前不可用」。
//
// 判据只用确定信息（不查 SHQueryRecycleBinW：空回收站时返回值含混，会把正常 NTFS
// 盘误判成不支持）：
//   ① 驱动器类型：只放行 FIXED/REMOVABLE，其余（REMOTE/CDROM/UNKNOWN）拒；
//   ② 文件系统：FAT/FAT32/exFAT 无回收站语义，拒；**查不到也拒**（fail-closed）。
// fail-closed 而不是 fail-open 的理由：Fail-open 的代价是「静默永久删除用户素材」，
// fail-closed 的代价只是「本该能删的盘报不可用、用户去资源管理器手动删」——两者
// 严重性差一个量级（PRD R5 明写绝不降级为直删）。
bool volumeSupportsRecycleBin(const std::wstring& widePath) {
    // UNC（\\server\share\...）没有「盘符根」，回收站不适用。
    if (widePath.rfind(L"\\\\", 0) == 0) return false;
    if (widePath.size() < 2 || widePath[1] != L':') return false;  // 非盘符路径：无法判定，保守拒

    wchar_t root[4] = {widePath[0], L':', L'\\', L'\0'};
    const UINT driveType = ::GetDriveTypeW(root);
    if (driveType != DRIVE_FIXED && driveType != DRIVE_REMOVABLE) return false;

    wchar_t fsName[MAX_PATH] = {0};
    if (!::GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fsName,
                                 MAX_PATH)) {
        return false;  // 取不到卷信息：无法确认支持回收站 → 保守拒（见函数头）
    }
    // lstrcmpiW = 大小写无关比较（kernel32，无需额外头）
    if (::lstrcmpiW(fsName, L"FAT") == 0 || ::lstrcmpiW(fsName, L"FAT32") == 0 ||
        ::lstrcmpiW(fsName, L"exFAT") == 0) {
        return false;
    }
    return true;
}

}  // namespace

TrashOutcome moveToTrash(const std::string& path) {
    const std::wstring widePath = platform::detail::utf8ToWide(path);
    if (widePath.empty()) return TrashOutcome{TrashFailure::NotFound};
    // 先做存在性判定：不存在时 IFileOperation 的 HRESULT 可能与权限/占用混淆，
    // 而「已不存在」是明确语义，直接给出（也省掉一次 STA 线程创建）。
    if (::GetFileAttributesW(widePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return TrashOutcome{TrashFailure::NotFound};
    }
    // 无回收站的卷必须拒绝：否则 Shell 会永久删除且不弹确认（见函数注释）。
    if (!volumeSupportsRecycleBin(widePath)) {
        LOG_WARN("移入回收站：该卷不支持回收站（网络盘/CD/FAT 系），拒绝删除以免永久丢失");
        return TrashOutcome{TrashFailure::TrashUnavailable};
    }

    const Attempt attempt = runOnStaThread(widePath);
    if (FAILED(attempt.createOp)) {
        LOG_WARN("移入回收站：COM 初始化/IFileOperation 创建失败（HRESULT " +
                 std::to_string(static_cast<long>(attempt.createOp)) + "）");
        return TrashOutcome{TrashFailure::TrashUnavailable};
    }
    if (FAILED(attempt.setFlags)) {
        LOG_WARN("移入回收站：IFileOperation 标志位设置失败（HRESULT " +
                 std::to_string(static_cast<long>(attempt.setFlags)) +
                 "），已放弃本次删除以免绕开回收站");
        return TrashOutcome{TrashFailure::TrashUnavailable};
    }
    if (FAILED(attempt.createItem)) {
        return TrashOutcome{mapHresult(attempt.createItem)};
    }
    // 取严三级：逐项结果 → 中断位 → 入队/执行 HRESULT（分布与优先级保持原样；
    // 「回调是否来过」只作最后一道兜底，不抢前面几档的具体错误）
    if (attempt.itemDeleteCalled && FAILED(attempt.itemDelete)) {
        return TrashOutcome{mapHresult(attempt.itemDelete)};
    }
    if (attempt.aborted) return TrashOutcome{TrashFailure::Busy};
    if (FAILED(attempt.hrDelete)) {
        return TrashOutcome{mapHresult(attempt.hrDelete)};
    }
    if (FAILED(attempt.perform)) {
        return TrashOutcome{mapHresult(attempt.perform)};
    }
    // 兜底判据：PerformOperations 返回 S_OK 但 PostDeleteItem 从未回调 = 无法确认
    // 到底删没删/进没进回收站 → 归为「不确定」TrashUnavailable，不谎报成功。
    if (!attempt.itemDeleteCalled) {
        LOG_WARN("移入回收站：进度回调未回填逐项结果（PerformOperations=" +
                 std::to_string(static_cast<long>(attempt.perform)) +
                 "），结果不可确认，按回收站不可用报出");
        return TrashOutcome{TrashFailure::TrashUnavailable};
    }
    return TrashOutcome{};
}

}  // namespace platform::file_trash
