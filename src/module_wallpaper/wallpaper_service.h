// src/module_wallpaper/wallpaper_service.h
// 壁纸服务骨架：状态快照 + 命令 + 后台扫描/计时线程 + 让路决策。
//
// 线程模型（需求 §3.3）：
//   - UI 主线程只调命令与 snapshot()；两者都线程安全，不直接碰扫描/播放。
//   - 扫描线程：懒唤醒的单工作线程，同一时刻只有一个扫描在跑；每次请求递增 generation，
//     旧代次结果直接丢弃（防旧扫描覆盖新目录）。
//   - 计时线程：250ms 一拍，只在 mu_ 内推进轮换并发出装载命令。
//   - stop() 置停止标志 + 唤醒两个线程后 join；扫描函数每个目录条目检查取消，join 有界
//     到「一条目录条目」的处理时间。
//   - 锁顺序固定：mu_ → playerMu_。playerMu_ 内不得回调本类任何加 mu_ 的方法。
//
// 本批边界：系统让路事件源（全屏/锁屏/熄屏/睡眠/电池）在 P3b watcher 接入；此处只有
// 仲裁器与 test-only 注入缝，不得对外宣称自动让路已生效。生产装配 = WorkerW 宿主 +
// mpv 播放器；注入测试播放器时**不建桌面宿主、不 spawn mpv**（单测不碰真桌面）。
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "module_wallpaper/core/arbiter.h"
#include "module_wallpaper/core/image_library.h"
#include "module_wallpaper/core/model.h"
#include "module_wallpaper/core/rotation.h"
#include "module_wallpaper/core/settings.h"
#include "module_wallpaper/core/video_library.h"
#include "module_wallpaper/platform/player_backend.h"
#include "module_wallpaper/platform/wallpaper_host.h"

namespace module::wallpaper {

// UI 批次唯一消费结构（签名冻结，字段语义见行内注释）。
struct WallpaperSnapshot {
    WallpaperKind kind = WallpaperKind::Video;
    Availability availability = Availability::FolderUnconfigured;
    std::string folderPath;  // 当前来源目录（空 = 未配置）
    bool scanning = false;
    int videoCount = 0;
    int imageTotal = 0;
    int imagePassing = 0;  // 本批恒等于 imageTotal（像素档过滤待 P3 解码接入）
    int imageFiltered = 0; // 本批恒 0
    PlayMode mode = PlayMode::LoopSingle;
    int rotationIntervalSec = 300;
    bool paused = false;        // true = 手动暂停或系统让路（不区分来源）
    bool manualPaused = false;  // 手动暂停位为真即 true，可与 systemHolds 并存
    HoldReasonSet systemHolds = 0;  // 只含系统原因，永不含 ManualPause 位
    float rate = 1.0f;
    float volume = 1.0f;
    bool muted = false;
    bool pauseOnBattery = false;
    int imageMinPixels = 3686400;
    ImageFit imageFit = ImageFit::Fill;
    int secondsUntilRotation = -1;  // -1 = 不轮换
    float rotationProgress = 0.0f;  // 0..1
    std::string statusTitle;        // presentation 生成，UI 不拼文案
    std::string statusSubtitle;
    std::string modeLabel;
    std::string countdownLabel;
    // 宿主/mpv 后台失败的最近原因（中文，可直接展示；空 = 无错误）。
    // 纯追加字段：Batch1 既有字段语义不变。
    std::string playbackError;
    // 纯追加字段（图片进轮换池批次）：rotation_.itemCount() 的快照（视频+图片混合池）。
    int poolCount = 0;
    // 混合池中图片条数；Image 来源恒等于 imageTotal。UI 副题按池构成分派文案。
    int poolImageCount = 0;
    // 当前装载的轮换项是图片（仅 Video 来源的混合池置位；Image 来源沿用单张/轮播语义）。
    // 为真时 mpv 对该 item 无 pause/seek 语义，UI 停用暂停控件、倒计时照常。
    bool currentItemImage = false;
};

class WallpaperService {
public:
    WallpaperService();
    ~WallpaperService();
    WallpaperService(const WallpaperService&) = delete;
    WallpaperService& operator=(const WallpaperService&) = delete;

