// src/module_mcp/zcode_mcp_config.cpp
// ZCode 全局 MCP 配置读写实现（need279）。与 claude_mcp_config.cpp 同构镜像；
// 本质差异只有两处：两级键 mcp.servers、安装三分判定（同址幂等/异址冲突）。
#include "zcode_mcp_config.h"

#include "mcp_config_common.h"

#include "core/fs_text.h"
#include "platform/json.h"

#include <functional>
#include <utility>
#include <system_error>

namespace module::mcp::zc {
namespace {

namespace pj = platform::json;

// 错误口径里的文件名（~ 记法用户可读；实际路径随编排参数传入）
const char* kConfigLabel = "~/.zcode/cli/config.json";

// 顶层对象校验（复用 cc::parseRoot 同款错误口径）
core::Result<pj::JsonObject> parseRoot(const std::string& jsonText) {
    const auto parsed = pj::parse(jsonText);
    if (parsed.isFail()) {
        return core::Result<pj::JsonObject>::fail(core::ErrorCode::JsonParseError,
                                                  std::string(kConfigLabel) +
                                                      "非法 JSON");
    }
    if (!parsed.value().isObject()) {
        return core::Result<pj::JsonObject>::fail(
            core::ErrorCode::JsonParseError,
            std::string(kConfigLabel) + "顶层非对象");
    }
    return core::Result<pj::JsonObject>::ok(parsed.value().asObject());
}

// need279 F-02：mcp / mcp.servers 两级形态校验。缺失或显式 null → 放行（安装
// 补建层级）；存在但非对象（数组/字符串/数字）→ fail 拒写。防「静默替换成空
// 对象吞掉原值」（cc 端审计 P1-5 同款，防的事故一样）
core::Result<void> ensureHierarchyIfPresent(const pj::JsonObject& root) {
    const pj::JsonValue mcp = root.get("mcp");
    if (!mcp.isNull() && !mcp.isObject()) {
        return core::Result<void>::fail(
            core::ErrorCode::JsonParseError,
            std::string(kConfigLabel) +
                " 的 mcp 是非对象形态（数组/字符串），拒绝写入以免吞掉原值；"
                "请先手工修正该键后再安装");
    }
    if (mcp.isObject()) {
        const pj::JsonValue servers = mcp.asObject().get("servers");
        if (!servers.isNull() && !servers.isObject()) {
            return core::Result<void>::fail(
                core::ErrorCode::JsonParseError,
                std::string(kConfigLabel) +
                    " 的 mcp.servers 是非对象形态（数组/字符串），拒绝写入以免吞掉"
                    "原值；请先手工修正该键后再安装");
        }
    }
    return core::Result<void>::ok();
}

// mcp.servers 里是否已有本 server 的键；有则带出条目 url（缺 url/非串 = 空串）
bool serverPresent(const pj::JsonObject& root, std::string* urlOut) {
    const pj::JsonValue mcp = root.get("mcp");
    if (!mcp.isObject()) return false;
    const pj::JsonValue servers = mcp.asObject().get("servers");
    if (!servers.isObject() || !servers.asObject().has(kServerName)) return false;
    if (urlOut != nullptr) {
        const pj::JsonValue entry = servers.asObject().get(kServerName);
        if (entry.isObject() && entry.asObject().get("url").isString()) {
            *urlOut = entry.asObject().get("url").asString();
        }
    }
    return true;
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
    const auto root = parseRoot(text);
    if (root.isFail()) {
        st.error = root.errorMessage();
        return st;
    }
    std::string url;
    st.installed = serverPresent(root.value(), &url);
    st.installedUrl = std::move(url);
    return st;
}

core::Result<std::string> applyServerEntry(const std::string& jsonText,
                                          const std::string& url,
                                          bool* changed) {
    if (changed != nullptr) *changed = false;
    auto rootResult = parseRoot(jsonText);
    if (rootResult.isFail()) {
        return core::propagate<std::string>(rootResult);
    }
    pj::JsonObject root = rootResult.value();
    const auto guard = ensureHierarchyIfPresent(root);
    if (guard.isFail()) {
        return core::propagate<std::string>(guard);
    }

    std::string existingUrl;
    if (serverPresent(root, &existingUrl)) {
        // 三分判定：同址 = 幂等 no-op；异址/缺 url = 冲突不覆盖（D2 裁决）
        if (existingUrl == url) {
            return core::Result<std::string>::ok(jsonText);  // 幂等 no-op
        }
        return core::Result<std::string>::fail(
            core::ErrorCode::Conflict,
            "ZCode 配置已有同名 devdesk 条目且地址不同（" +
                (existingUrl.empty() ? std::string("缺少 url") : existingUrl) +
                "），不覆盖；请先在 ZCode 配置中移除该条目后再安装");
    }

    // 取/补 mcp.servers 层级（形态校验已过：缺失或 null → 补建空对象）
    pj::JsonObject mcp =
        root.get("mcp").isObject() ? root.get("mcp").asObject() : pj::JsonObject();
    const pj::JsonValue serversVal = mcp.get("servers");
    pj::JsonObject servers =
        serversVal.isObject() ? serversVal.asObject() : pj::JsonObject();

    // 条目形态（PRD §8 裁决）：type + url + 显式 enabled=true
    pj::JsonObject entry;
    entry.set("type", pj::makeString("http"));
    entry.set("url", pj::makeString(url));
    entry.set("enabled", pj::makeBool(true));
    servers.set(kServerName, pj::JsonValue(entry));
    mcp.set("servers", pj::JsonValue(servers));
    root.set("mcp", pj::JsonValue(mcp));

    // 与原文件同档：两空格缩进（YYJSON_WRITE_PRETTY_TWO_SPACES）
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
    auto rootResult = parseRoot(jsonText);
    if (rootResult.isFail()) {
        return core::propagate<std::string>(rootResult);
    }
    pj::JsonObject root = rootResult.value();
    const auto guard = ensureHierarchyIfPresent(root);
    if (guard.isFail()) {
        return core::propagate<std::string>(guard);
    }
    const pj::JsonValue mcpVal = root.get("mcp");
    if (!mcpVal.isObject()) {
        return core::Result<std::string>::ok(jsonText);  // 无 mcp = 本就没有
    }
    const pj::JsonValue serversVal = mcpVal.asObject().get("servers");
    if (!serversVal.isObject() || !serversVal.asObject().has(kServerName)) {
        return core::Result<std::string>::ok(jsonText);  // 本就没有
    }
    // 逐键重建（封装无键删除 API）：只摘本 server，其余 server 原样搬过去。
    // servers 摘空后保留空对象——不额外删容器键（最小 diff，语义等价）
    pj::JsonObject kept;
    for (const std::string& key : serversVal.asObject().keys()) {
        if (key == kServerName) continue;
        kept.set(key, serversVal.asObject().get(key));
    }
    pj::JsonObject mcp = mcpVal.asObject();
    mcp.set("servers", pj::JsonValue(kept));
    root.set("mcp", pj::JsonValue(mcp));

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
        "ZCode MCP 配置", "ZCode");
}

core::Result<void> removeServerFile(const std::filesystem::path& file) {
    return config_common::rewriteExisting(
        file,
        [](const std::string& text, bool* changed) {
            return removeServerEntry(text, changed);
        },
        "ZCode MCP 配置还原", "ZCode");
}

} // namespace module::mcp::zc
