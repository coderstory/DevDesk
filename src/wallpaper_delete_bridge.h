// src/wallpaper_delete_bridge.h
// 壁纸素材删除确认弹窗桥（need356 F-02）：托盘「删除当前视频/图片…」→ 主窗确认弹窗
// → 确认后经壁纸托盘桥转发 service.requestDeleteCurrent()。模式同 quit_save_bridge
// （函数指针缝，真实现由 main.cpp 装配段注入；本头被 main.cpp / app_sidebar.cpp /
// wallpaper_service.cpp 引用）。
//
// 画在哪一层：壁纸模块无主窗 compose 顶层权限（模块 compose 在 router 页面子树内），
// 故弹窗画在主窗 compose 尾部（app_sidebar），与退出确认同款。
// 线程：postResult 任意线程可入（mutex 保护的结果队列）；takeResults 只在主线程
// （showAppNotice 非线程安全，禁 worker 直投 toast）。
#pragma once

#include <eui/dsl.h>

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace app::wallpaper_delete {

// 弹窗打开时的目标（仅供参考展示）：执行时服务重新取当前项，不信任本快照。
struct DeleteTarget {
    bool available = false;
    std::string fileName;
    bool image = false;
};

using TargetProvider = DeleteTarget (*)(void);
using ConfirmHandler = void (*)(void);
using ShowMainHandler = void (*)(void);

// 传 nullptr 对 = 恢复空桩（未装配 = 弹窗永不打开，确认无动作）。
void setHandlers(TargetProvider target, ConfirmHandler confirm);
// 唤起主窗（主窗隐藏/最小化时托盘触发，弹窗必须画在可见窗上；need300 同款）。
void setShowHandler(ShowMainHandler show);

// 托盘删除项的落点（主线程）：取目标 → 唤起主窗 → 置开态。目标不可删 = no-op。
void openConfirm();

// 主窗 compose 尾部调用：未开 = 零开销早退。
void drawDialog(eui::Ui& ui, const eui::Screen& screen);

// 删除结果 → toast 文案（纯函数，headless 可测）。
// failureKind = platform::file_trash::TrashFailure 的整数值（ok=true 时忽略）。
struct ToastText {
    std::string title;
    std::string message;
};
ToastText toastTextForResult(bool ok, const std::string& fileName, int failureKind);

// worker 线程写结果队列（mutex）；主线程 takeResults 后逐条 showAppNotice。
void postResult(bool ok, const std::string& fileName, int failureKind);
std::vector<std::pair<std::string, std::string>> takeResults();

// ── 单测观察缝（最小缝纪律同 quit_save_bridge）──
bool dialogOpenForTest();
void resetForTest();

}  // namespace app::wallpaper_delete

static_assert(std::is_same_v<decltype(&app::wallpaper_delete::setHandlers),
                             void (*)(app::wallpaper_delete::TargetProvider,
                                      app::wallpaper_delete::ConfirmHandler)>,
              "wallpaper_delete_bridge: setHandlers 签名漂移");
static_assert(std::is_same_v<decltype(&app::wallpaper_delete::setShowHandler),
                             void (*)(app::wallpaper_delete::ShowMainHandler)>,
              "wallpaper_delete_bridge: setShowHandler 签名漂移");
static_assert(std::is_same_v<decltype(&app::wallpaper_delete::openConfirm),
                             void (*)(void)>,
              "wallpaper_delete_bridge: openConfirm 签名漂移");
static_assert(std::is_same_v<decltype(&app::wallpaper_delete::postResult),
                             void (*)(bool, const std::string&, int)>,
              "wallpaper_delete_bridge: postResult 签名漂移");
