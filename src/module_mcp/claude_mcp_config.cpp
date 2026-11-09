// src/module_mcp/claude_mcp_config.cpp
#include "claude_mcp_config.h"

#include "core/atomic_write.h"
#include "core/fs_text.h"
#include "platform/json.h"

#include <functional>

namespace module::mcp::cc {

namespace {

namespace pj = ::platform::json;

// mcpServers 对象里是否有本 server 的键（mcpServers 缺失/非对象 = 没有）
bool serverPresent(const pj::JsonValue& root) {
    if (!root.isObject()) return false;
    const pj::JsonValue servers = root.asObject().get("mcpServers");
    return servers.isObject() && servers.asObject().has(kServerName);
}

// 审计 P1-5：mcpServers 键存在但非对象（数组/字符串/数字）→ fail 拒写。
// 缺失或显式 null → 放行（调用方新建对象）。防「静默替换成空对象吞掉原值」。
core::Result<void> ensureServersObjectIfPresent(const pj::JsonObject& root) {
    const pj::JsonValue existing = root.get("mcpServers");
    if (existing.isNull() || existing.isObject()) return core::Result<void>::ok();
    return core::Result<void>::fail(
        core::ErrorCode::JsonParseError,
        "~/.claude.json 的 mcpServers 是非对象形态（数组/字符串），拒绝写入以免吞掉原值；"
        "请先手工修正该键后再安装");
}

// 顶层对象校验（复用 hooks_settings 同款错误口径）
core::Result<pj::JsonObject> parseRoot(const std::string& jsonText,
                                      const char* what) {
    const auto parsed = pj::parse(jsonText);
    if (parsed.isFail()) {
        return core::Result<pj::JsonObject>::fail(core::ErrorCode::JsonParseError,
                                                  std::string(what) + "非法 JSON");
    }
    if (!parsed.value().isObject()) {
        return core::Result<pj::JsonObject>::fail(
            core::ErrorCode::JsonParseError, std::string(what) + "顶层非对象");
    }
    return core::Result<pj::JsonObject>::ok(parsed.value().asObject());
}

} // namespace

std::filesystem::path backupFile(const std::filesystem::path& file) {
    std::filesystem::path bak = file;
    bak += ".bak-devdesk";
    return bak;
}

ConfigState readConfigState(const std::filesystem::path& file) {
    ConfigState st;
    std::error_code ec;
    st.filePresent = std::filesystem::exists(file, ec) && !ec;
    if (!st.filePresent) return st;
    bool readFailed = false;
    const std::string text = core::readTextFile(file, &readFailed);
    if (readFailed) {
        st.error = "读取 " + file.string() + " 失败";
        return st;
    }
    const auto root = parseRoot(text, "~/.claude.json");
    if (root.isFail()) {
        st.error = root.errorMessage();
        return st;
    }
    st.installed = serverPresent(pj::JsonValue(root.value()));
    return st;
}

core::Result<std::string> applyServerEntry(const std::string& jsonText,
                                          const std::string& url,
                                          bool* changed) {
    if (changed != nullptr) *changed = false;
    auto rootResult = parseRoot(jsonText, "~/.claude.json");
    if (rootResult.isFail()) {
        return core::Result<std::string>::fail(rootResult.errorCode(),
                                              rootResult.errorMessage());
    }
    pj::JsonObject root = rootResult.value();
    const auto serversGuard = ensureServersObjectIfPresent(root);
    if (serversGuard.isFail()) {
        return core::Result<std::string>::fail(serversGuard.errorCode(),
                                               serversGuard.errorMessage());
    }
    const pj::JsonValue existing = root.get("mcpServers");
    pj::JsonObject servers =
        existing.isObject() ? existing.asObject() : pj::JsonObject();
    if (servers.has(kServerName)) {
        return core::Result<std::string>::ok(jsonText);  // 幂等 no-op
    }
    // 条目形态（claude mcp add --transport http 实测产物）：只两个字段
    pj::JsonObject entry;
    entry.set("type", pj::makeString("http"));
    entry.set("url", pj::makeString(url));
    servers.set(kServerName, pj::JsonValue(entry));
    root.set("mcpServers", pj::JsonValue(servers));

    // 与原文件同档：两空格缩进（yyjson YYJSON_WRITE_PRETTY_TWO_SPACES）
    const auto out = pj::stringify(pj::JsonValue(root), true);
    if (out.isFail()) {
        return core::Result<std::string>::fail(out.errorCode(), out.errorMessage());
    }
    if (changed != nullptr) *changed = true;
    return core::Result<std::string>::ok(out.value());
}

core::Result<std::string> removeServerEntry(const std::string& jsonText,
                                           bool* changed) {
    if (changed != nullptr) *changed = false;
    auto rootResult = parseRoot(jsonText, "~/.claude.json");
    if (rootResult.isFail()) {
        return core::Result<std::string>::fail(rootResult.errorCode(),
                                              rootResult.errorMessage());
    }
    pj::JsonObject root = rootResult.value();
    const auto serversGuard = ensureServersObjectIfPresent(root);
    if (serversGuard.isFail()) {
        return core::Result<std::string>::fail(serversGuard.errorCode(),
                                               serversGuard.errorMessage());
    }
    const pj::JsonValue existing = root.get("mcpServers");
    if (!existing.isObject() || !existing.asObject().has(kServerName)) {
        return core::Result<std::string>::ok(jsonText);  // 本就没有
    }
    // 逐键重建（封装无键删除 API）：只摘本 server，其余 server 原样搬过去。
    // mcpServers 摘空后保留空对象——不额外删容器键（最小 diff，语义等价）
    pj::JsonObject kept;
    for (const std::string& key : existing.asObject().keys()) {
        if (key == kServerName) continue;
        kept.set(key, existing.asObject().get(key));
    }
    root.set("mcpServers", pj::JsonValue(kept));

    const auto out = pj::stringify(pj::JsonValue(root), true);
    if (out.isFail()) {
        return core::Result<std::string>::fail(out.errorCode(), out.errorMessage());
    }
    if (changed != nullptr) *changed = true;
    return core::Result<std::string>::ok(out.value());
}

namespace {

// 读-改-写公共编排。文件必须已存在（绝不新建）；有改动才备份 + 原子写。
core::Result<void> rewriteExisting(const std::filesystem::path& file,
                                   const std::function<core::Result<std::string>(
                                       const std::string&, bool*)>& mutate,
                                   const char* label) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) {
        return core::Result<void>::fail(
            core::ErrorCode::FileNotFound,
            "未检测到 Claude Code 配置 " + file.string() + "（不自动创建）");
    }
    bool readFailed = false;
    const std::string current = core::readTextFile(file, &readFailed);
    if (readFailed) {
        return core::Result<void>::fail(core::ErrorCode::FileReadFailed,
                                       "读取 " + file.string() + " 失败");
    }
    bool changed = false;
    auto next = mutate(current, &changed);
    if (next.isFail()) {
        return core::Result<void>::fail(next.errorCode(), next.errorMessage());
    }
    if (!changed) return core::Result<void>::ok();  // 幂等：一字节不动

    // 备份原文件（失败即整体失败，目标文件保持原内容）
    const auto bakResult = core::atomicWriteFile(backupFile(file), current,
                                                 "Claude Code 配置备份");
    if (bakResult.isFail()) {
        return core::Result<void>::fail(bakResult.errorCode(),
                                       bakResult.errorMessage());
    }
    return core::atomicWriteFile(file, next.value(), label);
}

} // namespace

core::Result<void> installServerFile(const std::filesystem::path& file,
                                     const std::string& url) {
    return rewriteExisting(
        file,
        [&url](const std::string& text, bool* changed) {
            return applyServerEntry(text, url, changed);
        },
        "Claude Code MCP 配置");
}

core::Result<void> removeServerFile(const std::filesystem::path& file) {
    return rewriteExisting(
        file,
        [](const std::string& text, bool* changed) {
            return removeServerEntry(text, changed);
        },
        "Claude Code MCP 配置还原");
}

} // namespace module::mcp::cc
