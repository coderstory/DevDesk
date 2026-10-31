#pragma once
// driver_schema.h — 场景文件解析（债-18 UI 驱动入口底层件，零 probe_harness 依赖）：
// 读文件 → platform::json::parse（带错误详情重载，行号直接决定写场景体验）→
// schema 校验（方案卡 §4.3：全部退出码 2 档错误）→ Scene 结构。
// 非法键 / 未知 type / 未知键名给相近候选（防静默错拼，与 id 候选同机制）。
#include "driver_keys.h"

#include "core/result.h"
#include "platform/json.h"

#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace ui_drive {

struct SceneSettings {
    int defaultWaitTimeoutMs = 5000;
    bool keepGoing = false;  // failureStrategy: "fail-fast"(默认) | "keep-going"
    int windowWidth = 1200;
    int windowHeight = 1000;
    // settings.seed.config：<appconfig 命名空间> → JSON 对象。执行环境对每对调
    // seedAppConfigJson（原文 stringify 产出，当场读回校验——test_support 惯例）。
    std::vector<std::pair<std::string, std::string>> seedConfigs;
};

// 九种步骤类型（方案卡 §4.2）：click/type/key/scroll/drag/wait/assert/screenshot/dump。
// type 为唯一判别键；_comment 为 JSON 无注释的补偿约定（加载期忽略）。
struct Step {
    std::string type;
    std::string comment;
    std::string id;          // click / scroll 定位 / drag 起点 / wait·assert 目标
    std::string button;      // click: "left"(默认) | "right"
    bool force = false;      // click: 跳过命中回读断言（对齐 Playwright force）
    bool readback = true;    // click: 命中回读开关（默认开）
    bool hasPoint = false;   // click 坐标兜底（AC-2 场景禁用，报告标注「坐标步骤」）
    double pointX = 0.0;
    double pointY = 0.0;
    std::string text;        // type 文本 / wait·assert text 期望值
    std::string keyName;     // key: "Enter" / "Ctrl+S"
    double dx = 0.0;         // scroll
    double dy = 0.0;
    std::string toId;        // drag 终点 id
    int dragSteps = 8;       // drag 插值段数
    std::string until;       // wait: exists | missing | text（ms 字段 = 固定等待）
    std::string check;       // assert: exists | missing | text | geometry
    std::string op;          // contains(默认)|equals|within-window|intersects
    std::string otherId;     // assert geometry intersects 的 "other"
    int ms = 0;              // wait 固定模拟帧等待（动画兜底，非首选）
    int timeoutMs = 0;       // wait 覆写默认超时（0 = 用 settings 默认）
    std::string path;        // screenshot / dump 落盘路径（相对 --out-dir）
};

struct Scene {
    SceneSettings settings;
    std::vector<Step> steps;
};

// ---- schema 词表（相近候选用） ----

inline std::vector<std::string> stepTypeVocabulary() {
    return {"click", "type", "key", "scroll", "drag",
            "wait", "assert", "screenshot", "dump"};
}

inline std::vector<std::string> stepKeyVocabulary(const std::string& type) {
    if (type == "click") return {"id", "button", "force", "readback", "point", "_comment", "timeoutMs"};
    if (type == "type") return {"text", "_comment", "timeoutMs"};
    if (type == "key") return {"name", "_comment", "timeoutMs"};
    if (type == "scroll") return {"dx", "dy", "id", "_comment", "timeoutMs"};
    if (type == "drag") return {"from", "to", "steps", "_comment", "timeoutMs"};
    if (type == "wait") return {"until", "id", "text", "op", "ms", "_comment", "timeoutMs"};
    if (type == "assert") return {"check", "id", "text", "op", "other", "_comment", "timeoutMs"};
    if (type == "screenshot") return {"path", "_comment"};
    if (type == "dump") return {"path", "_comment"};
    return {"_comment"};
}

// ---- 加载与校验 ----

// 场景非法 = 进程退出码 2 档错误（与用法错误同档）。
core::Result<Scene> loadSceneFile(const std::string& path);
core::Result<Scene> parseScene(const std::string& text);

