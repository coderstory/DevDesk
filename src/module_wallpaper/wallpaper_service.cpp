#include "wallpaper_service.h"

#include <algorithm>
#include <cmath>
#include <system_error>
#include <utility>

#include "core/logging.h"
#include "module_wallpaper/core/availability.h"
#include "module_wallpaper/core/folder_policy.h"
#include "module_wallpaper/core/presentation.h"

namespace module::wallpaper {

WallpaperService::WallpaperService()
    : player_(std::make_unique<NullPlayerBackend>()),
      settingsPath_(wallpaperSettingsPath().string()) {}

WallpaperService::~WallpaperService() {
    stop();
}

RotationClock::time_point WallpaperService::nowLocked() const {
    return clock_ ? clock_() : RotationClock::now();
}

std::string WallpaperService::currentFolderLocked() const {
    return folderForKind(settings_, settings_.kind);
}

void WallpaperService::start() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (started_) return;
        stopRequested_.store(false);
        scanRequested_.store(false);
        settings_ = sanitizeWallpaperSettings(loadWallpaperSettings(settingsPath_));

        snapshot_.kind = settings_.kind;
        snapshot_.mode = settings_.mode;
        snapshot_.rotationIntervalSec = settings_.rotationIntervalSec;
        snapshot_.rate = settings_.rate;
        snapshot_.volume = settings_.volume;
        snapshot_.muted = settings_.muted;
        snapshot_.pauseOnBattery = settings_.pauseOnBattery;
        snapshot_.imageMinPixels = settings_.imageMinPixels;
        snapshot_.imageFit = settings_.imageFit;
        snapshot_.folderPath = currentFolderLocked();
        snapshot_.paused = false;
        snapshot_.manualPaused = false;
        snapshot_.systemHolds = 0;
        snapshot_.scanning = false;
        // 重新启用模块 = 从干净运行时态开始：计数/让路锚点不跨 start-stop 残留。
        snapshot_.availability = Availability::FolderUnconfigured;
        snapshot_.videoCount = 0;
        snapshot_.imageTotal = 0;
        snapshot_.imagePassing = 0;
        snapshot_.imageFiltered = 0;
        arbiter_ = HoldArbiter{};

        rotation_.setMode(settings_.mode);
        rotation_.setIntervalSec(settings_.rotationIntervalSec, nowLocked());
        arbiter_.setPauseOnBattery(settings_.pauseOnBattery);
        applySettingsToPlayerLocked();
        started_ = true;
    }

    try {
        if (!tickThread_.joinable()) {
            tickThread_ = std::thread(&WallpaperService::tickLoop, this);
        }
    } catch (const std::system_error& error) {
        LOG_ERROR(std::string("壁纸服务: 计时线程创建失败: ") + error.what());
    }
    try {
        if (!scanThread_.joinable()) {
            scanThread_ = std::thread(&WallpaperService::scanLoop, this);
        }
    } catch (const std::system_error& error) {
        LOG_ERROR(std::string("壁纸服务: 扫描线程创建失败: ") + error.what());
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        requestScanLocked();
    }
    LOG_INFO("壁纸服务已启动");
}

void WallpaperService::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!started_ && !scanThread_.joinable() && !tickThread_.joinable()) return;
        started_ = false;
        stopRequested_.store(true);
        scanRequested_.store(false);
        stopPlaybackLocked();
    }
    scanWaitCv_.notify_all();
    tickWaitCv_.notify_all();
    if (scanThread_.joinable()) scanThread_.join();
    if (tickThread_.joinable()) tickThread_.join();
    LOG_INFO("壁纸服务已停止");
}

WallpaperSnapshot WallpaperService::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    WallpaperSnapshot out = snapshot_;
    const RotationClock::time_point now = nowLocked();

    out.kind = settings_.kind;
    out.folderPath = currentFolderLocked();
    out.mode = settings_.mode;
    out.rotationIntervalSec = settings_.rotationIntervalSec;
    out.rate = settings_.rate;
    out.volume = settings_.volume;
    out.muted = settings_.muted;
    out.pauseOnBattery = settings_.pauseOnBattery;
    out.imageMinPixels = settings_.imageMinPixels;
    out.imageFit = settings_.imageFit;

    out.paused = arbiter_.held();
    out.manualPaused = arbiter_.manuallyPaused();
    out.systemHolds = arbiter_.systemHolds() & kSystemHoldMask();
    out.secondsUntilRotation = rotation_.secondsUntilNextRotation(now);
    out.rotationProgress = rotation_.progress(now);

    presentation::StatusInput status;
    status.kind = out.kind;
    status.availability = out.availability;
    status.mode = out.mode;
    status.paused = out.paused;
    status.manualPaused = out.manualPaused;
    status.systemHolds = out.systemHolds;
    out.modeLabel = presentation::playModeLabel(out.mode, out.kind);
    out.statusTitle = presentation::statusTitle(status);
    out.statusSubtitle = presentation::statusSubtitle(status);
    out.countdownLabel = presentation::countdownLabel(
        out.kind, out.mode, out.paused, out.secondsUntilRotation);
    return out;
}

