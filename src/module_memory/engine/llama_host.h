// src/module_memory/engine/llama_host.h
// 本机 embedding 服务（llama-server）托管：定位/下载/参数构造（S2 批次②⑤）。
// 定位与参数构造是纯逻辑（单测钉死）；下载与拉起是真副作用（后台线程调用）。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/result.h"

namespace module::memory::engine {

// llama-server 可执行名（平台差异：Windows 带 .exe）；实现按平台拆。
std::string llamaServerBinaryName();

// exe 同级随包 assets 根：<executableDir>/assets（core::paths::executableDir 已按
// 平台拆分——win GetModuleFileNameW / mac .app=Contents/MacOS，与图标 resolveIconPath
// 同落点）。executableDir 未知时返回空 path——调用方判空跳过包内序，绝不退化成相对 cwd。
std::filesystem::path bundledAssetsDir();

// 定位结果：exePath 空 = 未命中；source = "env"/"pkg"/"dataroot"/"path"/""
struct LlamaLocate {
    std::filesystem::path exePath;
    const char* source = "";
};

// 定位序（need:202 F-01）：DEVDESK_LLAMA_DIR → <assetsDir>/llama/bin/<exe> →
// <dataDir>/llama/bin/<exe> → PATH。assetsDir 传空 = 跳过包内序（纯函数，单测钉死）。
LlamaLocate locateLlamaServerBetween(const std::filesystem::path& assetsDir,
                                     const std::filesystem::path& dataDir);

// 便捷包装：包内序用 bundledAssetsDir()。找不到 exePath 为空（面板显引导，不静默起进程）。
LlamaLocate locateLlamaServerDetailed(const std::filesystem::path& dataDir);

// 旧签名薄包装（兼容既有调用方）
std::filesystem::path locateLlamaServer(const std::filesystem::path& dataDir);

// 模型定位结果：source = "pkg"/"dataroot"/""
struct EmbedModelLocate {
    std::filesystem::path modelPath;
    const char* source = "";
};

// 模型定位序（need:202 F-02）：<assetsDir>/models/<gguf> → <dataDir>/models/<gguf>。
// 0 字节文件视同缺失（同既有重下口径）。assetsDir 传空 = 跳过包内序（纯函数）。
EmbedModelLocate locateEmbedModelBetween(const std::filesystem::path& assetsDir,
                                         const std::filesystem::path& dataDir);

// 便捷包装：包内序用 bundledAssetsDir()
EmbedModelLocate locateEmbedModel(const std::filesystem::path& dataDir);

// 模型下载落点（恒数据根，绝不写进 .app/Program Files）：
// <dataDir>/models/<kEmbedModel 文件名>（随 DEVDESK_DATA_ROOT 隔离）
std::filesystem::path llamaModelPath(const std::filesystem::path& dataDir);

// F-03 诊断文案：四序逐项检查结果（env / 包内 / 数据根 / PATH），全空时给重装与
// DEVDESK_LLAMA_DIR 指路。纯只读文本。
std::string llamaLocateDiagnosis(const std::filesystem::path& dataDir);

// 拉起参数（纯函数）：--host 127.0.0.1 --port <port> -m <model> --embeddings（只开嵌入档）
std::vector<std::string> buildLlamaServerArgs(const std::filesystem::path& exe,
                                              const std::filesystem::path& modelPath,
                                              int port);

// 模型缺失时下载到 llamaModelPath（流式落盘，已存在且非空直接跳过）。
// 同步阻塞 + 网络重活：只允许后台线程调用；失败返回 fail（面板给可读提示与重试）。
core::Result<void> ensureEmbedModelDownloaded(
    const std::filesystem::path& dataDir);

} // namespace module::memory::engine
