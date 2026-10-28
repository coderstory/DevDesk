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

// 定位序：DEVDESK_LLAMA_DIR 环境变量 → <dataDir>/llama/bin/<exe> → PATH。
// 找不到返回空 path（面板显「需安装 embedding 服务」引导，不静默起进程）。
std::filesystem::path locateLlamaServer(const std::filesystem::path& dataDir);

// 模型文件落点：<dataDir>/models/<kEmbedModel 文件名>（数据目录内，随 DEVDESK_DATA_ROOT 隔离）
std::filesystem::path llamaModelPath(const std::filesystem::path& dataDir);

// 拉起参数（纯函数）：--host 127.0.0.1 --port <port> -m <model> --embeddings（只开嵌入档）
std::vector<std::string> buildLlamaServerArgs(const std::filesystem::path& exe,
                                              const std::filesystem::path& modelPath,
                                              int port);

// 模型缺失时下载到 llamaModelPath（流式落盘，已存在且非空直接跳过）。
// 同步阻塞 + 网络重活：只允许后台线程调用；失败返回 fail（面板给可读提示与重试）。
core::Result<void> ensureEmbedModelDownloaded(
    const std::filesystem::path& dataDir);

} // namespace module::memory::engine
