// src/platform/file_association_win.cpp
// Windows 记事本多扩展文件关联登记：HKCU\Software\Classes 写/查/清（F-18，
// need263 批 4 棒 B；取代 F-06 单 .md 版）。机制（官方，clue4）：ProgId 最小
// 结构 = (Default) 描述 + DefaultIcon + shell\open\command；扩展名默认值指向
// ProgId（多扩展 = 一个 ProgId 树 + 逐扩展默认值，Sumatra RegistryInstaller
// 循环模板）；登记后必须 SHChangeNotify(SHCNE_ASSOCCHANGED) 刷 Shell 缓存；
// 注销 = 删键（F-18 新语义「关闭=撤销已注册关联」，仅回读归己才清，见
// unregisterAll）。查询 = QueryCurrentDefault（AL_EFFECTIVE，UserChoice 优先、
// 回落类默认值），比对 ProgId 串判属（exe 路径变不影响判属）。全程 HKCU。
// 旧注册迁移（F-06 时代 "DevDesk.md"+.md）：开启时检测归己则重写为新 ProgId
// 并清旧 ProgId 树（幂等）。
#include "file_association.h"

#include "autostart.h"        // executablePath()：GetModuleFileNameW 运行时解析（复用，不自写）
#include "file_association_api_map.h" // 系统关联 UI 路径映射锚（launchAdvancedAssociationUI）
#include "file_open_common.h" // platform::detail::utf8ToWide / wideToUtf8 共用编码链

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>  // CoInitializeEx / CoCreateInstance / CoTaskMemFree（LEAN_AND_MEAN 不带 ole2.h）
#include <shellapi.h> // ShellExecuteW（launchAdvancedAssociationUI）
#include <shlobj.h>   // SHChangeNotify / SHCNE_ASSOCCHANGED
#include <shobjidl.h> // IApplicationAssociationRegistration 接口声明

#include <string>
#include <vector>

