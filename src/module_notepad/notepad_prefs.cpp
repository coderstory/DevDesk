// notepad_prefs.cpp — app_config 键值存取（core::appconfig TEXT 键，need:273 批 1）。
// 键不存在 = 首次打开（静默默认）；表不存在 / SQL 失败 = 真故障（LOG_ERROR 后
// 回默认值——偏好读写失败不阻断开窗）。
#include "notepad_prefs.h"

#include <cstdlib>

#include "core/app_config.h"
#include "core/database.h"
#include "core/logging.h"

namespace module::notepad {

namespace {

constexpr const char* kKeyWidth = "notepad.win.w";
constexpr const char* kKeyHeight = "notepad.win.h";
constexpr const char* kKeyX = "notepad.win.x";
constexpr const char* kKeyY = "notepad.win.y";
constexpr const char* kKeyPosSet = "notepad.win.posSet";
constexpr const char* kKeyFontFamily = "notepad.font.family";
constexpr const char* kKeyWrap = "notepad.wrap";
constexpr const char* kKeyEditorFontSize = "notepad.editor.fontSize";

int clampInt(const std::string& raw, int fallback, int lo, int hi) {
    if (raw.empty()) return fallback;
    const int v = std::atoi(raw.c_str());
    if (v < lo || v > hi) return fallback;
    return v;
}

void setKey(core::data::SqliteDb& db, const char* key, const std::string& value) {
    auto r = core::appconfig::set(db, key, value);
    if (r.isFail()) {
        LOG_ERROR(std::string("notepad: 写 app_config[") + key + "] 失败: " +
                  r.errorMessage());
    }
}

}  // namespace

NotepadPrefs notepadLoadPrefs() {
    NotepadPrefs out;
    core::data::SqliteDb* db = core::appDatabase();
    if (db == nullptr) return out;  // 测试进程 / 极早帧：默认值
    auto get = [db](const char* key) -> std::string {
        auto r = core::appconfig::get(*db, key);
        return r.isOk() ? r.value() : std::string();
    };
    // 尺寸钳制：记忆值必须落在 [最小窗, 8192] 内（防脏库炸窗）；最小 720×480
    // 与 DslWindowConfig.minSize 同源（设计稿 §2）
    out.width = clampInt(get(kKeyWidth), 900, 720, 8192);
    out.height = clampInt(get(kKeyHeight), 640, 480, 8192);
    out.x = clampInt(get(kKeyX), 0, -8192, 8192);
    out.y = clampInt(get(kKeyY), 0, -8192, 8192);
    out.posSet = get(kKeyPosSet) == "1";
    out.fontFamily = get(kKeyFontFamily);
    out.wrap = get(kKeyWrap) != "0";  // 缺省=开
    out.editorFontSize = clampInt(get(kKeyEditorFontSize), 14, 12, 20);
    return out;
}

void notepadSaveWindowGeometry(int width, int height, int x, int y) {
    core::data::SqliteDb* db = core::appDatabase();
    if (db == nullptr) return;
    setKey(*db, kKeyWidth, std::to_string(width));
    setKey(*db, kKeyHeight, std::to_string(height));
    setKey(*db, kKeyX, std::to_string(x));
    setKey(*db, kKeyY, std::to_string(y));
    setKey(*db, kKeyPosSet, "1");
}

}  // namespace module::notepad