void WallpaperService::setKind(WallpaperKind kind) {
    std::lock_guard<std::mutex> lock(mu_);
    if (settings_.kind == kind) return;
    settings_.kind = kind;
    snapshot_.kind = kind;
    snapshot_.folderPath = currentFolderLocked();
    // 切换来源 = 新一轮：停当前播放与轮换，再按新目录重新扫描。
    stopPlaybackLocked();
    requestScanLocked();
    persistLocked();
}

void WallpaperService::setMode(PlayMode mode) {
    std::lock_guard<std::mutex> lock(mu_);
    settings_.mode = mode;
    snapshot_.mode = mode;
    rotation_.setMode(mode);
    persistLocked();
}

void WallpaperService::setRotationIntervalSec(int seconds) {
    std::lock_guard<std::mutex> lock(mu_);
    const int snapped = snapRotationIntervalSec(seconds);
    settings_.rotationIntervalSec = snapped;
    snapshot_.rotationIntervalSec = snapped;
    rotation_.setIntervalSec(snapped, nowLocked());
    persistLocked();
}

void WallpaperService::setRate(float rate) {
    std::lock_guard<std::mutex> lock(mu_);
    settings_.rate = rate;
    settings_ = sanitizeWallpaperSettings(settings_);
    snapshot_.rate = settings_.rate;
    {
        std::lock_guard<std::mutex> playerLock(playerMu_);
        if (player_) player_->setRate(settings_.rate);
    }
    persistLocked();
}

void WallpaperService::setVolume(float volume) {
    std::lock_guard<std::mutex> lock(mu_);
    settings_.volume = volume;
    settings_ = sanitizeWallpaperSettings(settings_);
    snapshot_.volume = settings_.volume;
    {
        std::lock_guard<std::mutex> playerLock(playerMu_);
        if (player_) player_->setVolume(settings_.volume);
    }
    persistLocked();
}

void WallpaperService::setMuted(bool muted) {
    std::lock_guard<std::mutex> lock(mu_);
    settings_.muted = muted;
    snapshot_.muted = muted;
    {
        std::lock_guard<std::mutex> playerLock(playerMu_);
        if (player_) player_->setMute(muted);
    }
    persistLocked();
}

void WallpaperService::setPauseOnBattery(bool pauseOnBattery) {
    std::lock_guard<std::mutex> lock(mu_);
    const bool changed = arbiter_.pauseOnBattery() != pauseOnBattery;
    settings_.pauseOnBattery = pauseOnBattery;
    snapshot_.pauseOnBattery = pauseOnBattery;
    arbiter_.setPauseOnBattery(pauseOnBattery);
    if (changed) applyHoldDecisionLocked(nowLocked());
    persistLocked();
}

void WallpaperService::setImageMinPixels(int minPixels) {
    std::lock_guard<std::mutex> lock(mu_);
    settings_.imageMinPixels = minPixels;
    settings_ = sanitizeWallpaperSettings(settings_);
    snapshot_.imageMinPixels = settings_.imageMinPixels;
    if (settings_.kind == WallpaperKind::Image) requestScanLocked();
    persistLocked();
}

void WallpaperService::setImageFit(ImageFit fit) {
    std::lock_guard<std::mutex> lock(mu_);
    settings_.imageFit = fit;
    snapshot_.imageFit = fit;
    persistLocked();
}

bool WallpaperService::selectFolder(const std::string& path, std::string* error) {
    const std::string normalized = folder_policy::normalizeFolderPath(path);
    if (normalized.empty()) {
        if (error != nullptr) *error = "目录路径为空";
        return false;
    }
    if (!folder_policy::isAcceptableFolder(normalized)) {
        if (error != nullptr) *error = "目录不存在或不是文件夹: " + normalized;
        return false;
    }

    std::lock_guard<std::mutex> lock(mu_);
    if (settings_.kind == WallpaperKind::Image) {
        settings_.imageFolderPath = normalized;
    } else {
        settings_.sourceFolder = normalized;
    }
    snapshot_.folderPath = normalized;
    requestScanLocked();
    persistLocked();
    return true;
}

