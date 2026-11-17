// notepad_file_io.cpp — 读/写/编码回写实现（need:273 批 1）。
// 「打开不擅自转换」强保证：载入留 rawBytes；干净缓冲不落盘（saveSession 短路）；
// 脏缓冲保存 = BOM 回写 + 行尾档全文回写（技术方案卡 §八）。
#include "notepad_file_io.h"

#include <system_error>
#include <utility>  // std::move（载入结果搬运；std include 守卫要求显式声明）

#include <eui/dsl_app.h>    // app::requestUpdate（另存为成功后即时刷新）
#include <eui/platform.h>   // saveFileDialog（FD-1 need265，DevDesk 侧首个消费方）

#include "core/atomic_write.h"
#include "core/fs_text.h"
#include "core/logging.h"
#include "core/notepad_file_types.h"  // notepadExtensions（另存对话框过滤器）
#include "core/window/window_backend.h"  // setWindowTitle（迁移后标题联动）
#include "notepad_session.h"

namespace module::notepad {

core::Result<NotepadLoadedFile> notepadLoadFile(const std::string& path) {
    const std::filesystem::path p(path);  // fs::path 宽路径纪律（Windows 窄化陷阱）
    std::error_code ec;
    const auto size = std::filesystem::file_size(p, ec);
    if (ec) {
        return core::Result<NotepadLoadedFile>::fail(
            core::ErrorCode::FileNotFound, "文件不存在或已被移动: " + path);
    }
    NotepadLoadedFile out;
    out.byteCount = static_cast<size_t>(size);
    bool readFailed = false;
    out.rawBytes = core::readTextFile(p, &readFailed);
    if (readFailed) {
        return core::Result<NotepadLoadedFile>::fail(
            core::ErrorCode::FileReadFailed, "无读取权限或打开失败: " + path);
    }
    out.enc = notepadProbeEncoding(out.rawBytes);
    out.hasBom = (out.enc.bom == NotepadBom::Utf8);
    out.eol = notepadDetectEol(notepadStripBom(out.rawBytes));
    // 缓冲统一 LF（混合标志随归一回填）；UTF-16/非 UTF-8 文本域按原字节展示
    //（只读降级在 session 侧裁决，这里不做语义转换）
    out.text = notepadNormalizeToLf(notepadStripBom(out.rawBytes), &out.mixedEol);
    const auto mtime = std::filesystem::last_write_time(p, ec);
    if (!ec) {
        out.mtime = mtime;
        out.hasMtime = true;
    }
    return core::Result<NotepadLoadedFile>::ok(std::move(out));
}

std::string notepadEncodeBuffer(const std::string& textLf, bool hasBom,
                                NotepadEol eol) {
    std::string body = notepadApplyEol(textLf, eol);
    if (!hasBom) return body;
    std::string out;
    out.reserve(body.size() + 3);
    out.append(kUtf8Bom, 3);
    out.append(body);
    return out;
}

core::Result<void> notepadWriteFile(const std::string& path,
                                    const std::string& content) {
    return core::atomicWriteFile(std::filesystem::path(path), content, "记事本保存");
}

void notepadSaveSession(NotepadSession& s) {
    // 错误占位窗 / 只读降级 / 二进制拒编：无保存语义（保存钮已禁用，M+S 同口径拦截）
    if (!s.loadError.empty() || s.readonly || s.binarySuspect) return;
    // 干净短路（F-03）：无改动不落盘——「打开不擅自转换」的落盘侧，混合 EOL
    // 文件不会被一次无害的 M+S 顺手规范化
    if (!s.dirty) {
        sessionToast(s, "无改动");
        return;
    }
    const std::filesystem::path p(s.path);  // fs::path 宽路径纪律
    // mtime 拦截（F-03）：载入后被外部改动 → 拦截覆盖（三选「覆盖/另存为/取消」
    // 对话框批 4 接管，本批安全侧=不写）。载入时没取到 mtime 则无从比对，放行
    if (s.hasMtime) {
        std::error_code ec;
        const auto mtime = std::filesystem::last_write_time(p, ec);
        if (ec || mtime != s.loadMtime) {
            sessionToast(s, "文件已被其他程序修改，已拦截保存");
            LOG_WARN("记事本 mtime 拦截: " + s.path);
            return;
        }
    }
    // 脏缓冲 = BOM 回写 + 行尾档全文重编码；干净直通 rawBytes（字节级往返一致）
    const std::string bytes = s.dirty
                                  ? notepadEncodeBuffer(s.editText, s.hasBom, s.eolMode)
                                  : s.rawBytes;
    const auto result = notepadWriteFile(s.path, bytes);
    if (result.isFail()) {
        sessionToast(s, "保存失败: " + result.errorMessage());  // 写失败保持未保存
        LOG_WARN("记事本保存失败: " + s.path + " " + result.errorMessage());
        return;
    }
    // 落盘成功：基线/镜像前移（含编码/行尾基线——状态栏点切脏判定基准）+
    // mtime 重取（原子替换后必变），脏态清零（标题联动）
    s.rawBytes = bytes;
    s.loadText = s.editText;
    s.loadHasBom = s.hasBom;
    s.loadEol = s.eolMode;
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(p, ec);
    if (ec) {
        s.hasMtime = false;
    } else {
        s.loadMtime = mtime;
        s.hasMtime = true;
    }
    sessionSetDirty(s, false);
    sessionToast(s, "已保存");
    LOG_INFO("记事本已保存: " + s.path);
}

core::Result<void> notepadSaveSessionAsTo(NotepadSession& s,
                                          const std::string& newPath) {
    // 字节与保存同口径：脏缓冲 = BOM 回写 + 行尾档全文重编码；干净缓冲直通
    // rawBytes（字节级往返一致 =「打开不擅自转换」强保证）
    const std::string bytes = s.dirty
                                  ? notepadEncodeBuffer(s.editText, s.hasBom, s.eolMode)
                                  : s.rawBytes;
    const auto result = notepadWriteFile(newPath, bytes);
    if (result.isFail()) return result;  // 写失败零副作用：session 未落任何态
    // session 全量迁移（设计稿 §4.2「成功→文件名/语言随新路径更新，本窗转为
    // 新文件」）：normKey 变化影响查重键；基线三件前移到刚写入字节对应态
    const std::string oldPath = s.path;
    s.path = newPath;
    s.normKey = notepadNormKeyFor(newPath);
    s.title = notepadWindowTitleFor(newPath);
    s.rawBytes = bytes;
    s.loadText = s.editText;
    s.loadHasBom = s.hasBom;
    s.loadEol = s.eolMode;
    s.mixedEol = false;  // 归一档全文重写后不再混合
    s.enc = notepadProbeEncoding(bytes);
    s.language.clear();  // 语言随新扩展名（compose 懒求值重算）
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(
        std::filesystem::path(newPath), ec);  // fs::path 宽路径纪律
    if (ec) {
        s.hasMtime = false;
    } else {
        s.loadMtime = mtime;
        s.hasMtime = true;
    }
    sessionSetDirty(s, false);  // 脏态清零（●前缀随标题联动清除）
    if (s.handle != nullptr) {
        core::window::setWindowTitle(
            static_cast<core::window::Handle>(s.handle), s.title.c_str());
    }
    LOG_INFO("记事本另存为: " + oldPath + " → " + s.path);
    return core::Result<void>::ok();
}

void notepadSaveSessionAs(NotepadSession& s) {
    // 错误占位窗 / 只读降级 / 二进制拒编：无另存为语义（钮禁用 + M+Shift+S
    // 同口径拦截；只读窗编辑镜像可能是原字节乱码，重编码会写坏数据）
    if (!s.loadError.empty() || s.readonly || s.binarySuspect) return;
    eui::platform::SaveFileDialogOptions options;
    options.prompt = "另存为";
    options.defaultName = s.title;  // 默认文件名 = 当前文件名（PRD F-04）
    const size_t dot = s.title.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        options.defaultExtension = s.title.substr(dot + 1);
    }
    options.filterName = "文本与代码文件";
    for (const std::string_view ext : core::notepad::notepadExtensions()) {
        options.allowedExtensions.emplace_back(ext);
    }
    const eui::platform::FileDialogResult result =
        eui::platform::saveFileDialog(options);
    if (!result.selected()) return;  // 取消 → 零副作用（保持编辑态，PRD F-04）
    const auto done =
        notepadSaveSessionAsTo(s, result.paths.front());
    if (done.isFail()) {
        sessionToast(s, "另存为失败: " + done.errorMessage());  // 保持未保存态
        LOG_WARN("记事本另存为失败: " + s.path + " " + done.errorMessage());
        return;
    }
    sessionToast(s, "已另存为: " + s.title);
    app::requestUpdate();  // 状态栏/标题即时刷新
}

}  // namespace module::notepad
