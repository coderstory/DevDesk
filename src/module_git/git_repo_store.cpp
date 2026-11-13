// src/module_git/git_repo_store.cpp
#include "git_repo_store.h"

#include "core/logging.h"
#include "core/paths.h"
#include "platform/git/git_libgit2.h"
#include "platform/json.h"

#include <fstream>
#include <system_error>

namespace module::git {

namespace {

constexpr const char* kKeyRepoPath = "lastRepoPath";

// 小文件直读直写（json 封装只管字符串，无文件 IO 面——github_sync_config 同口径）
std::string readFileOrEmpty(const std::filesystem::path& file, bool& ok) {
    std::ifstream in(file, std::ios::binary);
    if (!in.good()) {
        ok = false;
        return {};
    }
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    ok = in.good() || in.eof();
    return text;
}

} // namespace

std::filesystem::path GitRepoStore::storeFile() {
    return ::core::paths::appDataDir() / "git_manager.json";
}

GitRepoMemory GitRepoStore::load() {
    GitRepoMemory out;
    std::error_code ec;
    if (!std::filesystem::exists(storeFile(), ec)) {
        return out;  // 无记忆 = 首启正常态，不记日志
    }
    bool ok = false;
    const std::string text = readFileOrEmpty(storeFile(), ok);
    if (!ok) {
        LOG_WARN("GitRepoStore: 记忆仓文件不可读（按无记忆处理）");
        return out;
    }
    auto parsed = ::platform::json::parse(text);
    if (parsed.isFail() || !parsed.value().isObject()) {
        LOG_WARN("GitRepoStore: 记忆仓内容损坏（按无记忆处理）");
        return out;
    }
    const auto obj = parsed.value().asObject();
    if (obj.has(kKeyRepoPath) && obj.get(kKeyRepoPath).isString()) {
        out.lastRepoPath = obj.get(kKeyRepoPath).asString();
    }
    return out;
}

core::Result<void> GitRepoStore::save(const GitRepoMemory& memory) {
    ::platform::json::JsonObject obj;
    obj.set(kKeyRepoPath, ::platform::json::makeString(memory.lastRepoPath));
    auto text = ::platform::json::stringify(
        ::platform::json::JsonValue(obj), /*pretty=*/true);
    if (text.isFail()) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "记忆仓序列化失败: " + text.errorMessage());
    }
    std::error_code mkEc;
    std::filesystem::create_directories(storeFile().parent_path(), mkEc);
    std::ofstream out(storeFile(), std::ios::binary | std::ios::trunc);
    if (!out.good()) {
        return core::Result<void>::fail(core::ErrorCode::FilePermissionDenied,
                                        "记忆仓文件不可写: " +
                                            platform::git::pathToUtf8(storeFile()));
    }
    out << text.value();
    out.close();
    if (!out.good()) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "记忆仓写入未完成");
    }
    return core::Result<void>::ok();
}

core::Result<void> GitRepoStore::clear() { return save(GitRepoMemory{}); }

bool GitRepoStore::hasMemory() { return !load().lastRepoPath.empty(); }

} // namespace module::git
