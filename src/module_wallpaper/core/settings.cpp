#include "module_wallpaper/core/settings.h"

#include <cmath>
#include <cstdint>
#include <system_error>

#include "core/atomic_write.h"
#include "core/fs_text.h"
#include "core/logging.h"
#include "core/paths.h"
#include "module_wallpaper/core/folder_policy.h"
#include "module_wallpaper/core/rotation.h"
#include "platform/json.h"

namespace module::wallpaper {

namespace {

constexpr const char* kLogTag = "壁纸设置: ";

std::string kindToJson(WallpaperKind kind) {
    return kind == WallpaperKind::Image ? "image" : "video";
}

WallpaperKind kindFromJson(const std::string& text, WallpaperKind fallback) {
    if (text == "video") return WallpaperKind::Video;
    if (text == "image") return WallpaperKind::Image;
    return fallback;
}

std::string modeToJson(PlayMode mode) {
    switch (mode) {
        case PlayMode::LoopSingle: return "loopSingle";
        case PlayMode::LoopList: return "loopList";
        case PlayMode::Shuffle: return "shuffle";
    }
    return "loopSingle";
}

PlayMode modeFromJson(const std::string& text, PlayMode fallback) {
    if (text == "loopSingle") return PlayMode::LoopSingle;
    if (text == "loopList") return PlayMode::LoopList;
    if (text == "shuffle") return PlayMode::Shuffle;
    return fallback;
}

std::string fitToJson(ImageFit fit) {
    switch (fit) {
        case ImageFit::Fill: return "fill";
        case ImageFit::Fit: return "fit";
        case ImageFit::Center: return "center";
        case ImageFit::Tile: return "tile";
    }
    return "fill";
}

ImageFit fitFromJson(const std::string& text, ImageFit fallback) {
    if (text == "fill") return ImageFit::Fill;
    if (text == "fit") return ImageFit::Fit;
    if (text == "center") return ImageFit::Center;
    if (text == "tile") return ImageFit::Tile;
    return fallback;
}

// json 封装对外只有 T_Number：整数与浮点在内部是两个 kind，asDouble() 只对浮点有效、
// asInt() 只对整数有效。组合读取：非零 double 优先，否则退回整数（两者都为 0 时结果仍是 0）。
double numberValue(const platform::json::JsonValue& value) {
    if (!value.isNumber()) return 0.0;
    const double asDouble = value.asDouble();
    if (asDouble != 0.0) return asDouble;
    return static_cast<double>(value.asInt());
}

std::string stringValue(const platform::json::JsonObject& object, const char* key,
                        const std::string& fallback) {
    const platform::json::JsonValue value = object.get(key);
    if (!value.isString()) return fallback;
    return value.asString();
}

bool boolValue(const platform::json::JsonObject& object, const char* key, bool fallback) {
    const platform::json::JsonValue value = object.get(key);
    if (!value.isBool()) return fallback;
    return value.asBool();
}

double numberField(const platform::json::JsonObject& object, const char* key, double fallback) {
    const platform::json::JsonValue value = object.get(key);
    if (!value.isNumber()) return fallback;
    return numberValue(value);
}

float clampFloat(float value, float low, float high, float fallback) {
    if (!std::isfinite(value)) return fallback;
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

// 分辨率档吸附：1080P / 2K / 4K 三档，非法值回默认 2K。
int snapImageMinPixels(int pixels) {
    constexpr int kChoices[] = {2073600, 3686400, 8294400};
    if (pixels <= 0) return 3686400;
    int best = kChoices[1];
    int bestDistance = std::abs(best - pixels);
    for (int choice : kChoices) {
        const int distance = std::abs(choice - pixels);
        if (distance < bestDistance) {
            best = choice;
            bestDistance = distance;
        }
    }
    return best;
}

std::string normalizeStoredFolder(const std::string& raw) {
    if (!folder_policy::isFolderConfigured(raw)) return {};
    return folder_policy::normalizeFolderPath(raw);
}

} // namespace

WallpaperSettings defaultWallpaperSettings() {
    return WallpaperSettings{};
}

WallpaperSettings sanitizeWallpaperSettings(const WallpaperSettings& settings) {
    WallpaperSettings out = settings;
    out.sourceFolder = normalizeStoredFolder(out.sourceFolder);
    out.imageFolderPath = normalizeStoredFolder(out.imageFolderPath);
    out.rate = clampFloat(out.rate, 0.5f, 2.0f, 1.0f);
    out.volume = clampFloat(out.volume, 0.0f, 1.0f, 1.0f);
    out.rotationIntervalSec = snapRotationIntervalSec(out.rotationIntervalSec);
    out.imageMinPixels = snapImageMinPixels(out.imageMinPixels);
    if (!std::isfinite(out.lastPlayedPosition) || out.lastPlayedPosition < 0.0) {
        out.lastPlayedPosition = 0.0;
    }
    return out;
}

std::filesystem::path wallpaperSettingsPath() {
    return core::paths::appDataDir() / "wallpaper" / "settings.json";
}

std::string folderForKind(const WallpaperSettings& settings, WallpaperKind kind) {
    return kind == WallpaperKind::Image ? settings.imageFolderPath : settings.sourceFolder;
}

WallpaperSettings loadWallpaperSettings(const std::filesystem::path& path) {
    WallpaperSettings settings = defaultWallpaperSettings();
    if (path.empty()) return settings;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return settings;

    bool readFailed = false;
    const std::string text = core::readTextFile(path, &readFailed);
    if (readFailed) {
        LOG_WARN(std::string(kLogTag) + "读取失败，使用默认值: " + path.string());
        return settings;
    }

    const core::Result<platform::json::JsonValue> parsed = platform::json::parse(text);
    if (parsed.isFail()) {
        LOG_WARN(std::string(kLogTag) + "JSON 损坏，使用默认值（" + path.string() + "）: " +
                 parsed.errorMessage());
        return settings;
    }
    if (!parsed.value().isObject()) {
        LOG_WARN(std::string(kLogTag) + "根节点不是对象，使用默认值（" + path.string() + "）");
        return settings;
    }

    const platform::json::JsonObject object = parsed.value().asObject();
    settings.kind = kindFromJson(stringValue(object, "wallpaperKind", "video"),
                                 WallpaperKind::Video);
    settings.sourceFolder = stringValue(object, "sourceFolder", "");
    settings.imageFolderPath = stringValue(object, "imageFolderPath", "");
    settings.rate = static_cast<float>(numberField(object, "rate", 1.0));
    settings.volume = static_cast<float>(numberField(object, "volume", 1.0));
    settings.muted = boolValue(object, "muted", false);
    settings.mode = modeFromJson(stringValue(object, "playMode", "loopSingle"),
                                 PlayMode::LoopSingle);
    settings.rotationIntervalSec =
        static_cast<int>(numberField(object, "rotationInterval", 300.0));
    settings.pauseOnBattery = boolValue(object, "pauseOnBattery", false);
    settings.imageMinPixels =
        static_cast<int>(numberField(object, "imageMinPixels", 3686400.0));
    settings.imageFit = fitFromJson(stringValue(object, "imageFit", "fill"), ImageFit::Fill);
    settings.lastPlayedPath = stringValue(object, "lastPlayedPath", "");
    settings.lastPlayedPosition = numberField(object, "lastPlayedPosition", 0.0);
    settings.lastImagePath = stringValue(object, "lastImagePath", "");
    return sanitizeWallpaperSettings(settings);
}

core::Result<void> saveWallpaperSettings(const std::filesystem::path& path,
                                         const WallpaperSettings& settings) {
    if (path.empty()) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                        std::string(kLogTag) + "设置路径为空");
    }
    const WallpaperSettings safe = sanitizeWallpaperSettings(settings);

