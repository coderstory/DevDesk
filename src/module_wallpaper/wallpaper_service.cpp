#include "wallpaper_service.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <filesystem>
#include <random>
#include <system_error>
#include <utility>

#include "core/logging.h"
#include "module_wallpaper/core/availability.h"
#include "module_wallpaper/core/folder_policy.h"
#include "module_wallpaper/core/presentation.h"
#include "module_wallpaper/platform/mpv_player.h"

namespace module::wallpaper {

namespace {
// 契约：与 scanImageLibrary 的扩展名白名单同口径（大小写不敏感）。
// 轮换池条目本身来自两类扫描器，此判定只用于装载时区分「静图 / 视频」语义。
bool isImageFilePath(const std::string& path) {
    std::string extension = std::filesystem::path(path).extension().string();
    if (!extension.empty() && extension.front() == '.') extension.erase(extension.begin());
    for (char& ch : extension) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return extension == "jpg" || extension == "jpeg" || extension == "png" ||
           extension == "webp" || extension == "heic";
}
} // namespace

WallpaperService::WallpaperService()
    : player_(std::make_unique<NullPlayerBackend>()),
      host_(createWallpaperHost()),
      settingsPath_(wallpaperSettingsPath().string()),
      // 随机模式按当前时间 + random_device 播种：默认种子会让每次启动的「随机」顺序完全相同。
      rotation_(static_cast<std::uint32_t>(
          std::random_device{}() ^
          static_cast<unsigned>(
              std::chrono::steady_clock::now().time_since_epoch().count()))) {}

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
        snapshot_.playbackError.clear();
        hostStarted_ = false;
        playerRunning_ = false;
        // 生产装配：真实播放器 + WorkerW 宿主；测试注入播放器时不碰桌面。
        if (!testPlayerInjected_) {
            std::lock_guard<std::mutex> playerLock(playerMu_);
            player_ = createMpvPlayer();
        }
        applySettingsToPlayerLocked();
        if (!testPlayerInjected_) {
            std::string error;
            if (host_ != nullptr && host_->start(&error)) {
                hostStarted_ = true;
            } else {
                snapshot_.playbackError =
                    error.empty() ? "壁纸宿主启动失败" : error;
                LOG_WARN("壁纸: " + snapshot_.playbackError);
            }
            // 契约 §9.1：宿主就绪 → mpv 以 videoChild 为 --wid 启动（尚不装载）。
            if (hostStarted_) ensurePlayerStartedLocked();
        }
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
        // 线程没起来时不能让快照永远停在「扫描中」；requestScanLocked 也会按 joinable 兜底。
        std::lock_guard<std::mutex> lock(mu_);
        snapshot_.scanning = false;
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
        // 固定顺序（契约 §9.6）：保存续播 → player.stop → host.stop → join 服务线程。
        if (started_) saveResumeLocked();
        started_ = false;
        stopRequested_.store(true);
        scanRequested_.store(false);
        // 使在飞扫描结果失效：否则它可能在 host.stop 之后才把结果 apply 回来。
        scanGeneration_.fetch_add(1, std::memory_order_relaxed);
        stopPlaybackLocked();
        // hostStarted_ 与 started_ 同一纪律：只在 mu_ 内写——在飞 tick/scan 持
        // mu_ 读此位，锁外写是数据竞争（审计 F-2）。
        hostStarted_ = false;
    }
    if (host_ != nullptr) host_->stop();
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
    out.poolCount = static_cast<int>(rotation_.itemCount());
    out.currentItemImage = currentItemImage_;

    presentation::StatusInput status;
    status.kind = out.kind;
    status.availability = out.availability;
    status.mode = out.mode;
    status.paused = out.paused;
    status.manualPaused = out.manualPaused;
    status.systemHolds = out.systemHolds;
    status.itemCount = static_cast<int>(rotation_.itemCount());
    status.rotationIntervalSec = out.rotationIntervalSec;
    status.imageItemCount = snapshot_.poolImageCount;
    status.currentItemImage = out.currentItemImage;
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
    // 只有真实可播时才推进：扫描中/空态下 rotation 仍可能残留上一轮清单，
    // 不挡住会装载不属于当前来源的旧条目。
    if (!started_ || snapshot_.availability != Availability::Playing) return;
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
    testPlayerInjected_ = true;
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
    if (!scanThread_.joinable()) {
        // 扫描线程创建失败（极少数系统错误）：不能把快照永远停在「扫描中」。
        scanGeneration_.fetch_add(1, std::memory_order_relaxed);
        scanRequested_.store(false);
        snapshot_.scanning = false;
        return;
    }
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

        try {
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
                // 混合池：同目录图片与视频平等入池（空视频纯图片目录也可轮换）。
                ImageScanResult imageResult;
                if (imageScanner_) {
                    imageResult = imageScanner_(folder, minPixels);
                } else {
                    imageResult = scanImageLibrary(folder, minPixels, kLibraryEntryCap,
                                                   shouldCancel);
                }
                if (stopRequested_.load() || scanGeneration_.load() != generation) continue;
                std::lock_guard<std::mutex> lock(mu_);
                if (scanGeneration_.load() != generation) continue;
                applyVideoScanLocked(result, imageResult);
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
        } catch (const std::exception& error) {
            // 单次扫描异常不能让工作线程 terminate（注入扫描器/内存分配都可能抛）。
            LOG_ERROR(std::string("壁纸服务: 扫描线程异常: ") + error.what());
            std::lock_guard<std::mutex> lock(mu_);
            if (!scanRequested_.load()) snapshot_.scanning = false;
        } catch (...) {
            LOG_ERROR("壁纸服务: 扫描线程未知异常");
            std::lock_guard<std::mutex> lock(mu_);
            if (!scanRequested_.load()) snapshot_.scanning = false;
        }
    }
}

