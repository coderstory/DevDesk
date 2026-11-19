// notepad_module.cpp — openNotepadWindow：查重聚焦 / 新窗装配
//（need:273 批 1）。窗口规格 = 设计稿 §2：默认 900×640 / 最小 720×480 /
// 可缩放；尺寸位置记忆应用级一份（越界拉回主屏工作区）；同文件重复送达 =
// 聚焦已有窗不开新窗（v5 定案，防同文件双窗保存静默覆盖）。
#include "notepad_module.h"

#include <algorithm>
#include <memory>
#include <utility>

#include <eui/dsl_app.h>  // openWindow/DslWindowConfig（定义在 EUI glfw TU）

#include "core/logging.h"
#include "core/theme.h"
#include "core/window/window_backend.h"  // requestWindowFocus（批 0 need272）
#include "notepad_encoding.h"
#include "notepad_file_io.h"
#include "notepad_prefs.h"
#include "notepad_protect.h"  // notepadCloseShouldIntercept（关窗 veto 裁决单点）
#include "notepad_session.h"

namespace module::notepad {
namespace {

// 记忆位置越界（屏外/换屏）拉回主屏工作区：窗口整体钳回可视区（留 8px 余量）。
void clampIntoWorkArea(int& x, int& y, int w, int h) {
    int ax = 0;
    int ay = 0;
    int aw = 0;
    int ah = 0;
    core::window::getPrimaryMonitorWorkArea(ax, ay, aw, ah);
    if (aw <= 0 || ah <= 0) return;  // 工作区取不到（后端降级）→ 保持原值
    constexpr int kMargin = 8;
    x = std::max(ax + kMargin, std::min(x, ax + aw - w - kMargin));
    y = std::max(ay + kMargin, std::min(y, ay + ah - h - kMargin));
}

// 载入结果 → session 状态裁决（只读降级矩阵；F-14「仍要打开」三选批 4）。
void applyLoadedState(NotepadSession& s, NotepadLoadedFile& loaded) {
    s.rawBytes = std::move(loaded.rawBytes);
    s.editText = loaded.text;
    s.loadText = loaded.text;
    s.enc = loaded.enc;
    s.eolMode = loaded.eol;
    s.loadEol = loaded.eol;
    s.mixedEol = loaded.mixedEol;
    s.hasBom = loaded.hasBom;
    s.loadHasBom = loaded.hasBom;
    s.loadMtime = loaded.mtime;
    s.hasMtime = loaded.hasMtime;
    if (loaded.byteCount > kNotepadHugeFileBytes) {
        s.hugeFile = true;
        s.largeFile = true;
        s.wrapOn = false;  // F-19：大文件模式强制关软换行（换行钮同口径禁用）
        s.readonly = true;
        s.readonlyReason = "文件超过 10MB，已只读打开";
        return;
    }
    if (loaded.byteCount > kNotepadLargeFileBytes) s.largeFile = true;
    if (loaded.enc.binary) {
        s.binarySuspect = true;
        s.readonly = true;
        s.readonlyReason = "可能为二进制文件，已只读打开";
        return;
    }
    if (loaded.enc.bom == NotepadBom::Utf16Le ||
        loaded.enc.bom == NotepadBom::Utf16Be) {
        s.readonly = true;
        s.readonlyReason = loaded.enc.bom == NotepadBom::Utf16Le
                               ? "UTF-16LE 编码（推测），暂以原始字节只读打开"
                               : "UTF-16BE 编码（推测），暂以原始字节只读打开";
        return;
    }
    if (!loaded.enc.validUtf8) {
        s.readonly = true;
        s.readonlyReason = std::string("非 UTF-8 编码（") +
                           (loaded.enc.guessLabel != nullptr
                                ? loaded.enc.guessLabel
                                : "未知（推测）") +
                           "）";
    }
    if (s.largeFile) s.wrapOn = false;  // F-19：大文件模式强制关软换行（>1MB 可编辑档）
}

}  // namespace

void openNotepadWindow(const std::string& path) {
    if (path.empty()) return;
    const std::string norm = notepadNormKeyFor(path);
    if (NotepadSession* existing = findSessionByNormKey(norm)) {
        // 同文件查重命中（注册表存活 = 窗活着）→ 聚焦 + toast，不开新窗（v5/F-20①）
        if (existing->handle != nullptr) {
            core::window::requestWindowFocus(
                static_cast<core::window::Handle>(existing->handle));
        }
        sessionToast(*existing, "已在原窗口打开，已为你聚焦");
        LOG_INFO("记事本查重命中，聚焦已有窗: " + path +
                 "（pageId=" + existing->pageId + "）");
        return;
    }

    auto session = std::make_unique<NotepadSession>();
    NotepadSession& s = *session;
    s.path = path;
    s.normKey = norm;
    s.title = notepadWindowTitleFor(path);
    s.pageId = "notepad." + std::to_string(detail::nextPageIdSeq());
    const NotepadPrefs prefs = notepadLoadPrefs();
    s.editorFontSize = prefs.editorFontSize;
    s.fontFamily = prefs.fontFamily;  // 空 = 系统默认（F-09）
    s.wrapOn = prefs.wrap;            // F-19 状态层（渲染旋钮缺，见 session.h 注）

    // 流程 A2 异常分支：读失败仍开窗，窗内错误占位；窗口照开照关
    auto loaded = notepadLoadFile(path);
    if (loaded.isFail()) {
        s.loadError = loaded.errorCode() == core::ErrorCode::FileNotFound
                          ? "文件不存在或已被移动"
                          : "无读取权限或打开失败";
        LOG_WARN("记事本载入失败（错误占位开窗）: " + path + " — " +
                 loaded.errorMessage());
    } else {
        applyLoadedState(s, loaded.value());
    }

    NotepadSession* raw = &s;
    detail::sessionRegistry().push_back(std::move(session));

    // 窗口规格：尺寸记忆应用级一份（首次 = 900×640 默认）；越界拉回工作区。
    // bug282 独立普通窗三件套（设计稿口径）：底色钉不透明（三档窗口效果下的
    // 半透 alpha 不连坐子窗）、材质固定 None（全局磨砂/半透 backdrop 不贯通）、
    // 不吃全局 clearColor 广播（切档回填 alpha<1 会重新漏底）+ 内容不延伸进
    // 标题栏（主窗自绘壳的全局 FSCV 不连坐子窗，保原生标题栏条）。
    eui::Color opaqueBackground = core::theme::current().colors.background;
    opaqueBackground.a = 1.0f;
    app::DslWindowConfig config;
    config.title(raw->title)
        .pageId(raw->pageId)
        .windowSize(prefs.width, prefs.height)
        .minSize(720, 480)  // need269 已落地（dsl_app.h:288-301）
        .resizable(true)
        .focusOnShow(true)
        .clearColor(opaqueBackground)
        .ignoreClearColorOverride(true)
        .windowEffect(core::platform::WindowEffect::None)
        .titlebarExtendContent(false)
        // bug373：开窗即 restore+前置（requestWindowFocus：Win 端 iconic 先
        // SW_RESTORE、前台锁抢不到则闪任务栏引导；macOS deminiaturize+激活）。
        // 主窗最小化/后台时双击文件开编辑器，新窗必须顶到用户眼前，而不是
        // 跟随进程驻留在后台/最小化态。查重命中路径同款（本文件上方聚焦分支）。
        .onWindowCreated([raw](core::window::Handle h) {
            raw->handle = h;
            core::window::requestWindowFocus(h);
        })
        .onWindowDestroyed([raw](core::window::Handle) {
            notepadSessionWindowDestroyed(raw);  // 注销防悬垂 + 几何记忆落库
        })
        .onCloseRequested([raw](core::window::Handle) {
            // 未保存保护（F-05）：干净/已豁免（旗标消费）直通放行；脏 → 弹
            // 该窗自己的三选并返回 false 拦截本次 close（GLFW/WM_CLOSE 不销毁）。
            if (!notepadCloseShouldIntercept(*raw)) return true;
            raw->protectOpen = true;
            raw->protectQuitContext = false;  // 关窗语境：保存成功走 requestWindowClose
            return false;
        })
        .onKeyEvent([raw](const eui::KeyEvent& key) {
            notepadHandleKeySession(*raw, key);
        });
    if (prefs.posSet) {
        int x = prefs.x;
        int y = prefs.y;
        clampIntoWorkArea(x, y, prefs.width, prefs.height);
        config.windowPosition(x, y);
    }
    app::openWindow(config, [raw](eui::Ui& ui, const eui::Screen& screen) {
        notepadComposeSession(*raw, ui, screen);
    });
    LOG_INFO("记事本开窗: " + path + "（pageId=" + raw->pageId +
             "，readonly=" + (raw->readonly ? "1" : "0") + "）");
}

}  // namespace module::notepad
