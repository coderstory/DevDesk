// src/module_mcp/claude_mcp_config.cpp
#include "claude_mcp_config.h"

#include "mcp_config_common.h"
#include "core/fs_text.h"
#include "platform/json.h"

#include <functional>
#include <system_error>

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
        return core::propagate<std::string>(rootResult);
    }
    pj::JsonObject root = rootResult.value();
    const auto serversGuard = ensureServersObjectIfPresent(root);
    if (serversGuard.isFail()) {
        return core::propagate<std::string>(serversGuard);
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
        return core::propagate<std::string>(out);
    }
    if (changed != nullptr) *changed = true;
    return core::Result<std::string>::ok(out.value());
}

core::Result<std::string> removeServerEntry(const std::string& jsonText,
                                           bool* changed) {
    if (changed != nullptr) *changed = false;
    auto rootResult = parseRoot(jsonText, "~/.claude.json");
    if (rootResult.isFail()) {
        return core::propagate<std::string>(rootResult);
    }
    pj::JsonObject root = rootResult.value();
    const auto serversGuard = ensureServersObjectIfPresent(root);
    if (serversGuard.isFail()) {
        return core::propagate<std::string>(serversGuard);
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
        return core::propagate<std::string>(out);
    }
    if (changed != nullptr) *changed = true;
    return core::Result<std::string>::ok(out.value());
}

std::filesystem::path backupFile(const std::filesystem::path& file) {
    return config_common::backupFile(file);
}

core::Result<void> installServerFile(const std::filesystem::path& file,
                                     const std::string& url) {
    return config_common::rewriteExisting(
        file,
        [&url](const std::string& text, bool* changed) {
            return applyServerEntry(text, url, changed);
        },
        "Claude Code MCP 配置", "Claude Code");
}

core::Result<void> removeServerFile(const std::filesystem::path& file) {
    return config_common::rewriteExisting(
        file,
        [](const std::string& text, bool* changed) {
            return removeServerEntry(text, changed);
        },
        "Claude Code MCP 配置还原", "Claude Code");
}

} // namespace module::mcp::cc