    platform::json::JsonObject object;
    object.set("wallpaperKind", platform::json::makeString(kindToJson(safe.kind)));
    object.set("sourceFolder", platform::json::makeString(safe.sourceFolder));
    object.set("imageFolderPath", platform::json::makeString(safe.imageFolderPath));
    object.set("rate", platform::json::makeDouble(safe.rate));
    object.set("volume", platform::json::makeDouble(safe.volume));
    object.set("muted", platform::json::makeBool(safe.muted));
    object.set("playMode", platform::json::makeString(modeToJson(safe.mode)));
    object.set("rotationInterval", platform::json::makeInt(safe.rotationIntervalSec));
    object.set("pauseOnBattery", platform::json::makeBool(safe.pauseOnBattery));
    object.set("imageMinPixels", platform::json::makeInt(safe.imageMinPixels));
    object.set("imageFit", platform::json::makeString(fitToJson(safe.imageFit)));
    object.set("lastPlayedPath", platform::json::makeString(safe.lastPlayedPath));
    object.set("lastPlayedPosition", platform::json::makeDouble(safe.lastPlayedPosition));
    object.set("lastImagePath", platform::json::makeString(safe.lastImagePath));

    const core::Result<std::string> text =
        platform::json::stringify(platform::json::JsonValue(object), true);
    if (text.isFail()) {
        return core::Result<void>::fail(core::ErrorCode::Internal,
                                        std::string(kLogTag) + "序列化失败: " +
                                            text.errorMessage());
    }

    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return core::Result<void>::fail(core::ErrorCode::FileWriteFailed,
                                            std::string(kLogTag) + "创建目录失败 " +
                                                path.parent_path().string() + ": " +
                                                ec.message());
        }
    }
    return core::atomicWriteFile(path, text.value(), "壁纸设置");
}

} // namespace module::wallpaper