namespace detail {

// json 封装 Int/Double 双 kind，asInt/asDouble 各只读自己的字段（整数 kind 的
// asDouble 返 0、小数 kind 的 asInt 返 0）——场景数值两路读兜底。
inline int numberAsInt(const platform::json::JsonValue& v) {
    const int64_t i = v.asInt();
    if (i != 0) return static_cast<int>(i);
    const double d = v.asDouble();
    return d != 0.0 ? static_cast<int>(d) : 0;
}

inline double numberAsDouble(const platform::json::JsonValue& v) {
    const double d = v.asDouble();
    if (d != 0.0) return d;
    return static_cast<double>(v.asInt());
}

inline std::string trimSpaces(const std::string& s) {
    std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

inline std::string candidatesSuffix(const std::string& value,
                                    const std::vector<std::string>& vocab) {
    const std::vector<std::string> hits = nearCandidates(value, vocab);
    if (hits.empty()) return {};
    std::string out = "（相近候选:";
    for (std::size_t i = 0; i < hits.size(); ++i) {
        out += (i == 0 ? " " : "、") + hits[i];
    }
    out += "）";
    return out;
}

inline bool isString(const platform::json::JsonValue& v) { return v.isString(); }
inline bool isNumber(const platform::json::JsonValue& v) { return v.isNumber(); }

// 步骤字段白名单 + 类型校验。失败信息已含步骤序号（1 起）与字段名。
core::Result<void> validateStep(const platform::json::JsonValue& obj, std::size_t index1, Step* out);

} // namespace detail

inline core::Result<Scene> parseScene(const std::string& text) {
    // 解析失败带行号（ParseDiag.bytePos 数换行符；场景是 AI 长期契约，行号
    // 直接决定写场景体验——方案卡 §7.3）
    platform::json::ParseDiag diag;
    auto parsed = platform::json::parse(text, &diag);
    if (parsed.isFail()) {
        std::size_t line = 1;
        for (std::size_t i = 0; i < diag.bytePos && i < text.size(); ++i) {
            if (text[i] == '\n') ++line;
        }
        return core::Result<Scene>::fail(
            core::ErrorCode::ValidationError,
            "第 " + std::to_string(line) + " 行：JSON 解析失败（yyjson: " + diag.msg + "）");
    }
    if (!parsed.value().isObject()) {
        return core::Result<Scene>::fail(core::ErrorCode::ValidationError,
                                         "场景顶层必须是 JSON 对象");
    }
    const platform::json::JsonObject root = parsed.value().asObject();

    // 未知顶层键（给相近候选）
    const std::vector<std::string> kTopKeys = {"version", "settings", "steps"};
    for (const std::string& key : root.keys()) {
        bool known = false;
        for (const auto& k : kTopKeys) known = known || key == k;
        if (!known) {
            return core::Result<Scene>::fail(
                core::ErrorCode::ValidationError,
                "未知顶层键 '" + key + "'" + detail::candidatesSuffix(key, kTopKeys));
        }
    }

    Scene scene;

    // version 必填且恒为 1
    if (!root.has("version")) {
        return core::Result<Scene>::fail(core::ErrorCode::ValidationError,
                                         "缺少顶层键 'version'（当前仅支持 1）");
    }
    const platform::json::JsonValue version = root.get("version");
    if (!version.isNumber() || version.asInt() != 1) {
        return core::Result<Scene>::fail(
            core::ErrorCode::ValidationError,
            "version 仅支持 1（实测 '" + (version.isNumber()
                                              ? std::to_string(version.asInt())
                                              : version.asString()) + "'）");
    }

    // settings（可选）
    if (root.has("settings")) {
        const platform::json::JsonValue sv = root.get("settings");
        if (!sv.isObject()) {
            return core::Result<Scene>::fail(core::ErrorCode::ValidationError,
                                             "settings 必须是对象");
        }
        const platform::json::JsonObject settings = sv.asObject();
        const std::vector<std::string> kSettingKeys = {
            "defaultWaitTimeoutMs", "failureStrategy", "window", "seed"};
        for (const std::string& key : settings.keys()) {
            bool known = false;
            for (const auto& k : kSettingKeys) known = known || key == k;
            if (!known) {
                return core::Result<Scene>::fail(
                    core::ErrorCode::ValidationError,
                    "未知 settings 键 '" + key + "'" +
                        detail::candidatesSuffix(key, kSettingKeys));
            }
        }
        if (settings.has("defaultWaitTimeoutMs")) {
            const platform::json::JsonValue v = settings.get("defaultWaitTimeoutMs");
            if (!v.isNumber()) {
                return core::Result<Scene>::fail(
                    core::ErrorCode::ValidationError,
                    "settings.defaultWaitTimeoutMs 必须是数字");
            }
            scene.settings.defaultWaitTimeoutMs = static_cast<int>(v.asInt());
        }
        if (settings.has("failureStrategy")) {
            const platform::json::JsonValue v = settings.get("failureStrategy");
            if (!v.isString() || (v.asString() != "fail-fast" && v.asString() != "keep-going")) {
                return core::Result<Scene>::fail(
                    core::ErrorCode::ValidationError,
                    "settings.failureStrategy 仅支持 \"fail-fast\" / \"keep-going\"");
            }
            scene.settings.keepGoing = v.asString() == "keep-going";
        }
        if (settings.has("window")) {
            const platform::json::JsonValue wv = settings.get("window");
            if (!wv.isObject()) {
                return core::Result<Scene>::fail(core::ErrorCode::ValidationError,
                                                 "settings.window 必须是对象");
            }
            const platform::json::JsonObject window = wv.asObject();
            for (const std::string& key : window.keys()) {
                if (key != "width" && key != "height") {
                    return core::Result<Scene>::fail(
                        core::ErrorCode::ValidationError,
                        "未知 settings.window 键 '" + key + "'（相近候选: width、height）");
                }
                if (!window.get(key).isNumber()) {
                    return core::Result<Scene>::fail(
                        core::ErrorCode::ValidationError,
                        "settings.window." + key + " 必须是数字");
                }
            }
            if (window.has("width")) scene.settings.windowWidth = static_cast<int>(window.get("width").asInt());
            if (window.has("height")) scene.settings.windowHeight = static_cast<int>(window.get("height").asInt());
        }
        if (settings.has("seed")) {
            const platform::json::JsonValue seedV = settings.get("seed");
            if (!seedV.isObject()) {
                return core::Result<Scene>::fail(core::ErrorCode::ValidationError,
                                                 "settings.seed 必须是对象");
            }
            const platform::json::JsonObject seed = seedV.asObject();
            for (const std::string& key : seed.keys()) {
                if (key != "config") {
                    return core::Result<Scene>::fail(
                        core::ErrorCode::ValidationError,
                        "未知 settings.seed 键 '" + key + "'（相近候选: config）");
                }
            }
            if (seed.has("config")) {
                const platform::json::JsonValue cfgV = seed.get("config");
                if (!cfgV.isObject()) {
                    return core::Result<Scene>::fail(
                        core::ErrorCode::ValidationError,
                        "settings.seed.config 必须是「命名空间 → JSON 对象」映射");
                }
                const platform::json::JsonObject cfg = cfgV.asObject();
                for (const std::string& ns : cfg.keys()) {
                    if (!cfg.get(ns).isObject()) {
                        return core::Result<Scene>::fail(
                            core::ErrorCode::ValidationError,
                            "settings.seed.config." + ns + " 必须是对象");
                    }
                    auto text = platform::json::stringify(cfg.get(ns));
                    if (text.isFail()) {
                        return core::Result<Scene>::fail(
                            core::ErrorCode::ValidationError,
                            "settings.seed.config." + ns + " 序列化失败");
                    }
                    scene.settings.seedConfigs.emplace_back(ns, text.value());
                }
            }
        }
    }

    // steps：必填数组且 ≥1 步（0 步 = 假绿风险，拒绝执行）
    if (!root.has("steps")) {
        return core::Result<Scene>::fail(core::ErrorCode::ValidationError,
                                         "缺少顶层键 'steps'");
    }
    const platform::json::JsonValue stepsV = root.get("steps");
    if (!stepsV.isArray()) {
        return core::Result<Scene>::fail(core::ErrorCode::ValidationError,
                                         "steps 必须是数组");
    }
    if (stepsV.size() == 0) {
        return core::Result<Scene>::fail(
            core::ErrorCode::ValidationError,
            "场景无步骤（0 步 = 假绿风险，拒绝执行——对齐 devdesk_tests 0 命中退 2 口径）");
    }
    for (int i = 0; i < stepsV.size(); ++i) {
        const platform::json::JsonValue item = stepsV[i];
        if (!item.isObject()) {
            return core::Result<Scene>::fail(
                core::ErrorCode::ValidationError,
                "第 " + std::to_string(i + 1) + " 步必须是对象");
        }
        Step step;
        auto validated = detail::validateStep(item, static_cast<std::size_t>(i) + 1, &step);
        if (validated.isFail()) {
            return core::Result<Scene>::fail(validated.errorCode(), validated.errorMessage());
        }
        scene.steps.push_back(std::move(step));
    }
    return core::Result<Scene>::ok(std::move(scene));
}

inline core::Result<Scene> loadSceneFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return core::Result<Scene>::fail(core::ErrorCode::ValidationError,
                                         "场景文件不存在或不可读: " + path);
    }
    std::string text((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    auto scene = parseScene(text);
    if (scene.isFail()) {
        return core::Result<Scene>::fail(
            scene.errorCode(), "场景文件 " + path + " " + scene.errorMessage());
    }
    return scene;
}

namespace detail {

inline core::Result<void> failStep(std::size_t index1, const std::string& msg) {
    return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                    "第 " + std::to_string(index1) + " 步：" + msg);
}

inline core::Result<void> validateStep(const platform::json::JsonValue& item,
                                       std::size_t index1, Step* out) {
    const platform::json::JsonObject obj = item.asObject();

    // type 判别键
    if (!obj.has("type") || !obj.get("type").isString()) {
        return failStep(index1, "缺少字符串字段 'type'");
    }
    const std::string type = obj.get("type").asString();
    const std::vector<std::string> types = stepTypeVocabulary();
    bool typeKnown = false;
    for (const auto& t : types) typeKnown = typeKnown || t == type;
    if (!typeKnown) {
        return failStep(index1, "未知步骤 type '" + type + "'" +
                                    candidatesSuffix(type, types));
    }
    out->type = type;

    const std::vector<std::string> allowedKeys = stepKeyVocabulary(type);
    for (const std::string& key : obj.keys()) {
        if (key == "type") continue;  // 判别键本身恒合法
        bool known = false;
        for (const auto& k : allowedKeys) known = known || key == k;
        if (!known) {
            return failStep(index1, "type='" + type + "' 不支持字段 '" + key + "'" +
                                        candidatesSuffix(key, allowedKeys));
        }
    }

    auto getString = [&](const char* key, std::string* dst,
                         bool required = false) -> core::Result<void> {
        if (!obj.has(key)) {
            if (required) return failStep(index1, std::string("缺少字段 '") + key + "'");
            return core::Result<void>::ok();
        }
        if (!obj.get(key).isString()) {
            return failStep(index1, std::string(key) + " 必须是字符串");
        }
        *dst = obj.get(key).asString();
        return core::Result<void>::ok();
    };
    auto getInt = [&](const char* key, int* dst) -> core::Result<void> {
        if (!obj.has(key)) return core::Result<void>::ok();
        if (!obj.get(key).isNumber()) {
            return failStep(index1, std::string(key) + " 必须是数字");
        }
        *dst = numberAsInt(obj.get(key));
        return core::Result<void>::ok();
    };
    auto getNumber = [&](const char* key, double* dst) -> core::Result<void> {
        if (!obj.has(key)) return core::Result<void>::ok();
        if (!obj.get(key).isNumber()) {
            return failStep(index1, std::string(key) + " 必须是数字");
        }
        *dst = numberAsDouble(obj.get(key));
        return core::Result<void>::ok();
    };
    auto getBool = [&](const char* key, bool* dst) -> core::Result<void> {
        if (!obj.has(key)) return core::Result<void>::ok();
        if (!obj.get(key).isBool()) {
            return failStep(index1, std::string(key) + " 必须是布尔");
        }
        *dst = obj.get(key).asBool();
        return core::Result<void>::ok();
    };

    if (obj.has("_comment")) {
        if (!obj.get("_comment").isString()) {
            return failStep(index1, "_comment 必须是字符串");
        }
        out->comment = obj.get("_comment").asString();
    }
    if (auto r = getInt("timeoutMs", &out->timeoutMs); r.isFail()) return r;

    if (type == "click") {
        if (auto r = getString("id", &out->id); r.isFail()) return r;
        if (auto r = getString("button", &out->button); r.isFail()) return r;
        if (auto r = getBool("force", &out->force); r.isFail()) return r;
        if (auto r = getBool("readback", &out->readback); r.isFail()) return r;
        if (obj.has("point")) {
            const platform::json::JsonValue p = obj.get("point");
            if (!p.isArray() || p.size() != 2 || !p[0].isNumber() || !p[1].isNumber()) {
                return failStep(index1, "point 必须是 [x, y] 数字数组");
            }
            out->hasPoint = true;
            out->pointX = p[0].asDouble();
            out->pointY = p[1].asDouble();
        }
        if (!out->button.empty() && out->button != "left" && out->button != "right") {
            return failStep(index1, "button 仅支持 \"left\" / \"right\"");
        }
        if (out->id.empty() && !out->hasPoint) {
            return failStep(index1, "click 需要 'id' 或 'point' 二选一");
        }
        if (out->hasPoint && !out->id.empty()) {
            return failStep(index1, "click 的 'id' 与 'point' 互斥（只给一个）");
        }
        return core::Result<void>::ok();
    }
    if (type == "type") {
        if (auto r = getString("text", &out->text, /*required=*/true); r.isFail()) return r;
        return core::Result<void>::ok();
    }
    if (type == "key") {
        if (auto r = getString("name", &out->keyName, /*required=*/true); r.isFail()) return r;
        const KeySpec spec = lookupKeyName(out->keyName);
        if (!spec.known) {
            return failStep(index1, "未知键名 '" + out->keyName + "'" +
                                        candidatesSuffix(out->keyName, keyNameVocabulary()));
        }
        return core::Result<void>::ok();
    }
    if (type == "scroll") {
        if (auto r = getNumber("dx", &out->dx); r.isFail()) return r;
        if (auto r = getNumber("dy", &out->dy); r.isFail()) return r;
        if (auto r = getString("id", &out->id); r.isFail()) return r;
        if (out->dx == 0.0 && out->dy == 0.0) {
            return failStep(index1, "scroll 的 dx/dy 不能同时为 0");
        }
        return core::Result<void>::ok();
    }
    if (type == "drag") {
        if (auto r = getString("from", &out->id, /*required=*/true); r.isFail()) return r;
        if (auto r = getString("to", &out->toId, /*required=*/true); r.isFail()) return r;
        if (auto r = getInt("steps", &out->dragSteps); r.isFail()) return r;
        if (out->dragSteps <= 0) out->dragSteps = 8;
        return core::Result<void>::ok();
    }
    if (type == "wait") {
        if (auto r = getString("until", &out->until); r.isFail()) return r;
        if (auto r = getString("id", &out->id); r.isFail()) return r;
        if (auto r = getString("text", &out->text); r.isFail()) return r;
        if (auto r = getString("op", &out->op); r.isFail()) return r;
        if (auto r = getInt("ms", &out->ms); r.isFail()) return r;
        if (out->until.empty() && out->ms <= 0) {
            return failStep(index1, "wait 需要 'until'（exists/missing/text）或 'ms'（固定等待）");
        }
        if (!out->until.empty() && out->until != "exists" && out->until != "missing" &&
            out->until != "text") {
            return failStep(index1, "wait.until 仅支持 exists / missing / text（相近候选: exists、missing、text）");
        }
        if ((out->until == "exists" || out->until == "missing") && out->id.empty()) {
            return failStep(index1, "wait.until=exists/missing 需要 'id'");
        }
        if (out->until == "text" && out->text.empty()) {
            return failStep(index1, "wait.until=text 需要 'text'");
        }
        if (out->until == "text" && !out->op.empty() && out->op != "contains" && out->op != "equals") {
            return failStep(index1, "wait 文本 op 仅支持 contains / equals");
        }
        return core::Result<void>::ok();
    }
    if (type == "assert") {
        if (auto r = getString("check", &out->check, /*required=*/true); r.isFail()) return r;
        if (auto r = getString("id", &out->id); r.isFail()) return r;
        if (auto r = getString("text", &out->text); r.isFail()) return r;
        if (auto r = getString("op", &out->op); r.isFail()) return r;
        if (auto r = getString("other", &out->otherId); r.isFail()) return r;
        if (out->check != "exists" && out->check != "missing" && out->check != "text" &&
            out->check != "geometry") {
            return failStep(index1,
                            "assert.check 仅支持 exists / missing / text / geometry"
                            "（相近候选: exists、missing、text、geometry）");
        }
        if ((out->check == "exists" || out->check == "missing") && out->id.empty()) {
            return failStep(index1, "assert.check=exists/missing 需要 'id'");
        }
        if (out->check == "text") {
            if (out->text.empty()) return failStep(index1, "assert.check=text 需要 'text'");
            if (!out->op.empty() && out->op != "contains" && out->op != "equals") {
                return failStep(index1, "assert 文本 op 仅支持 contains / equals（缺省 contains）");
            }
        }
        if (out->check == "geometry") {
            if (out->op != "within-window" && out->op != "intersects") {
                return failStep(index1,
                                "assert.check=geometry 的 op 仅支持 within-window / intersects");
            }
            if (out->op == "intersects" && out->otherId.empty()) {
                return failStep(index1, "assert geometry op=intersects 需要 'other'（另一元素 id）");
            }
            if (out->id.empty()) {
                return failStep(index1, "assert.check=geometry 需要 'id'");
            }
        }
        return core::Result<void>::ok();
    }
    if (type == "screenshot" || type == "dump") {
        if (auto r = getString("path", &out->path, /*required=*/true); r.isFail()) return r;
        return core::Result<void>::ok();
    }
    return failStep(index1, "未知步骤 type '" + type + "'");
}

} // namespace detail

} // namespace ui_drive
