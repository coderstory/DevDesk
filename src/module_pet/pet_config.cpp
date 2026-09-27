// src/module_pet/pet_config.cpp
// pet_config.json 读写（照抄 SettingsStore 的容错模式：缺失/损坏回退默认）
#include "module_pet/pet_config.h"

#include <fstream>
#include <sstream>
#include <utility>

#include "core/atomic_write.h"
#include "core/logging.h"
#include "core/paths.h"
#include "platform/json.h"

namespace module::pet {

namespace {
constexpr const char* kKeySkin = "skinName";
constexpr const char* kKeyX = "x";
constexpr const char* kKeyY = "y";
// clickThrough 字段已于 2026-09-24 移除（挂机穿透功能删除）；旧
// pet_config.json 残留的 clickThrough 键被 load 静默忽略（向后兼容）
} // namespace

std::filesystem::path petConfigFile() {
    return core::paths::petsDir() / "pet_config.json";
}

PetConfigStore::PetConfigStore(std::filesystem::path file)
    : file_(std::move(file)) {}

PetConfig PetConfigStore::sanitized(const PetConfig& cfg) {
    PetConfig out = cfg;
    // 皮肤名只许 [A-Za-z0-9_-]（要拼进文件系统路径，防穿越）；
    // -1「未设置」哨兵单边独立生效；M2 多显示器：其他负值是左/上副屏的合法
    // 坐标原样保留（旧 x<-1→-1 收敛把副屏位置吞成未设置，重启跳回主屏）；
    // 正负都钳进 int16 量级防手改乱值
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
    return out;
}

PetConfig PetConfigStore::load() const {
    PetConfig cfg;  // 全默认值兜底

    std::error_code ec;
    if (!std::filesystem::exists(file_, ec) || ec) {
        return cfg;  // 首次启动：无配置文件，正常路径
    }

    std::ifstream f(file_);
    if (!f) {
        LOG_WARN("Pet: 无法读取配置文件，使用默认值: " + file_.string());
        return cfg;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    f.close();

    auto parsed = platform::json::parse(ss.str());
    if (parsed.isFail() || !parsed.unsafeValue().isObject()) {
        LOG_WARN("Pet: 配置文件解析失败，使用默认值: " + file_.string());
        return cfg;
    }
    platform::json::JsonObject obj = parsed.unsafeValue().asObject();

    if (obj.has(kKeySkin) && obj.get(kKeySkin).isString()) {
        cfg.skinName = obj.get(kKeySkin).asString();
    }
    if (obj.has(kKeyX) && obj.get(kKeyX).isNumber()) {
        cfg.x = static_cast<int>(obj.get(kKeyX).asInt());
    }
    if (obj.has(kKeyY) && obj.get(kKeyY).isNumber()) {
        cfg.y = static_cast<int>(obj.get(kKeyY).asInt());
    }
    return sanitized(cfg);
}

core::Result<void> PetConfigStore::save(const PetConfig& cfgRaw) const {
    const PetConfig cfg = sanitized(cfgRaw);

    platform::json::JsonObject obj;
    obj.set(kKeySkin, platform::json::makeString(cfg.skinName));
    obj.set(kKeyX, platform::json::makeInt(cfg.x));
    obj.set(kKeyY, platform::json::makeInt(cfg.y));

    auto text = platform::json::stringify(
        platform::json::JsonValue(obj), /*pretty=*/true);
    if (text.isFail()) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        "配置序列化失败: " + text.errorMessage());
    }

    // 父目录确保存在（petsDir 理论上已建好，这里兜底）
    std::error_code ec;
    const auto parent = file_.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent, ec) && !ec) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return core::Result<void>::fail(core::ErrorCode::Internal,
                                            "配置目录创建失败: " + ec.message());
        }
    }

    // 原子写（tmp → remove → rename，core/atomic_write.h 唯一实现，审计 THS-03
    // 第二批收口）：旧的 std::ofstream(file_, trunc) 原地写在写窗口内崩溃/断电
    // 会把 pet_config.json 截成半截，load 走解析失败分支静默回落默认值（桌宠
    // 坐标回到「未设置」、皮肤选择回 robot）。尾处理照旧实现：serialize + "\n"
    return core::atomicWriteFile(file_, text.unsafeValue() + "\n", "配置文件");
}

} // namespace module::pet
