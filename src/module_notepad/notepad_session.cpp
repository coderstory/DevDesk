// notepad_session.cpp — session 注册表 / 规范路径查重 / 标题 / 脏态联动
//（need:273 批 1）。生命周期不变量照抄 md_viewer_window 先例：注册表 erase
// 只发生在 onWindowDestroyed（销毁前回调）→ compose 闭包捕获的裸指针与窗口同寿。
#include "notepad_session.h"

#include <cstdint>
#include <memory>
#include <system_error>
#include <vector>

#include "core/logging.h"
#include "core/notepad_file_types.h"  // notepadAsciiLower（扩展名大小写折叠）
#include "core/window/window_backend.h"  // setWindowTitle（脏态前缀）/ getWindowPos（关窗记忆）
#include "notepad_module.h"
#include "notepad_prefs.h"

namespace module::notepad {

namespace {

std::vector<std::unique_ptr<NotepadSession>>& sessions() {
    static std::vector<std::unique_ptr<NotepadSession>> registry;
    return registry;
}

}  // namespace

namespace detail {

std::vector<std::unique_ptr<NotepadSession>>& sessionRegistry() {
    return sessions();
}

std::uint64_t nextPageIdSeq() {
    // 全局递增页序号：跨注册表清空单调递增，pageId 唯一性不依赖注册表现状
    //（v5：仅窗口内部唯一标识，不用于同路径双开）
    static std::uint64_t seq = 0;
    return ++seq;
}

}  // namespace detail

std::string notepadWindowTitleFor(const std::string& path, size_t maxBytes) {
    const size_t sep = path.find_last_of("/\\");
    const std::string name =
        sep == std::string::npos ? path : path.substr(sep + 1);
    if (name.size() <= maxBytes) return name;
    const std::string ellipsis = "…";  // 3 字节
    const size_t keep =
        maxBytes > ellipsis.size() + 1 ? maxBytes - ellipsis.size() : 1;
    const size_t head = keep - keep / 3;
    const size_t tail = keep - head;
    const auto snapToCodepoint = [&name](size_t pos) {
        while (pos > 0 &&
               (static_cast<unsigned char>(name[pos]) & 0xC0) == 0x80) {
            --pos;
        }
        return pos;
    };
    const size_t headEnd = snapToCodepoint(head);
    const size_t tailBegin =
        snapToCodepoint(name.size() > tail ? name.size() - tail : 0);
    if (headEnd == 0 || tailBegin <= headEnd) return name.substr(0, maxBytes);
    return name.substr(0, headEnd) + ellipsis + name.substr(tailBegin);
}

std::string notepadNormKeyFor(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path p(path);
    auto canon = std::filesystem::weakly_canonical(p, ec);
    if (!ec) return canon.string();
    canon = std::filesystem::absolute(p, ec);  // 文件不存在也查重（错误占位窗）
    return canon.lexically_normal().string();
}

NotepadSession* findSessionByNormKey(const std::string& normKey) {
    for (auto& s : sessions()) {
        if (s->normKey == normKey) return s.get();
    }
    return nullptr;
}

void notepadSessionWindowDestroyed(NotepadSession* s) {
    // 关窗记忆回写（应用级一份，设计稿 §2）：尺寸取 compose 最近回填；位置趁
    // 句柄仍有效（销毁前回调契约）读窗位。首窗从未 compose（异常路径）不落库。
    if (s->sizeSeen && s->handle != nullptr) {
        int x = 0;
        int y = 0;
        core::window::getWindowPos(static_cast<core::window::Handle>(s->handle),
                                   x, y);
        notepadSaveWindowGeometry(s->lastW, s->lastH, x, y);
    }
    for (auto it = sessions().begin(); it != sessions().end(); ++it) {
        if (it->get() == s) {
            sessions().erase(it);
            return;
        }
    }
}

void sessionToast(NotepadSession& s, const std::string& message) {
    s.toastMessage = message;
    s.toastUntil = std::chrono::steady_clock::now() +
                   std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                       std::chrono::duration<float>(1.5f));
}

void sessionSetDirty(NotepadSession& s, bool dirty) {
    // 「不保存」豁免只对决策当时的编辑代生效：任何新的脏化（含已脏再编辑、
    // 早返回路径）都作废豁免旗标，重新进保护。
    if (dirty) s.skipCloseConfirm = false;
    if (s.dirty == dirty) return;
    s.dirty = dirty;
    if (s.handle != nullptr) {
        // 标题 = 脏态「• 」前缀（macOS 文档惯例）+ 文件名；setWindowTitle
        // 即时生效（批 0 能力，need272）
        const std::string title = (dirty ? "• " : "") + s.title;
        core::window::setWindowTitle(
            static_cast<core::window::Handle>(s.handle), title.c_str());
    }
}

size_t notepadSessionCountForTest() { return sessions().size(); }

std::string notepadSessionPageIdForTest(size_t index) {
    return index < sessions().size() ? sessions()[index]->pageId : std::string();
}

void resetNotepadSessionsForTest() { sessions().clear(); }

bool notepadSessionHasExtension(const NotepadSession& s,
                                std::string_view lowerExt) {
    const std::string& key = s.normKey;
    if (key.size() < lowerExt.size()) return false;
    for (size_t i = 0; i < lowerExt.size(); ++i) {
        const char actual =
            core::notepad::notepadAsciiLower(key[key.size() - lowerExt.size() + i]);
        if (actual != lowerExt[i]) return false;
    }
    return true;
}

NotepadToolbarMatrix notepadToolbarMatrixFor(const NotepadSession& s) {
    NotepadToolbarMatrix m;
    const bool errorState = !s.loadError.empty();
    const bool editable = !errorState && !s.readonly && !s.binarySuspect;
    // need380：格式化面扩 .sql（SQL 格式化器 notepad_sql_format；.json 原口径）
    m.showFormat = !errorState && (notepadSessionHasExtension(s, ".json") ||
                                   notepadSessionHasExtension(s, ".sql"));
    m.showPreview = !errorState && (notepadSessionHasExtension(s, ".md") ||
                                    notepadSessionHasExtension(s, ".markdown"));
    m.formatEnabled = editable && !s.previewOn;
    m.previewEnabled = !s.largeFile;
    m.findToggleEnabled = !s.previewOn;
    // P2-1（need289 走查）：错误占位窗字体面禁用（设计稿 §7.2 联动矩阵——
    // A± 原已守，触发器漏）
    m.fontControlsEnabled = !s.previewOn && !errorState;
    return m;
}

}  // namespace module::notepad
