// src/module_memory/hook/hooks_settings.cpp
#include "hooks_settings.h"

#include "core/atomic_write.h"
#include "core/fs_text.h"
#include "platform/json.h"

#include <fstream>

namespace module::memory::hook {

namespace {

using platform::json::JsonObject;
using platform::json::JsonValue;

// 确保父对象下存在 object 键并返回（缺则补建接回）
JsonObject ensureObject(JsonObject& parent, const std::string& key) {
    const JsonValue existing = parent.get(key);
    if (existing.isObject()) return existing.asObject();
    JsonObject fresh;
    parent.set(key, JsonValue(fresh));
    return parent.get(key).asObject();
}

// UserPromptSubmit[].hooks[] 里 command == cmd 的条目存在吗
bool commandPresent(const JsonValue& upsArray, const std::string& cmd) {
    if (!upsArray.isArray()) return false;
    for (int i = 0; i < upsArray.size(); ++i) {
        const JsonValue entry = upsArray[i];
        if (!entry.isObject()) continue;
        const JsonValue inner = entry.asObject().get("hooks");
        if (!inner.isArray()) continue;
        for (int k = 0; k < inner.size(); ++k) {
            if (inner[k].isObject() &&
                inner[k].asObject().get("command").asString() == cmd) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

std::string buildHookCommand(const std::string& exePath) {
    return "\"" + exePath + "\" memory hook";
}

core::Result<std::string> applyHookEntry(const std::string& settingsJson,
                                         const std::string& hookCommand,
                                         bool* changed) {
    if (changed != nullptr) *changed = false;
    const auto parsed = platform::json::parse(settingsJson);
    if (parsed.isFail()) {
        return core::Result<std::string>::fail(core::ErrorCode::JsonParseError,
                                               "settings.json 非法 JSON");
    }
    if (!parsed.value().isObject()) {
        return core::Result<std::string>::fail(core::ErrorCode::JsonParseError,
                                               "settings.json 顶层非对象");
    }
    JsonObject root = parsed.value().asObject();
    JsonObject hooks = ensureObject(root, "hooks");
    JsonValue ups = hooks.get("UserPromptSubmit");
    if (!ups.isArray()) {
        ups = platform::json::makeArray();
    }
    if (commandPresent(ups, hookCommand)) {
        return core::Result<std::string>::ok(settingsJson);  // 幂等 no-op
    }

    // {matcher:"", hooks:[{type,command,timeout}]}
    JsonObject hookItem;
    hookItem.set("type", platform::json::makeString("command"));
    hookItem.set("command", platform::json::makeString(hookCommand));
    hookItem.set("timeout", platform::json::makeInt(10));
    JsonObject matcherEntry;
    matcherEntry.set("matcher", platform::json::makeString(""));
    JsonValue innerArr = platform::json::makeArray();
    innerArr.append(JsonValue(hookItem));
    matcherEntry.set("hooks", innerArr);

    JsonValue newUps = platform::json::makeArray();
    for (int i = 0; i < ups.size(); ++i) newUps.append(ups[i]);
    newUps.append(JsonValue(matcherEntry));
    hooks.set("UserPromptSubmit", newUps);
    root.set("hooks", JsonValue(hooks));

    const auto out = platform::json::stringify(JsonValue(root), true);
    if (out.isFail()) {
        return core::Result<std::string>::fail(out.errorCode(),
                                               out.errorMessage());
    }
    if (changed != nullptr) *changed = true;
    return core::Result<std::string>::ok(out.value());
}

core::Result<std::string> removeHookEntry(const std::string& settingsJson,
                                          const std::string& hookCommand,
                                          bool* changed) {
    if (changed != nullptr) *changed = false;
    const auto parsed = platform::json::parse(settingsJson);
    if (parsed.isFail()) {
        return core::Result<std::string>::fail(core::ErrorCode::JsonParseError,
                                               "settings.json 非法 JSON");
    }
    if (!parsed.value().isObject()) {
        return core::Result<std::string>::fail(core::ErrorCode::JsonParseError,
                                               "settings.json 顶层非对象");
    }
    JsonObject root = parsed.value().asObject();
    const JsonValue hooksVal = root.get("hooks");
    if (!hooksVal.isObject()) {
        return core::Result<std::string>::ok(settingsJson);  // 无 hooks：原样
    }
    const JsonValue ups = platform::json::JsonObject(hooksVal.asObject()).get("UserPromptSubmit");
    if (!ups.isArray()) {
        return core::Result<std::string>::ok(settingsJson);
    }
    // 逐 matcher 条目过滤内层 hooks 数组中 command == cmd 的项；空组回收
    bool removed = false;
    JsonValue newUps = platform::json::makeArray();
    for (int i = 0; i < ups.size(); ++i) {
        const JsonValue entry = ups[i];
        if (!entry.isObject()) {
            newUps.append(entry);
            continue;
        }
        const JsonValue inner = entry.asObject().get("hooks");
        if (!inner.isArray()) {
            newUps.append(entry);
            continue;
        }
        JsonValue kept = platform::json::makeArray();
        for (int k = 0; k < inner.size(); ++k) {
            const bool isOurs = inner[k].isObject() &&
                                inner[k].asObject().get("command").asString() ==
                                    hookCommand;
            if (isOurs) {
                removed = true;
            } else {
                kept.append(inner[k]);
            }
        }
        if (kept.size() == 0) continue;  // 空组回收
        JsonObject patched = entry.asObject();
        patched.set("hooks", kept);
        newUps.append(JsonValue(patched));
    }
    if (!removed) {
        return core::Result<std::string>::ok(settingsJson);  // 本就没有
    }
    JsonObject hooks = hooksVal.asObject();
    if (newUps.size() == 0) {
        hooks.set("UserPromptSubmit", platform::json::makeNull());
        // 键级回收：置 null 等价移除语义（封装无键删除 API；null 键对 Claude Code
        // 的 schema 校验无害——数组字段可空）
    } else {
        hooks.set("UserPromptSubmit", newUps);
    }
    root.set("hooks", JsonValue(hooks));
    const auto out = platform::json::stringify(JsonValue(root), true);
    if (out.isFail()) {
        return core::Result<std::string>::fail(out.errorCode(),
                                               out.errorMessage());
    }
    if (changed != nullptr) *changed = true;
    return core::Result<std::string>::ok(out.value());
}

core::Result<void> installHookFile(const std::filesystem::path& settingsFile,
                                   const std::string& hookCommand) {
    const std::string current =
        core::readTextFile(settingsFile);  // 缺文件 = 空 → apply 补建
    bool changed = false;
    auto next = applyHookEntry(current.empty() ? std::string("{}") : current,
                               hookCommand, &changed);
    if (next.isFail()) {
        return core::Result<void>::fail(next.errorCode(), next.errorMessage());
    }
    if (!changed) return core::Result<void>::ok();
    return core::atomicWriteFile(settingsFile, next.value(),
                                 "Claude Code hooks 配置");
}

core::Result<void> restoreHookFile(const std::filesystem::path& settingsFile,
                                   const std::string& hookCommand) {
    if (!std::filesystem::exists(settingsFile)) {
        return core::Result<void>::ok();  // 还原路径绝不报错挡停机
    }
    const std::string current = core::readTextFile(settingsFile);
    bool changed = false;
    auto next = removeHookEntry(current, hookCommand, &changed);
    if (next.isFail()) {
        return core::Result<void>::fail(next.errorCode(), next.errorMessage());
    }
    if (!changed) return core::Result<void>::ok();
    return core::atomicWriteFile(settingsFile, next.value(),
                                 "Claude Code hooks 配置还原");
}

}  // namespace module::memory::hook
