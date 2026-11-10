// src/platform/file_association_win.cpp
// Windows .md 文件关联登记：HKCU\Software\Classes 写/查/清（F-06）。
// 机制（官方，clue4）：ProgId 最小结构 = (Default) 描述 + DefaultIcon +
// shell\open\command；扩展名默认值指向 ProgId；登记后必须
// SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL) 刷 Shell 缓存；
// 无注销专用 API = 删键（本模块仅测试清理缝删键，生产不注销，PRD D-4）。
// 查询 = IApplicationAssociationRegistration::QueryCurrentDefault（AL_EFFECTIVE，
// UserChoice 优先、回落类默认值），比对 ProgId 串判属（exe 路径变不影响判属）。
// 全程 HKCU（用户级 Classes 合并进 HKCR），免管理员。
#include "file_association.h"

#include "autostart.h"        // executablePath()：GetModuleFileNameW 运行时解析（复用，不自写）
#include "file_open_common.h" // platform::detail::utf8ToWide / wideToUtf8 共用编码链

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>  // CoInitializeEx / CoCreateInstance / CoTaskMemFree（LEAN_AND_MEAN 不带 ole2.h）
#include <shlobj.h>   // SHChangeNotify / SHCNE_ASSOCCHANGED
#include <shobjidl.h> // IApplicationAssociationRegistration 接口声明

#include <string>

