// src/module_memory/engine/llama_host.cpp
#include "llama_host.h"

#include "telemem_config.h"
#include "core/logging.h"
#include "platform/exec_capture.h"
#include "platform/http_client.h"

#include <cstdio>
#include <fstream>
#include <cstdlib>
#include <system_error>

namespace module::memory::engine {

namespace {

// 模型仓库定案：bge-small-zh-v1.5 的 Q8_0 量化 GGUF（维度 512 建索引前定死）。
// HF 文件名为小写 q8_0（仓内实测只有小写形态，大写 Q8_0 resolve 恒 404——池 3/G-3）。
constexpr const char* kEmbedModelGguf = "bge-small-zh-v1.5-q8_0.gguf";
constexpr const char* kEmbedModelUrl =
    "https://huggingface.co/CompendiumLabs/bge-small-zh-v1.5-gguf/resolve/main/"
    "bge-small-zh-v1.5-q8_0.gguf";
// 下载空闲超时：低于 8KB/s 持续 30s 判死（总墙钟不设限——百 MB 级模型慢网合法）
constexpr long kDownloadLowSpeedBytes = 8 * 1024;
constexpr long kDownloadLowSpeedSecs = 30;

std::filesystem::path exeNameIn(const std::filesystem::path& dir) {
    return dir / llamaServerBinaryName();
}

} // namespace

std::filesystem::path locateLlamaServer(const std::filesystem::path& dataDir) {
    // 1) 显式环境覆盖（诊断/自备包）
    if (const char* envDir = std::getenv("DEVDESK_LLAMA_DIR")) {
        if (*envDir != '\0') {
            const std::filesystem::path p = exeNameIn(envDir);
            std::error_code ec;
            if (std::filesystem::exists(p, ec) && !ec) return p;
        }
    }
    // 2) 数据目录（DevDesk 托管安装的落点）
    {
        const std::filesystem::path p = exeNameIn(dataDir / "llama" / "bin");
        std::error_code ec;
        if (std::filesystem::exists(p, ec) && !ec) return p;
    }
    // 3) PATH（平台探测：SearchPathW / 逐目录 stat）
    if (platform::executableOnPath(llamaServerBinaryName())) {
        return std::filesystem::path(llamaServerBinaryName());
    }
    return {};
}

std::filesystem::path llamaModelPath(const std::filesystem::path& dataDir) {
    return dataDir / "models" / kEmbedModelGguf;
}

std::vector<std::string> buildLlamaServerArgs(
    const std::filesystem::path& exe, const std::filesystem::path& modelPath,
    int port) {
    return {exe.string(),
            "--host",
            "127.0.0.1",
            "--port",
            std::to_string(port),
            "-m",
            modelPath.string(),
            "--embeddings"};
}

core::Result<void> ensureEmbedModelDownloaded(
    const std::filesystem::path& dataDir) {
    const std::filesystem::path target = llamaModelPath(dataDir);
    std::error_code ec;
    if (std::filesystem::exists(target, ec) && !ec) {
        if (const auto size = std::filesystem::file_size(target, ec); !ec && size > 0) {
            return core::Result<void>::ok();  // 已就位，不重下
        }
    }
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        return core::Result<void>::fail(
            core::ErrorCode::FileWriteFailed, "建模型目录失败: " + ec.message());
    }

    platform::http::Request req;
    req.method = "GET";
    req.url = kEmbedModelUrl;
    req.timeoutMs = 0;  // 流式下载不看总墙钟，空闲超时兜底
    req.lowSpeedBytesPerSec = kDownloadLowSpeedBytes;
    req.lowSpeedTimeSec = kDownloadLowSpeedSecs;
    auto stream = platform::http::sendStreaming(req);
    if (!stream.isOk()) {
        return core::Result<void>::fail(
            core::ErrorCode::NetworkUnreachable, "下载连接失败: " + stream.errorMessage());
    }

    // 临时文件写满后一步替换：半截模型不冒充完整文件
    const std::filesystem::path tmp = target.string() + ".part";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
        return core::Result<void>::fail(
            core::ErrorCode::FileWriteFailed, "写模型临时文件失败: " + tmp.string());
    }
    std::string chunk;
    std::string err;
    bool streamError = false;
    for (;;) {
        const platform::http::ChunkResult step =
            stream.value()->readChunk(chunk, err);
        if (step == platform::http::ChunkResult::More) {
            out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            if (!out) {
                stream.value()->cancel();
                out.close();
                std::filesystem::remove(tmp, ec);
                return core::Result<void>::fail(core::ErrorCode::FileWriteFailed, "写模型文件中断（磁盘满?）");
            }
            continue;
        }
        if (step == platform::http::ChunkResult::Error) {
            streamError = true;
        }
        break;  // End 或 Error：正文结束
    }
    out.close();
    const int status = stream.value()->statusCode();
    if (streamError) {
        std::filesystem::remove(tmp, ec);
        return core::Result<void>::fail(
            core::ErrorCode::NetworkTimeout, "下载中断: " + err);
    }
    if (status != 200) {
        std::filesystem::remove(tmp, ec);
        return core::Result<void>::fail(
            core::ErrorCode::HttpError, "下载失败: HTTP " + std::to_string(status));
    }
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return core::Result<void>::fail(
            core::ErrorCode::FileWriteFailed, "模型文件就位失败: " + ec.message());
    }
    LOG_INFO("embedding 模型已下载: " + target.string());
    return core::Result<void>::ok();
}

} // namespace module::memory::engine