void WallpaperService::rescan() {
    std::lock_guard<std::mutex> lock(mu_);
    requestScanLocked();
}

void WallpaperService::toggleManualPause() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!started_) return;
    const float position = playerPositionLocked();
    const bool next = !arbiter_.manuallyPaused();
    if (!arbiter_.setManualPause(next, position)) return;
    applyHoldDecisionLocked(nowLocked());
}

void WallpaperService::next() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!started_) return;
    const RotationClock::time_point now = nowLocked();
    const std::optional<std::string> path = rotation_.advanceNow(now);
    if (path.has_value()) loadItemLocked(*path, now);
}

// ---------------- 测试缝 ----------------

void WallpaperService::setVideoScannerForTest(VideoScanner scanner) {
    std::lock_guard<std::mutex> lock(mu_);
    videoScanner_ = std::move(scanner);
}

void WallpaperService::setImageScannerForTest(ImageScanner scanner) {
    std::lock_guard<std::mutex> lock(mu_);
    imageScanner_ = std::move(scanner);
}

void WallpaperService::setPlayerForTest(std::unique_ptr<PlayerBackend> player) {
    std::lock_guard<std::mutex> lock(mu_);
    std::lock_guard<std::mutex> playerLock(playerMu_);
    player_ = std::move(player);
    if (player_ == nullptr) return;
    player_->setRate(settings_.rate);
    player_->setVolume(settings_.volume);
    player_->setMute(settings_.muted);
    player_->pause(arbiter_.held());
}

void WallpaperService::setClockForTest(Clock clock) {
    std::lock_guard<std::mutex> lock(mu_);
    clock_ = std::move(clock);
}

void WallpaperService::setSettingsPathForTest(std::string path) {
    std::lock_guard<std::mutex> lock(mu_);
    settingsPath_ = std::move(path);
}

void WallpaperService::setSystemHoldForTest(HoldReason reason, bool active) {
    if (reason == HoldReason::ManualPause) return;  // 手动暂停走 toggleManualPause
    std::lock_guard<std::mutex> lock(mu_);
    const float position = playerPositionLocked();
    if (!arbiter_.setSystemReason(reason, active, position)) return;
    applyHoldDecisionLocked(nowLocked());
}

// ---------------- 内部实现 ----------------

void WallpaperService::requestScanLocked() {
    if (!started_) return;
    scanGeneration_.fetch_add(1, std::memory_order_relaxed);
    const std::string folder = currentFolderLocked();
    if (folder.empty()) {
        scanRequested_.store(false);
        snapshot_.scanning = false;
        snapshot_.availability = Availability::FolderUnconfigured;
        stopPlaybackLocked();
        return;
    }
    scanRequested_.store(true);
    snapshot_.scanning = true;
    scanWaitCv_.notify_one();
}

void WallpaperService::scanLoop() {
    std::unique_lock<std::mutex> waitLock(scanWaitMutex_);
    while (!stopRequested_.load()) {
        scanWaitCv_.wait_for(waitLock, std::chrono::milliseconds(100), [this] {
            return stopRequested_.load() || scanRequested_.load();
        });
        if (stopRequested_.load()) break;
        if (!scanRequested_.load()) continue;

        std::string folder;
        WallpaperKind kind = WallpaperKind::Video;
        int minPixels = 3686400;
        std::uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!scanRequested_.exchange(false)) continue;
            generation = scanGeneration_.load();
            kind = settings_.kind;
            folder = currentFolderLocked();
            minPixels = settings_.imageMinPixels;
            if (folder.empty()) {
                snapshot_.scanning = false;
                snapshot_.availability = Availability::FolderUnconfigured;
                stopPlaybackLocked();
                continue;
            }
        }

        const auto shouldCancel = [this, generation] {
            return stopRequested_.load() || scanGeneration_.load() != generation;
        };

        if (kind == WallpaperKind::Video) {
            VideoScanResult result;
            if (videoScanner_) {
                result = videoScanner_(folder);
            } else {
                result = scanVideoLibrary(folder, kLibraryEntryCap, shouldCancel);
            }
            if (stopRequested_.load() || scanGeneration_.load() != generation) continue;
            std::lock_guard<std::mutex> lock(mu_);
            if (scanGeneration_.load() != generation) continue;
            applyVideoScanLocked(result);
        } else {
            ImageScanResult result;
            if (imageScanner_) {
                result = imageScanner_(folder, minPixels);
            } else {
                result = scanImageLibrary(folder, minPixels, kLibraryEntryCap, shouldCancel);
            }
            if (stopRequested_.load() || scanGeneration_.load() != generation) continue;
            std::lock_guard<std::mutex> lock(mu_);
            if (scanGeneration_.load() != generation) continue;
            applyImageScanLocked(result);
        }
    }
}

