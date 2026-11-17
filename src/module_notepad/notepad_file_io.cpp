// notepad_file_io.cpp — 读/写/编码回写实现（need:273 批 1）。
// 「打开不擅自转换」强保证：载入留 rawBytes；干净缓冲不落盘（saveSession 短路）；
// 脏缓冲保存 = BOM 回写 + 行尾档全文回写（技术方案卡 §八）。
#include "notepad_file_io.h"

#include <system_error>
#include <utility>  // std::move（载入结果搬运；std include 守卫要求显式声明）

#include "core/atomic_write.h"
#include "core/fs_text.h"
#include "core/logging.h"
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
    // 落盘成功：基线/镜像前移 + mtime 重取（原子替换后必变），脏态清零（标题联动）
    s.rawBytes = bytes;
    s.loadText = s.editText;
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

}  // namespace module::notepad
