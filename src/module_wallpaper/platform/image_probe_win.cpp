// src/module_wallpaper/platform/image_probe_win.cpp
// WIC 头解码：CreateDecoderFromFilename + 首帧 GetSize。只读元数据头，不解码像素。
// 失败一律返回 known=false（调用方语义 = 尺寸未知不过滤，见 image_probe.h）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <initguid.h>
#include <wincodec.h>

#include "module_wallpaper/platform/image_probe.h"

#include <cstddef>
#include <string>
#include <vector>

namespace module::wallpaper {
namespace {

std::wstring utf8ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0) return std::wstring();
    std::vector<wchar_t> buffer(static_cast<std::size_t>(size));
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, buffer.data(), size);
    return std::wstring(buffer.data());
}

} // namespace

ProbedImageSize probeImageSize(const std::string& path) {
    ProbedImageSize result;
    const std::wstring wide = utf8ToWide(path);
    if (wide.empty()) return result;

    // COM 套间按调用进出配对；扫描线程独享本次初始化（MTA）。已初始化为 STA 的
    // 线程会拿到 RPC_E_CHANGED_MODE → 按失败处理返回 unknown，不换套间硬闯。
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return result;

    IWICImagingFactory* factory = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IWICImagingFactory,
                                   reinterpret_cast<void**>(&factory))) &&
        factory != nullptr) {
        IWICBitmapDecoder* decoder = nullptr;
        if (SUCCEEDED(factory->CreateDecoderFromFilename(
                wide.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                &decoder)) &&
            decoder != nullptr) {
            IWICBitmapFrameDecode* frame = nullptr;
            if (SUCCEEDED(decoder->GetFrame(0, &frame)) && frame != nullptr) {
                UINT width = 0;
                UINT height = 0;
                if (SUCCEEDED(frame->GetSize(&width, &height)) && width > 0 && height > 0) {
                    result.known = true;
                    result.width = static_cast<int>(width);
                    result.height = static_cast<int>(height);
                }
                frame->Release();
            }
            decoder->Release();
        }
        factory->Release();
    }
    CoUninitialize();
    return result;
}

} // namespace module::wallpaper