void WallpaperService::tickLoop() {
    std::unique_lock<std::mutex> waitLock(tickWaitMutex_);
    while (!stopRequested_.load()) {
        tickWaitCv_.wait_for(waitLock, std::chrono::milliseconds(250));
        if (stopRequested_.load()) break;

        std::lock_guard<std::mutex> lock(mu_);
        if (!started_) continue;
        if (!rotation_.isRunning() || rotation_.isPaused()) continue;
        const RotationClock::time_point now = nowLocked();
        const std::optional<std::string> path = rotation_.tick(now);
        if (path.has_value()) loadItemLocked(*path, now);
    }
}

void WallpaperService::applyVideoScanLocked(const VideoScanResult& result) {
    snapshot_.videoCount = static_cast<int>(result.files.size());
    snapshot_.scanning = false;
    snapshot_.availability =
        evaluateAvailability(true, result.status, snapshot_.videoCount);
    if (snapshot_.availability != Availability::Playing) {
        stopPlaybackLocked();
        return;
    }
    activateItemsLocked(result.files, nowLocked());
}

void WallpaperService::applyImageScanLocked(const ImageScanResult& result) {
    snapshot_.imageTotal = result.total;
    snapshot_.imagePassing = result.passing;
    snapshot_.imageFiltered = result.filtered;
    snapshot_.scanning = false;
    snapshot_.availability = evaluateAvailability(true, result.status, result.passing);
    if (snapshot_.availability != Availability::Playing) {
        stopPlaybackLocked();
        return;
    }
    activateItemsLocked(result.files, nowLocked());
}

void WallpaperService::activateItemsLocked(std::vector<std::string> items,
                                           RotationClock::time_point now) {
    if (!rotation_.isRunning()) {
        rotation_.setItems(std::move(items));
        const std::optional<std::string> first = rotation_.start(now);
        if (first.has_value()) {
            loadItemLocked(*first, now);
        } else {
            applyHoldDecisionLocked(now);
        }
        return;
    }
    if (!rotation_.refreshItems(std::move(items))) {
        const std::optional<std::string> first = rotation_.start(now);
        if (first.has_value()) loadItemLocked(*first, now);
        return;
    }
    // 重扫保留当前与剩余：只重新落一次暂停/恢复决策（暂停时冻结、解禁时恢复）。
    applyHoldDecisionLocked(now);
}

void WallpaperService::stopPlaybackLocked() {
    rotation_.stop();
    arbiter_.invalidateResumeAnchor();
    std::lock_guard<std::mutex> playerLock(playerMu_);
    if (player_) player_->stop();
}

void WallpaperService::loadItemLocked(const std::string& path,
                                      RotationClock::time_point now) {
    // 换片后旧锚点失效；先置暂停态再装载，避免 P3 真播放器装载后闪现播放。
    arbiter_.invalidateResumeAnchor();
    const bool held = arbiter_.held();
    {
        std::lock_guard<std::mutex> playerLock(playerMu_);
        if (player_) {
            player_->pause(held);
            player_->load(path);
        }
    }
    if (held) rotation_.pause(now);
}

void WallpaperService::applyHoldDecisionLocked(RotationClock::time_point now) {
    if (arbiter_.held()) {
        rotation_.pause(now);
        std::lock_guard<std::mutex> playerLock(playerMu_);
        if (player_) player_->pause(true);
        return;
    }
    rotation_.resume(now);
    float seekSeconds = 0.0f;
    const bool shouldSeek = arbiter_.takeResumeSeek(&seekSeconds);
    std::lock_guard<std::mutex> playerLock(playerMu_);
    if (player_ == nullptr) return;
    if (shouldSeek) player_->seek(seekSeconds);
    player_->pause(false);
}

float WallpaperService::playerPositionLocked() {
    std::lock_guard<std::mutex> playerLock(playerMu_);
    if (player_ == nullptr) return 0.0f;
    return player_->positionSec();
}

void WallpaperService::applySettingsToPlayerLocked() {
    std::lock_guard<std::mutex> playerLock(playerMu_);
    if (player_ == nullptr) return;
    player_->setRate(settings_.rate);
    player_->setVolume(settings_.volume);
    player_->setMute(settings_.muted);
}

void WallpaperService::persistLocked() {
    const core::Result<void> result = saveWallpaperSettings(settingsPath_, settings_);
    if (result.isFail()) {
        LOG_WARN("壁纸设置保存失败: " + result.errorMessage());
    }
}

} // namespace module::wallpaper