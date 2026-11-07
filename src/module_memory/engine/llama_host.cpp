// src/module_memory/engine/llama_host.cpp
#include "llama_host.h"

#include "telemem_config.h"
#include "core/logging.h"
#include "core/paths.h"
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

// 存在且非 0 字节（0 字节视同缺失，与既有重下口径一致）
bool usableFile(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || ec) return false;
    const auto size = std::filesystem::file_size(p, ec);
    return !ec && size > 0;
}

} // namespace

std::filesystem::path bundledAssetsDir() {
    const auto exeDir = core::paths::executableDir();
    if (exeDir.empty()) return {};
    return exeDir / "assets";
}

LlamaLocate locateLlamaServerBetween(const std::filesystem::path& assetsDir,
                                     const std::filesystem::path& dataDir) {
    // 1) 显式环境覆盖（诊断/自备包）——兼容两种指向：bin 目录本体（历史口径）
    //    或含 llama/bin 的根（与数据根同布局，测试/装机脚本常见形态）
    if (const char* envDir = std::getenv("DEVDESK_LLAMA_DIR")) {
        if (*envDir != '\0') {
            std::error_code ec;
            const std::filesystem::path direct = exeNameIn(envDir);
            if (std::filesystem::exists(direct, ec) && !ec) return {direct, "env"};
            ec.clear();
            const std::filesystem::path nested =
                exeNameIn(std::filesystem::path(envDir) / "llama" / "bin");
            if (std::filesystem::exists(nested, ec) && !ec) return {nested, "env"};
        }
    }
    // 2) exe 同级随包 assets（装机即达，零下载）
    if (!assetsDir.empty()) {
        const std::filesystem::path p = exeNameIn(assetsDir / "llama" / "bin");
        std::error_code ec;
        if (std::filesystem::exists(p, ec) && !ec) return {p, "pkg"};
    }
    // 3) 数据目录（DevDesk 托管安装的落点，旧装机向后兼容）
    {
        const std::filesystem::path p = exeNameIn(dataDir / "llama" / "bin");
        std::error_code ec;
        if (std::filesystem::exists(p, ec) && !ec) return {p, "dataroot"};
    }
    // 4) 显式绝对目录（bug182：macOS GUI 进程 PATH 只给系统目录，brew 装的
    //    llama-server 在 Finder/Dock 拉起下 PATH 序恒 miss——与 bug202 Python
    //    候选同款处理；目录不存在 stat 天然落选，Windows 无害）
    for (const char* dir : {"/opt/homebrew/bin", "/usr/local/bin"}) {
        std::error_code ec;
        const std::filesystem::path p = exeNameIn(std::filesystem::path(dir));
        if (std::filesystem::exists(p, ec) && !ec) return {p, "brew"};
    }
    // 5) PATH（平台探测：SearchPathW / 逐目录 stat）
    if (platform::executableOnPath(llamaServerBinaryName())) {
        return {std::filesystem::path(llamaServerBinaryName()), "path"};
    }
    return {};
}

LlamaLocate locateLlamaServerDetailed(const std::filesystem::path& dataDir) {
    return locateLlamaServerBetween(bundledAssetsDir(), dataDir);
}

std::filesystem::path locateLlamaServer(const std::filesystem::path& dataDir) {
    return locateLlamaServerDetailed(dataDir).exePath;
}

EmbedModelLocate locateEmbedModelBetween(
    const std::filesystem::path& assetsDir, const std::filesystem::path& dataDir) {
    // 1) exe 同级随包 assets（assets 序优先；数据根文件不动）
    if (!assetsDir.empty()) {
        const std::filesystem::path p = assetsDir / "models" / kEmbedModelGguf;
        if (usableFile(p)) return {p, "pkg"};
    }
    // 2) 数据根（旧装机兼容，存在即用不重下）
    {
        const std::filesystem::path p = dataDir / "models" / kEmbedModelGguf;
        if (usableFile(p)) return {p, "dataroot"};
    }
    return {};
}

EmbedModelLocate locateEmbedModel(const std::filesystem::path& dataDir) {
    return locateEmbedModelBetween(bundledAssetsDir(), dataDir);
}

std::filesystem::path llamaModelPath(const std::filesystem::path& dataDir) {
    return dataDir / "models" / kEmbedModelGguf;
}

std::string llamaLocateDiagnosis(const std::filesystem::path& dataDir) {
    // 四序逐项给事实（不短路），全空时指路重装 / env
    const char* envDir = std::getenv("DEVDESK_LLAMA_DIR");
    std::string envState = (envDir != nullptr && *envDir != '\0')
                               ? "已设但未命中"
                               : "未设";
    if (envDir != nullptr && *envDir != '\0') {
        std::error_code ec;
        if (std::filesystem::exists(exeNameIn(envDir), ec) && !ec)
            envState = "命中";  // 定位已被 env 序短路时不会走到这，防御性给全事实
    }
    const auto assets = bundledAssetsDir();
    const std::string pkgState = assets.empty()
                                     ? "exe 目录未知"
                                     : (std::filesystem::exists(
                                            exeNameIn(assets / "llama" / "bin"))
                                            ? "命中"
                                            : "缺失");
    const std::string dataState =
        std::filesystem::exists(exeNameIn(dataDir / "llama" / "bin")) ? "命中"
                                                                     : "缺失";
    const bool brewHit = [&] {
        std::error_code ec;
        return (std::filesystem::exists(exeNameIn("/opt/homebrew/bin"), ec) && !ec) ||
               (std::filesystem::exists(exeNameIn("/usr/local/bin"), ec) && !ec);
    }();
    const std::string pathState =
        (platform::executableOnPath(llamaServerBinaryName()) || brewHit) ? "命中"
                                                                        : "未命中";
    return "llama-server 未找到（四序检查：DEVDESK_LLAMA_DIR 环境变量=" +
           envState + "；包内 assets/llama/bin=" + pkgState + "；数据根 memory/llama/bin=" +
           dataState + "；PATH/brew=" + pathState +
           "）——可重装随包 assets 的安装包，或设 DEVDESK_LLAMA_DIR 指向本机 llama/bin";
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
