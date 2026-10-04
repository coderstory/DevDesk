// src/module_pet/pet_config.cpp
// pet_config 的 app_config 表存取；整份存一行（键 pet_config），不读旧文件、不迁移。
#include "module_pet/pet_config.h"

#include <utility>

#include "core/app_config.h"
#include "core/logging.h"
#include "platform/json.h"

namespace module::pet {

namespace {
constexpr const char* kKeySkin = "skinName";
constexpr const char* kKeyX = "x";
constexpr const char* kKeyY = "y";
constexpr const char* kKeyScale = "scale";  // 显示大小档（0.75/1.0/1.5）
} // namespace

PetConfigStore::PetConfigStore(core::data::SqliteDb* db) : db_(db) {}

PetConfig PetConfigStore::sanitized(const PetConfig& cfg) {
    PetConfig out = cfg;
    // 皮肤名只许 [A-Za-z0-9_-]（要拼进文件系统路径，防穿越）；-1 哨兵单边独立生效，
    // 其他负值是左/上副屏的合法坐标原样保留；正负都钳进 int16 量级防脏值。
    if (out.skinName.empty() ||
        out.skinName.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
            std::string::npos) {
        out.skinName = kDefaultSkinName;
    }
    if (out.x < -32767) out.x = -32767;
    if (out.y < -32767) out.y = -32767;
    if (out.x > 32767) out.x = 32767;
    if (out.y > 32767) out.y = 32767;
    // 显示大小档：白名单外（0.9 / 2.0 等）收敛默认中档
    out.scale = sanitizePetScale(out.scale);
    return out;
}

core::Result<PetConfig> PetConfigStore::loadChecked() const {
    PetConfig cfg;  // 全默认值兜底
    if (db_ == nullptr) {
        return core::Result<PetConfig>::fail(
            core::ErrorCode::NotFound,
            "应用数据库未就绪（core::appDatabase() 为空）");
    }
    auto raw = core::appconfig::get(*db_, kPetConfigKey);
    if (raw.isFail()) {
        // 键不存在 = 首次启动（静默用默认）；表不存在/SQL 失败 = 真故障，必须报出来。
        if (raw.errorCode() == core::ErrorCode::NotFound) {
            return core::Result<PetConfig>::ok(cfg);
        }
        return core::Result<PetConfig>::fail(
            raw.errorCode(),
            "读取 app_config[" + std::string(kPetConfigKey) + "] 失败: " +
                raw.errorMessage());
    }
    auto parsed = platform::json::parse(raw.value());
    if (parsed.isFail() || !parsed.value().isObject()) {
        return core::Result<PetConfig>::fail(
            core::ErrorCode::JsonParseError,
            "app_config[" + std::string(kPetConfigKey) + "] 不是合法 JSON 对象: " +
                (parsed.isFail() ? parsed.errorMessage() : std::string("顶层非对象")));
    }
    platform::json::JsonObject obj = parsed.value().asObject();

    if (obj.has(kKeySkin) && obj.get(kKeySkin).isString()) {
        cfg.skinName = obj.get(kKeySkin).asString();
    }
    if (obj.has(kKeyX) && obj.get(kKeyX).isNumber()) {
        cfg.x = static_cast<int>(obj.get(kKeyX).asInt());
    }
    if (obj.has(kKeyY) && obj.get(kKeyY).isNumber()) {
        cfg.y = static_cast<int>(obj.get(kKeyY).asInt());
    }
    if (obj.has(kKeyScale) && obj.get(kKeyScale).isNumber()) {
        // 数字双分支读取：序列化器可能把整值写成 "2.0"（Double）或 "2"（Int），只读 asDouble()
        // 恒返 0，会先读成 0 再被 sanitize 收敛默认档，故 asDouble 为 0 时回退 asInt。
        const platform::json::JsonValue v = obj.get(kKeyScale);
        cfg.scale = v.asDouble() != 0.0 ? v.asDouble()
                                        : static_cast<double>(v.asInt());
    } else if (obj.has(kKeyScale)) {
        LOG_WARN("Pet: 配置 scale 键非法（非数字），回落默认 1.0");
    }
    // 白名单外值回落默认档前出声（scale 直接决定用户可见档位，收敛必须留日志）。
    if (sanitizePetScale(cfg.scale) != cfg.scale) {
        LOG_WARN("Pet: 配置 scale=" + std::to_string(cfg.scale) +
                 " 不在三档白名单（0.75/1.0/1.5），回落默认 1.0");
    }
    return core::Result<PetConfig>::ok(sanitized(cfg));
}

PetConfig PetConfigStore::load() const {
    auto loaded = loadChecked();
    if (loaded.isFail()) {
        LOG_ERROR("Pet: 读取桌宠配置失败，回退默认值（本次改动将无法保存）: " +
                  loaded.errorMessage());
        return sanitized(PetConfig{});
    }
    return loaded.value();
}

core::Result<void> PetConfigStore::save(const PetConfig& cfgRaw) const {
    const PetConfig cfg = sanitized(cfgRaw);
    if (db_ == nullptr) {
        return core::Result<void>::fail(
            core::ErrorCode::NotFound,
            "应用数据库未就绪（core::appDatabase() 为空），桌宠配置未保存");
    }

    platform::json::JsonObject obj;
    obj.set(kKeySkin, platform::json::makeString(cfg.skinName));
    obj.set(kKeyX, platform::json::makeInt(cfg.x));
    obj.set(kKeyY, platform::json::makeInt(cfg.y));
    obj.set(kKeyScale, platform::json::makeDouble(cfg.scale));

    auto text = platform::json::stringify(platform::json::JsonValue(obj),
                                          /*pretty=*/true);
    if (text.isFail()) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "配置序列化失败: " + text.errorMessage());
    }
    return core::appconfig::set(*db_, kPetConfigKey, text.value());
}

} // namespace module::pet