namespace platform::file_association {
namespace {

// CLSID/IID 手写固定值（不依赖 __uuidof/libuuid）：按本工具链 mingw 头核对
//（shobjidl.h:32607 coclass / :23340 interface）+ HKCR 注册项 + 实机探针
//（写 .mdtest→QueryCurrentDefault 回读 DevDeskTest.md）三方验证。
// 注意：MSVC SDK 的 interface IID 不同（4e5f4267-…），切工具链须重核；
// mingw gcc 的 __uuidof 实测取值不可靠（探针 E_NOINTERFACE），禁用。
// CLSID_ApplicationAssociationRegistration {591209C7-767B-42B2-9FBA-44EE4615F2C7}
constexpr CLSID kAssocRegistrationClsid = {
    0x591209C7, 0x767B, 0x42B2,
    {0x9F, 0xBA, 0x44, 0xEE, 0x46, 0x15, 0xF2, 0xC7}};
// IID_IApplicationAssociationRegistration {4E530B0A-E611-4C77-A3AC-9031D022281B}
constexpr IID kAssocRegistrationIid = {
    0x4E530B0A, 0xE611, 0x4C77,
    {0xA3, 0xAC, 0x90, 0x31, 0xD0, 0x22, 0x28, 0x1B}};

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
std::wstring progIdSubkey(const std::string& progId, const wchar_t* leaf) {
    std::wstring key = std::wstring(kClassesRoot) + L"\\" +
                       platform::detail::utf8ToWide(progId);
    if (leaf != nullptr) key += std::wstring(L"\\") + leaf;
    return key;
}

std::wstring extSubkey(const std::string& extension) {
    return std::wstring(kClassesRoot) + L"\\" +
           platform::detail::utf8ToWide(extension);
}

// 回读 ProgId 是否归本应用（新 ProgId 或 F-06 时代旧 ProgId——旧注册同样
// 视为「归己」，迁移与撤销都认）
bool isOursProgId(const std::string& currentProgId, const AssocConfig& cfg) {
    return currentProgId == cfg.progId ||
           currentProgId == kLegacyMdProgId;
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

// 登记 ProgId 树（一次）+ 逐扩展默认值指向（幂等：同值覆写，无累积）
core::Result<void> writeRegistration(const AssocConfig& cfg) {
    const std::string exe = autostart::executablePath();
    if (exe.empty()) {
        return core::Result<void>::fail(
            core::ErrorCode::Internal,
            "未能解析本进程 exe 路径，文件关联未登记");
    }
    if (core::Result<void> r = setSzValue(
            progIdSubkey(cfg.progId, nullptr), nullptr,
            platform::detail::utf8ToWide(kProgIdDescription));
        r.isFail()) {
        return r;
    }
    if (core::Result<void> r = setSzValue(
            progIdSubkey(cfg.progId, L"DefaultIcon"), nullptr,
            platform::detail::utf8ToWide(buildDefaultIconValue(exe)));
        r.isFail()) {
        return r;
    }
    if (core::Result<void> r = setSzValue(
            progIdSubkey(cfg.progId, L"shell\\open\\command"), nullptr,
            platform::detail::utf8ToWide(buildOpenCommandValue(exe)));
        r.isFail()) {
        return r;
    }
    for (const std::string& ext : cfg.extensions) {
        if (core::Result<void> r = setSzValue(
                extSubkey(ext), nullptr,
                platform::detail::utf8ToWide(cfg.progId));
            r.isFail()) {
            return r;
        }
    }
    return core::Result<void>::ok();
}

// 删 ProgId 树（RegDeleteTreeW；键不存在 = 幂等成功）
core::Result<void> deleteProgIdTree(const std::string& progId) {
    const std::wstring key = progIdSubkey(progId, nullptr);
    const LSTATUS st = RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
    if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) {
        return core::Result<void>::fail(
            core::ErrorCode::Internal,
            "删 HKCU 关联 ProgId 键失败 (RegDeleteTreeW=" +
                std::to_string(static_cast<long>(st)) + ")");
    }
    return core::Result<void>::ok();
}

// 查询当前生效默认 ProgId 串；fail = 查询失败（调用方转 Unknown，绝不映射 Others）
core::Result<std::string> queryCurrentDefaultProgId(const std::string& extension) {
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
                platform::detail::utf8ToWide(extension).c_str(),
                AT_FILEEXTENSION, AL_EFFECTIVE, &raw);
            if (SUCCEEDED(qr) && raw != nullptr) {
                out = core::Result<std::string>::ok(
                    platform::detail::wideToUtf8(raw, ::lstrlenW(raw)));
            } else {
                out = core::Result<std::string>::fail(
                    core::ErrorCode::Internal,
                    "QueryCurrentDefault 查询失败 (" + extension +
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

core::Result<AssocState> isDefaultFor(const std::string& extension,
                                      const AssocConfig& config) {
    const core::Result<std::string> current = queryCurrentDefaultProgId(extension);
    return core::Result<AssocState>::ok(classifyState(
        current.isOk(), current.valueOr(""), config.progId));
}

core::Result<AssocState> overallState(const AssocConfig& config) {
    std::vector<AssocState> states;
    states.reserve(config.extensions.size());
    for (const std::string& ext : config.extensions) {
        const core::Result<std::string> current = queryCurrentDefaultProgId(ext);
        states.push_back(classifyState(current.isOk(), current.valueOr(""),
                                       config.progId));
    }
    return core::Result<AssocState>::ok(aggregateAssocStates(states));
}

core::Result<bool> setAsDefaultFor(const AssocConfig& config) {
    // 旧注册迁移检测（F-06 "DevDesk.md"+.md）：先查 .md 归属——写入后它已翻新，
    // 旧判据必须取写入前快照。检测归己才迁移（他者占用不动）。
    bool legacyOwned = false;
    if (core::Result<std::string> legacy = queryCurrentDefaultProgId(".md");
        legacy.isOk() && legacy.value() == kLegacyMdProgId) {
        legacyOwned = true;
    }
    if (core::Result<void> w = writeRegistration(config); w.isFail()) {
        return core::propagate<bool>(w);
    }
    if (legacyOwned) {
        // 旧 ProgId 树清理：扩展名默认值已被本次写入重写为新 ProgId，仅剩
        // 孤儿 ProgId 树可删（幂等：不存在 = FILE_NOT_FOUND ok）
        if (core::Result<void> r = deleteProgIdTree(kLegacyMdProgId);
            r.isFail()) {
            return core::propagate<bool>(r);
        }
    }
    // 官方 must-call：登记后必发关联变更广播（spy 缝在此断言整批恰一次）
    shellAssociationsChanged();
    // 写后回查：bool = 归本应用。回查落 Unknown/Others → ok(false)——写入成功
    // ≠ 行为生效（F-07 判定规则），呈现与自愈以本查询回读为准
    return core::Result<bool>::ok(
        overallState(config).valueOr(AssocState::Unknown) == AssocState::Ours);
}

core::Result<void> unregisterAll(const AssocConfig& config) {
    bool clearedAny = false;
    for (const std::string& ext : config.extensions) {
        // 仅当回读归己（新/旧 ProgId）才清——不误删用户后选的他者
        const core::Result<std::string> current = queryCurrentDefaultProgId(ext);
        if (current.isFail() || !isOursProgId(current.value(), config)) {
            continue;
        }
        const std::wstring extKey = extSubkey(ext);
        const LSTATUS st = RegDeleteKeyValueW(HKEY_CURRENT_USER, extKey.c_str(),
                                              nullptr);  // null = 键默认值
        if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) {
            return core::Result<void>::fail(
                core::ErrorCode::Internal,
                "撤销 HKCU 关联扩展名默认值失败 (" + ext +
                    " (RegDeleteKeyValueW=" +
                    std::to_string(static_cast<long>(st)) + "))");
        }
        clearedAny = true;
    }
    if (clearedAny) {
        // 清掉仍归己的扩展后，我们的 ProgId 树（新/旧）随之无扩展指向，整树删
        //（自建键，归属无歧义）
        if (core::Result<void> r = deleteProgIdTree(config.progId); r.isFail()) {
            return r;
        }
        if (core::Result<void> r = deleteProgIdTree(kLegacyMdProgId); r.isFail()) {
            return r;
        }
        shellAssociationsChanged();
    }
    return core::Result<void>::ok();  // 未登记/已注销态 = 幂等成功
}

core::Result<void> launchAdvancedAssociationUI(const std::string& extension) {
    (void)extension;  // 系统页不按扩展定位；参数留扩展点（api_map 锚唯一权威）
    const HINSTANCE r = ::ShellExecuteW(nullptr, L"open",
                                        platform::detail::utf8ToWide(
                                            mac_api::kAdvancedAssociationUiUri)
                                            .c_str(),
                                        nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        return core::Result<void>::fail(
            core::ErrorCode::Internal,
            "拉起系统默认应用设置页失败 (ShellExecuteW SE_ERR=" +
                std::to_string(reinterpret_cast<INT_PTR>(r) & 0xFFFF) + ")");
    }
    return core::Result<void>::ok();
}

ShellNotifySink* setShellNotifySink(ShellNotifySink* sink) {
    ShellNotifySink* const old = g_notifySink;
    g_notifySink = sink;
    return old;
}

core::Result<void> removeRegistrationForTest(const AssocConfig& config) {
    // ProgId 树整删（测试专用键）
    if (core::Result<void> r = deleteProgIdTree(config.progId); r.isFail()) {
        return r;
    }
    // 逐扩展：先删默认值；整键仅当清空后才删——残留他者子键
    //（ERROR_KEY_HAS_CHILDREN，旧版系统表现为 ERROR_ACCESS_DENIED）与他者内容
    // 一律保留，清掉我们的默认值即达成清理目标，不算失败
    for (const std::string& ext : config.extensions) {
        const std::wstring extKey = extSubkey(ext);
        LSTATUS st = RegDeleteKeyValueW(HKEY_CURRENT_USER, extKey.c_str(),
                                        nullptr);  // null = 键默认值
        if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) {
            return core::Result<void>::fail(
                core::ErrorCode::Internal,
                "删 HKCU 关联扩展名默认值失败 (RegDeleteKeyValueW=" +
                    std::to_string(static_cast<long>(st)) + ")");
        }
        st = RegDeleteKeyW(HKEY_CURRENT_USER, extKey.c_str());
        if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND &&
            st != ERROR_ACCESS_DENIED && st != ERROR_KEY_HAS_CHILDREN) {
            return core::Result<void>::fail(
                core::ErrorCode::Internal,
                "删 HKCU 关联扩展名键失败 (RegDeleteKeyW=" +
                    std::to_string(static_cast<long>(st)) + ")");
        }
    }
    return core::Result<void>::ok();  // 已注销态 = 幂等成功
}

} // namespace platform::file_association
