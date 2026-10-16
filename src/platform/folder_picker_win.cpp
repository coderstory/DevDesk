// src/platform/folder_picker_win.cpp
// Windows 实现：IFileDialog COM（FOS_PICKFOLDERS）
#include "folder_picker.h"

#include <Objbase.h>     // CoInitializeEx, CoUninitialize
#include <ShlObj.h>      // IFileDialog, CLSID_FileOpenDialog, FOS_PICKFOLDERS
#include <Shlwapi.h>     // SHCreateItemFromParsingName（MinGW 需要链接 shell32 与 shlwapi）

#include <string>
#include <utility>

namespace platform::folderpicker {

core::Result<std::string> pickFolder(const std::string& title,
                                     const std::string& startDir) {
    // COM 初始化：调用线程 CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)。
    // 若线程已是 STA/其他模式（RPC_E_CHANGED_MODE），跳过配对的 CoUninitialize。
    const HRESULT initHr =
        ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comInitialized = SUCCEEDED(initHr);
    if (!comInitialized && initHr != RPC_E_CHANGED_MODE) {
        return core::Result<std::string>::fail(
            core::ErrorCode::Internal,
            "COM 初始化失败（HRESULT " + std::to_string(initHr) + "）");
    }

    core::Result<std::string> result =
        core::Result<std::string>::fail(core::ErrorCode::Internal,
                                        "用户取消选择"); // 默认失败

    // 创建 IFileDialog 实例
    IFileDialog* pfd = nullptr;
    HRESULT hr = ::CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&pfd));
    if (FAILED(hr) || pfd == nullptr) {
        if (comInitialized) ::CoUninitialize();
        return core::Result<std::string>::fail(
            core::ErrorCode::Internal,
            "创建目录选择对话框失败（HRESULT " + std::to_string(hr) + "）");
    }

    // 设为目录选择模式
    DWORD dwOptions = 0;
    hr = pfd->GetOptions(&dwOptions);
    if (SUCCEEDED(hr)) {
        dwOptions |= FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM;
        pfd->SetOptions(dwOptions);
    }

    // 设置标题
    if (!title.empty()) {
        const int titleLen = ::MultiByteToWideChar(
            CP_UTF8, 0, title.c_str(), static_cast<int>(title.size()),
            nullptr, 0);
        if (titleLen > 0) {
            std::wstring wtitle(static_cast<size_t>(titleLen), L'\0');
            ::MultiByteToWideChar(
                CP_UTF8, 0, title.c_str(), static_cast<int>(title.size()),
                &wtitle[0], titleLen);
            pfd->SetTitle(wtitle.c_str());
        }
    }

    // 设置初始目录
    if (!startDir.empty()) {
        const int dirLen = ::MultiByteToWideChar(
            CP_UTF8, 0, startDir.c_str(), static_cast<int>(startDir.size()),
            nullptr, 0);
        if (dirLen > 0) {
            std::wstring wdir(static_cast<size_t>(dirLen), L'\0');
            ::MultiByteToWideChar(
                CP_UTF8, 0, startDir.c_str(), static_cast<int>(startDir.size()),
                &wdir[0], dirLen);
            IShellItem* psiFolder = nullptr;
            hr = ::SHCreateItemFromParsingName(wdir.c_str(), nullptr,
                                               IID_PPV_ARGS(&psiFolder));
            if (SUCCEEDED(hr) && psiFolder != nullptr) {
                pfd->SetDefaultFolder(psiFolder);
                psiFolder->Release();
            }
        }
    }

    // 显示对话框
    hr = pfd->Show(nullptr);
    if (SUCCEEDED(hr)) {
        // 用户选择了目录
        IShellItem* psiResult = nullptr;
        hr = pfd->GetResult(&psiResult);
        if (SUCCEEDED(hr) && psiResult != nullptr) {
            PWSTR pszPath = nullptr;
            hr = psiResult->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
            if (SUCCEEDED(hr) && pszPath != nullptr) {
                const int pathLen = ::WideCharToMultiByte(
                    CP_UTF8, 0, pszPath, -1, nullptr, 0, nullptr, nullptr);
                if (pathLen > 0) {
                    std::string utf8Path(static_cast<size_t>(pathLen - 1), '\0');
                    ::WideCharToMultiByte(
                        CP_UTF8, 0, pszPath, -1, &utf8Path[0], pathLen,
                        nullptr, nullptr);
                    result = core::Result<std::string>::ok(std::move(utf8Path));
                }
                ::CoTaskMemFree(pszPath);
            }
            psiResult->Release();
        }
    }
    // hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) → 保留默认的 fail（"用户取消选择"）

    pfd->Release();

    if (comInitialized) ::CoUninitialize();
    return result;
}

} // namespace platform::folderpicker