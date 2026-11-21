// src/wallpaper_delete_bridge.cpp
// 壁纸素材删除确认弹窗桥实现（语义见 wallpaper_delete_bridge.h）。弹窗用 #156
// 标准壳危险变体（删除是破坏性动作，但可回收：进系统废纸篓）；主题 token 每帧
// 取 current()（跟随换肤）。结果队列跨线程：worker postResult，主线程 drain。
#include "wallpaper_delete_bridge.h"

#include "core/component_theme.h"
#include "core/theme.h"
#include "platform/file_trash.h"  // TrashFailure 档位（文案映射）
#include "ui_kit/dialog_shell.h"

#include <cstddef>
#include <mutex>

namespace app::wallpaper_delete {

namespace {

TargetProvider g_target = nullptr;
ConfirmHandler g_confirm = nullptr;
ShowMainHandler g_showMain = nullptr;
bool g_dialogOpen = false;

// 打开时取的目标（仅用于标题/文件名展示）；执行时服务重新取当前项。
DeleteTarget g_openTarget{};

std::mutex g_resultsMutex;
std::vector<std::pair<std::string, std::string>> g_results;

// 弹窗文件名展示：超长中间截断，保留扩展名（文件名进弹窗唯一入口）。
// 例：aaaaaaaaaaaaaaaaaaaaaaaaaaaa.mp4 → aaaaaaaa…aaaaaaaa.mp4
std::string middleTruncateFileName(const std::string& fileName, std::size_t maxChars) {
    if (fileName.size() <= maxChars || maxChars < 8) {
        return fileName;
    }
    const std::string::size_type dot = fileName.find_last_of('.');
    // 扩展名（含点）只在「点不在首位且不太长」时保留，否则整体当无扩展名。
    std::string extension;
    std::size_t stemLen = fileName.size();
    if (dot != std::string::npos && dot > 0 && fileName.size() - dot <= 8) {
        extension = fileName.substr(dot);
        stemLen = dot;
    }
    const std::size_t keep = maxChars - extension.size() - 1;  // 1 = 省略号
    if (keep < 2) {
        return fileName.substr(0, maxChars);
    }
    const std::size_t head = (keep + 1) / 2;
    const std::size_t tail = keep - head;
    return fileName.substr(0, head) + "\xE2\x80\xA6" +  // …
           fileName.substr(stemLen - tail, tail) + extension;
}

const char* folderNameOf(const std::string& path) {
    const std::string::size_type slash = path.find_last_of("/\\");
    return path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
}

}  // namespace

void setHandlers(TargetProvider target, ConfirmHandler confirm) {
    g_target = target;
    g_confirm = confirm;
}

void setShowHandler(ShowMainHandler show) {
    g_showMain = show;
}

void openConfirm() {
    if (g_target == nullptr) {
        return;
    }
    const DeleteTarget target = g_target();
    if (!target.available) {
        return;
    }
    g_openTarget = target;
    // 弹窗画在主窗：主窗隐藏/最小化（托盘触发态）时先唤起，否则用户看不见也无法响应。
    if (g_showMain != nullptr) {
        g_showMain();
    }
    g_dialogOpen = true;
}

ToastText toastTextForResult(bool ok, const std::string& fileName, int failureKind) {
    if (ok) {
        return {"已移到废纸篓", fileName};
    }
    // failureKind = platform::file_trash::TrashFailure 整数值（桥不懂平台枚举，
    // 用裸 int 保持缝本体零平台依赖）。
    switch (static_cast<platform::file_trash::TrashFailure>(failureKind)) {
        case platform::file_trash::TrashFailure::NotFound:
            return {"删除失败", "文件已被移动或占用，文件仍在文件夹里"};
        case platform::file_trash::TrashFailure::PermissionDenied:
            return {"删除失败", "没有权限删除这个文件，文件仍在文件夹里"};
        case platform::file_trash::TrashFailure::TrashUnavailable:
            return {"删除失败", "系统废纸篓当前不可用，文件仍在文件夹里"};
        case platform::file_trash::TrashFailure::Busy:
            return {"删除失败", "文件正被占用，文件仍在文件夹里"};
        case platform::file_trash::TrashFailure::None:
            break;  // 成功已由 ok 覆盖；落到兜底
    }
    return {"删除失败", "文件仍在文件夹里"};
}

void postResult(bool ok, const std::string& fileName, int failureKind) {
    const ToastText text = toastTextForResult(ok, fileName, failureKind);
    std::lock_guard<std::mutex> lock(g_resultsMutex);
    g_results.emplace_back(text.title, text.message);
}

std::vector<std::pair<std::string, std::string>> takeResults() {
    std::lock_guard<std::mutex> lock(g_resultsMutex);
    std::vector<std::pair<std::string, std::string>> out;
    out.swap(g_results);
    return out;
}

void drawDialog(eui::Ui& ui, const eui::Screen& screen) {
    if (!g_dialogOpen) {
        return;
    }
    const core::theme::Tokens& t = core::theme::current();
    const components::theme::ThemeColorTokens wc =
        core::theme::componentTokensWeChat(t);

    ui_kit::DialogShellSpec spec;
    spec.id = "app.wallpaperdelete";
    spec.title = g_openTarget.image ? "把当前图片移到废纸篓？"
                                    : "把当前视频移到废纸篓？";
    spec.titleColor = t.colors.errorText;  // 破坏性动作标题提强度（规范 §3.3）
    spec.dangerConfirm = true;             // 主钮 = 移到废纸篓
    spec.open = true;
    // need337 紧凑档：内容实占 ≈ 头 69 + 正文 66 + 底 73 ≈ 208，取 212 不空。
    spec.compact = true;
    spec.height = 212.0f;
    // z=1200（>#156 规范档 1000）：根层弹窗须压过页内常驻 overlay（页内 toast
    // z1100 抬高页面子树 max zIndex，取 1000 会整层落后 + 点击被截胡）。
    spec.zIndex = 1200;
    spec.onOpenChange = [](bool open) {
        if (!open) {
            g_dialogOpen = false;
        }
    };
    spec.cancel = {"取消", [] { g_dialogOpen = false; }};
    spec.confirm = {"移到废纸篓", [] {
                        g_dialogOpen = false;
                        if (g_confirm != nullptr) {
                            g_confirm();
                        }
                    }};
    ui_kit::drawDialogShell(ui, screen, spec, t, wc,
                            [&](const ui_kit::DialogContentArea& area) {
            // 文本宽口径：drawDialogText 自减 inset（见 quit_save_bridge 同款）
            const float textW = area.w + ui_kit::kDialogBodyInsetX;
            ui_kit::drawDialogText(ui, "app.wallpaperdelete.msg.name", textW,
                                   middleTruncateFileName(
                                       folderNameOf(g_openTarget.fileName), 40),
                                   t.sizes.fontSizeBody, t.colors.textPrimary,
                                   {.y = area.y, .height = 20.0f,
                                    .lineHeight = 20.0f});
            ui_kit::drawDialogText(
                ui, "app.wallpaperdelete.msg.hint", textW,
                "文件会移到系统废纸篓，之后可从废纸篓恢复。删除后会自动播放下一个。",
                t.sizes.fontSizeSmall, t.colors.textTertiary,
                {.y = area.y + 30.0f, .height = 18.0f, .lineHeight = 18.0f});
        });
}

bool dialogOpenForTest() {
    return g_dialogOpen;
}

void resetForTest() {
    g_target = nullptr;
    g_confirm = nullptr;
    g_showMain = nullptr;
    g_dialogOpen = false;
    g_openTarget = DeleteTarget{};
    std::lock_guard<std::mutex> lock(g_resultsMutex);
    g_results.clear();
}

}  // namespace app::wallpaper_delete