namespace platform::file_association {
namespace {

// CLSID/IID 手写 SDK 固定值：不依赖 __uuidof/libuuid（MSVC 与 MinGW 双通）
// CLSID_ApplicationAssociationRegistration {4DB5ADFE-2257-4B1D-9029-2F02AF3BB2DC}
constexpr CLSID kAssocRegistrationClsid = {
    0x4DB5ADFE, 0x2257, 0x4B1D,
    {0x90, 0x29, 0x2F, 0x02, 0xAF, 0x3B, 0xB2, 0xDC}};
// IID_IApplicationAssociationRegistration {4E5F4267-9A01-4A4F-A76D-8B75A1D0D5B6}
constexpr IID kAssocRegistrationIid = {
    0x4E5F4267, 0x9A01, 0x4A4F,
    {0xA7, 0x6D, 0x8B, 0x75, 0xA1, 0xD0, 0xD5, 0xB6}};

constexpr wchar_t kClassesRoot[] = L"Software\\Classes";

// spy 缝存储：null = 生产真调用（单测装 spy，用例退出前恢复 null）
ShellNotifySink* g_notifySink = nullptr;

void shellAssociationsChanged() {
    if (g_notifySink != nullptr) {
        g_notifySink();
        return;
    }
    ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// "Software\Classes\<progId>"（leaf 非空 = 再挂一级子键）
std::wstring progIdSubkey(const AssocConfig& cfg, const wchar_t* leaf) {
    std::wstring key = std::wstring(kClassesRoot) + L"\\" +
                       platform::detail::utf8ToWide(cfg.progId);
    if (leaf != nullptr) key += std::wstring(L"\\") + leaf;
    return key;
}

std::wstring extSubkey(const AssocConfig& cfg) {
    return std::wstring(kClassesRoot) + L"\\" +
           platform::detail::utf8ToWide(cfg.extension);
}

// 写 REG_SZ 值（valueName=null = 键默认值；键不存在则连带创建整条路径）
core::Result<void> setSzValue(const std::wstring& subkey,
                              const wchar_t* valueName,
                              const std::wstring& data) {
    const LSTATUS st =
        RegSetKeyValueW(HKEY_CURRENT_USER, subkey.c_str(), valueName, REG_SZ,
                        data.c_str(),
                        static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    if (st != ERROR_SUCCESS) {
        return core::Result<void>::fail(
            core::ErrorCode::Internal,
            "写 HKCU 关联注册表值失败 (" +
                platform::detail::wideToUtf8(subkey.c_str(),
                                             static_cast<int>(subkey.size())) +
                " (RegSetKeyValueW=" + std::to_string(static_cast<long>(st)) +
                ")");
    }
    return core::Result<void>::ok();
}

// 登记 ProgId 树 + 扩展名默认值指向（幂等：同值覆写，无累积）
core::Result<void> writeRegistration(const AssocConfig& cfg) {
    const std::string exe = autostart::executablePath();
    if (exe.empty()) {
        return core::Result<void>::fail(
            core::ErrorCode::Internal,
            "未能解析本进程 exe 路径，文件关联未登记");
    }
    if (core::Result<void> r = setSzValue(
            progIdSubkey(cfg, nullptr), nullptr,
            platform::detail::utf8ToWide(kProgIdDescription));
        r.isFail()) {
        return r;
    }
    if (core::Result<void> r = setSzValue(
            progIdSubkey(cfg, L"DefaultIcon"), nullptr,
            platform::detail::utf8ToWide(buildDefaultIconValue(exe)));
        r.isFail()) {
        return r;
    }
    if (core::Result<void> r = setSzValue(
            progIdSubkey(cfg, L"shell\\open\\command"), nullptr,
            platform::detail::utf8ToWide(buildOpenCommandValue(exe)));
        r.isFail()) {
        return r;
    }
    return setSzValue(extSubkey(cfg), nullptr,
                      platform::detail::utf8ToWide(cfg.progId));
}

// 查询当前生效默认 ProgId 串；fail = 查询失败（调用方转 Unknown，绝不映射 Others）
core::Result<std::string> queryCurrentDefaultProgId(const AssocConfig& cfg) {
    // 套间初始化：进程内可能已是其他模型——S_FALSE（已初始化）与
    // RPC_E_CHANGED_MODE（模型冲突，对象仍可用）都放行；仅 S_OK/S_FALSE 需
    // 退出时配对 CoUninitialize 平衡计数。
    const HRESULT ci = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool balanced = (ci == S_OK || ci == S_FALSE);
    if (FAILED(ci) && ci != RPC_E_CHANGED_MODE) {
        return core::Result<std::string>::fail(
            core::ErrorCode::Internal,
            "COM 初始化失败，关联查询不可用 (CoInitializeEx=" +
                std::to_string(static_cast<long>(ci)) + ")");
    }
    core::Result<std::string> out = core::Result<std::string>::fail(
        core::ErrorCode::Internal, "QueryCurrentDefault 未执行");
    {
        IApplicationAssociationRegistration* reg = nullptr;
        const HRESULT hr = ::CoCreateInstance(
            kAssocRegistrationClsid, nullptr, CLSCTX_INPROC_SERVER,
            kAssocRegistrationIid, reinterpret_cast<void**>(&reg));
        if (SUCCEEDED(hr) && reg != nullptr) {
            LPWSTR raw = nullptr;
            const HRESULT qr = reg->QueryCurrentDefault(
                platform::detail::utf8ToWide(cfg.extension).c_str(),
                AT_FILEEXTENSION, AL_EFFECTIVE, &raw);
            if (SUCCEEDED(qr) && raw != nullptr) {
                out = core::Result<std::string>::ok(
                    platform::detail::wideToUtf8(raw, ::lstrlenW(raw)));
            } else {
                out = core::Result<std::string>::fail(
                    core::ErrorCode::Internal,
                    "QueryCurrentDefault 查询失败 (" + cfg.extension +
                        " (hr=" + std::to_string(static_cast<long>(qr)) + "))");
            }
            if (raw != nullptr) ::CoTaskMemFree(raw);
            reg->Release();
        } else {
            out = core::Result<std::string>::fail(
                core::ErrorCode::Internal,
                "创建 ApplicationAssociationRegistration 失败 (hr=" +
                    std::to_string(static_cast<long>(hr)) + ")");
        }
    }
    if (balanced) ::CoUninitialize();
    return out;
}

} // namespace

core::Result<AssocState> isDefaultForMd(const AssocConfig& config) {
    const core::Result<std::string> current = queryCurrentDefaultProgId(config);
    return core::Result<AssocState>::ok(classifyState(
        current.isOk(), current.valueOr(""), config.progId));
}

core::Result<bool> setAsDefaultForMd(const AssocConfig& config) {
    if (core::Result<void> w = writeRegistration(config); w.isFail()) {
        return core::Result<bool>::fail(w.errorCode(), w.errorMessage());
    }
    // 官方 must-call：登记后必发关联变更广播（spy 缝在此断言）
    shellAssociationsChanged();
    // 写后回查：bool = 归本应用。回查落 Unknown/Others → ok(false)——写入成功
    // ≠ 行为生效（F-07 判定规则），呈现与自愈以本查询回读为准
    const core::Result<AssocState> state = isDefaultForMd(config);
    return core::Result<bool>::ok(state.isOk() &&
                                  state.value() == AssocState::Ours);
}

ShellNotifySink* setShellNotifySink(ShellNotifySink* sink) {
    ShellNotifySink* const old = g_notifySink;
    g_notifySink = sink;
    return old;
}

core::Result<void> removeRegistrationForTest(const AssocConfig& config) {
    // ProgId 树整删（测试专用键；生产永不走此路径——PRD D-4 不注销）
    const std::wstring progKey = progIdSubkey(config, nullptr);
    LSTATUS st = RegDeleteTreeW(HKEY_CURRENT_USER, progKey.c_str());
    if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) {
        return core::Result<void>::fail(
            core::ErrorCode::Internal,
            "删 HKCU 关联 ProgId 键失败 (RegDeleteTreeW=" +
                std::to_string(static_cast<long>(st)) + ")");
    }
    // 扩展名键：先删默认值；整键仅当清空后才删（键内他者内容保留）——
    // RegDeleteKeyW 遇残留子键会失败，视为已达成「不毁他者登记」
    const std::wstring extKey = extSubkey(config);
    st = RegDeleteKeyValueW(HKEY_CURRENT_USER, extKey.c_str(),
                            nullptr);  // null = 键默认值
    if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) {
        return core::Result<void>::fail(
            core::ErrorCode::Internal,
            "删 HKCU 关联扩展名默认值失败 (RegDeleteKeyValueW=" +
                std::to_string(static_cast<long>(st)) + ")");
    }
    st = RegDeleteKeyW(HKEY_CURRENT_USER, extKey.c_str());
    if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND &&
        st != ERROR_ACCESS_DENIED) {
        return core::Result<void>::fail(
            core::ErrorCode::Internal,
            "删 HKCU 关联扩展名键失败 (RegDeleteKeyW=" +
                std::to_string(static_cast<long>(st)) + ")");
    }
    return core::Result<void>::ok();  // 已注销态 = 幂等成功
}

} // namespace platform::file_association