void WallpaperService::tickLoop() {
    std::unique_lock<std::mutex> waitLock(tickWaitMutex_);
    while (!stopRequested_.load()) {
        tickWaitCv_.wait_for(waitLock, std::chrono::milliseconds(250));
        if (stopRequested_.load()) break;

        try {
            std::lock_guard<std::mutex> lock(mu_);
            if (!started_) continue;
            pollPlayerErrorLocked();
            if (!rotation_.isRunning() || rotation_.isPaused()) continue;
            const RotationClock::time_point now = nowLocked();
            const std::optional<std::string> path = rotation_.tick(now);
            if (path.has_value()) loadItemLocked(*path, now);
        } catch (const std::exception& error) {
            LOG_ERROR(std::string("壁纸服务: 计时线程异常: ") + error.what());
        } catch (...) {
            LOG_ERROR("壁纸服务: 计时线程未知异常");
        }
    }
}

void WallpaperService::applyVideoScanLocked(const VideoScanResult& video,
                                            const ImageScanResult& images) {
    snapshot_.videoCount = static_cast<int>(video.files.size());
    snapshot_.poolImageCount = static_cast<int>(images.files.size());
    snapshot_.scanning = false;
    snapshot_.availability =
        evaluateAvailability(true, video.status,
                             snapshot_.videoCount + snapshot_.poolImageCount);
    if (snapshot_.availability != Availability::Playing) {
        stopPlaybackLocked();
        return;
    }
    if (hostStarted_ && host_ != nullptr) {
        std::string error;
        if (!host_->show(&error)) {
            snapshot_.playbackError = error.empty() ? "显示壁纸宿主失败" : error;
            LOG_WARN("壁纸: " + snapshot_.playbackError);
        }
    }
    // 池内视频与图片平等参与：合并后全局字典序，与两类扫描器各自的排序口径一致。
    std::vector<std::string> pool = video.files;
    pool.insert(pool.end(), images.files.begin(), images.files.end());
    std::sort(pool.begin(), pool.end());
    activateItemsLocked(std::move(pool), nowLocked());
}