    // 幂等：重复 start/stop 不重复建线程、不重复扫描。
    void start();
    void stop();

    WallpaperSnapshot snapshot() const;

    void setKind(WallpaperKind kind);
    void setMode(PlayMode mode);
    void setRotationIntervalSec(int seconds);
    void setRate(float rate);
    void setVolume(float volume);
    void setMuted(bool muted);
    void setPauseOnBattery(bool pauseOnBattery);
    void setImageMinPixels(int minPixels);
    void setImageFit(ImageFit fit);

    // 规范化 + 校验路径后写入当前来源目录并持久化。失败填 error（中文、可直接展示），返回 false。
    bool selectFolder(const std::string& path, std::string* error);

    void rescan();
    void toggleManualPause();
    void next();

    // ---- 测试缝（仅测试用，生产代码不得调用）----
    // 扫描器/玩家/时钟必须在 start() **之前**注入：扫描线程无锁读取 scanner/clock，
    // start 后再替换属数据竞争（本批不为测试缝加锁）。
    // setPlayerForTest 同时把服务切到「无桌面装配」：不建 WorkerW 宿主、不 spawn mpv。
    using VideoScanner = std::function<VideoScanResult(const std::string& folder)>;
    using ImageScanner = std::function<ImageScanResult(const std::string& folder, int minPixels)>;
    using Clock = std::function<RotationClock::time_point()>;

    void setVideoScannerForTest(VideoScanner scanner);
    void setImageScannerForTest(ImageScanner scanner);
    void setPlayerForTest(std::unique_ptr<PlayerBackend> player);
    void setClockForTest(Clock clock);
    void setSettingsPathForTest(std::string path);
    // 注入一个系统让路原因（P3 watcher 之前用它验证快照合并与暂停/恢复路径）。
    void setSystemHoldForTest(HoldReason reason, bool active);

private:
    RotationClock::time_point nowLocked() const;
    std::string currentFolderLocked() const;
    void requestScanLocked();
    void scanLoop();
    void tickLoop();

    // 视频来源扫描落库：池 = 视频 + 同目录图片（混合池），videoCount 只计视频。
    void applyVideoScanLocked(const VideoScanResult& video, const ImageScanResult& images);
    void applyImageScanLocked(const ImageScanResult& result);
    void activateItemsLocked(std::vector<std::string> items,
                             RotationClock::time_point now);
    void stopPlaybackLocked();

    void loadItemLocked(const std::string& path, RotationClock::time_point now);
    void applyHoldDecisionLocked(RotationClock::time_point now);
    float playerPositionLocked();
    void applySettingsToPlayerLocked();
    void ensurePlayerStartedLocked();
    void pollPlayerErrorLocked();
    void saveResumeLocked();
    void persistLocked();

    mutable std::mutex mu_;
    mutable std::mutex playerMu_;
    std::unique_ptr<PlayerBackend> player_;
    std::unique_ptr<WallpaperHost> host_;
    bool hostStarted_ = false;      // 真实宿主线程已建立
    bool playerRunning_ = false;    // mpv 进程已启动（stopPlayback 后置 false）
    bool testPlayerInjected_ = false;  // 注入测试播放器 = 无桌面装配

    VideoScanner videoScanner_;
    ImageScanner imageScanner_;
    Clock clock_;
    std::string settingsPath_;

    WallpaperSettings settings_;
    WallpaperSnapshot snapshot_;
    RotationController rotation_;
    HoldArbiter arbiter_;

    bool currentItemImage_ = false;  // 仅 mu_ 内读写；当前轮换项为图片（见 snapshot 注释）

    bool started_ = false;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> scanRequested_{false};
    std::atomic<std::uint64_t> scanGeneration_{0};

    std::mutex scanWaitMutex_;
    std::condition_variable scanWaitCv_;
    std::thread scanThread_;

    std::mutex tickWaitMutex_;
    std::condition_variable tickWaitCv_;
    std::thread tickThread_;
};

} // namespace module::wallpaper