void WallpaperService::applyImageScanLocked(const ImageScanResult& result) {
    snapshot_.imageTotal = result.total;
    snapshot_.imagePassing = result.passing;
    snapshot_.imageFiltered = result.filtered;
    snapshot_.poolImageCount = result.total;
    snapshot_.scanning = false;
    snapshot_.availability = evaluateAvailability(true, result.status, result.passing);
    if (snapshot_.availability != Availability::Playing) {
        stopPlaybackLocked();
        return;
    }
    // 图片来源真播放：宿主显示与视频来源同路径（启动即 hold 时等扫描结果再显示）。
    if (hostStarted_ && host_ != nullptr) {
        std::string error;
        if (!host_->show(&error)) {
            snapshot_.playbackError = error.empty() ? "显示壁纸宿主失败" : error;
            LOG_WARN("壁纸: " + snapshot_.playbackError);
        }
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
    // 必须清空清单：只停运行态的话，next()/旧命令仍能把上一来源/已删除的条目装载回来。
    rotation_.setItems({});
    currentItemImage_ = false;
    snapshot_.currentItemImage = false;
    arbiter_.invalidateResumeAnchor();
    {
        std::lock_guard<std::mutex> playerLock(playerMu_);
        if (player_) player_->stop();
    }
    playerRunning_ = false;
    if (hostStarted_ && host_ != nullptr) host_->hide();
}

void WallpaperService::loadItemLocked(const std::string& path,
                                      RotationClock::time_point now) {
    // 换片后旧锚点失效；先置暂停态再装载，避免真播放器装载后闪现播放。
    arbiter_.invalidateResumeAnchor();
    const bool held = arbiter_.held();
    // 契约：图片项同样走 loadfile（spawn 已全局 --image-display-duration=inf，
    // 常驻由 mpv 保证，换片只由轮换驱动）；position/seek/续播仅对视频项有意义。
    const bool imageItem = isImageFilePath(path);
    currentItemImage_ = imageItem;
    // UI 图片态只对 Video 来源（混合池）置位；Image 来源沿用单张/轮播既有语义。
    snapshot_.currentItemImage = imageItem && settings_.kind == WallpaperKind::Video;
    ensurePlayerStartedLocked();
    // 单循环续播：启动后首次装载的条目若与上次退出时相同，seek 回原位置（仅视频）。
    const bool resume = !imageItem && settings_.kind == WallpaperKind::Video &&
                        settings_.mode == PlayMode::LoopSingle &&
                        !settings_.lastPlayedPath.empty() &&
                        settings_.lastPlayedPath == path &&
                        settings_.lastPlayedPosition > 0.0;
    {
        std::lock_guard<std::mutex> playerLock(playerMu_);
        if (player_) {
            player_->pause(held);
            player_->load(path);
            if (resume) {
                player_->seek(static_cast<float>(settings_.lastPlayedPosition));
                settings_.lastPlayedPath.clear();
                settings_.lastPlayedPosition = 0.0;
            }
        }
    }
    if (held) rotation_.pause(now);
}

void WallpaperService::ensurePlayerStartedLocked() {
    if (playerRunning_ || !hostStarted_ || host_ == nullptr || player_ == nullptr) return;
    const HostHandle handle = host_->handle();
    if (handle.videoChild == nullptr) return;
    {
        std::lock_guard<std::mutex> playerLock(playerMu_);
        player_->start(reinterpret_cast<std::uintptr_t>(handle.videoChild));
    }
    playerRunning_ = true;
}

void WallpaperService::pollPlayerErrorLocked() {
    std::string error;
    {
        std::lock_guard<std::mutex> playerLock(playerMu_);
        if (player_ == nullptr || !player_->takeError(&error)) return;
    }
    snapshot_.playbackError = error;
    LOG_WARN("壁纸播放错误: " + error);
}

void WallpaperService::saveResumeLocked() {
    if (settings_.kind != WallpaperKind::Video) return;
    if (settings_.mode != PlayMode::LoopSingle) return;
    const std::optional<std::string> current = rotation_.currentPath();
    if (!current.has_value()) return;
    settings_.lastPlayedPath = *current;
    settings_.lastPlayedPosition = static_cast<double>(playerPositionLocked());
    persistLocked();
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
    // 图片项 position 恒 0 → 锚点无意义，seek 必须是安全 no-op。
    if (shouldSeek && !currentItemImage_) player_->seek(seekSeconds);
    player_->pause(false);
}

float WallpaperService::playerPositionLocked() {
    // 契约：图片态 position 查询恒 0 且不报错（mpv 静图无 time-pos 语义，不触碰播放器）。
    if (currentItemImage_) return 0.0f;
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