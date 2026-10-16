// src/module_claude_proxy/claude_module.cpp
// Claude 配置切换器模块：Module 接口 + compose 骨架 + 事件回调接线 + 操作。
// 可视区域在 claude_ui_*.cpp 的自由函数里（状态显式入参、不捕获 this），本文件只管分区委派。
// 数据缓存：compose 只读内存快照，SQLite 查询全在后台线程，命中帧零 IO；写操作标脏缓存，下一帧重查。
#include "claude_service.h"
#include "claude_tools.h"
#include "claude_ui_detail.h"
#include "claude_ui_ccswitch.h"
#include "claude_ui_form.h"
#include "claude_ui_list.h"
#include "claude_ui_logic.h"
#include "claude_ui_logs.h"
#include "claude_ui_quota.h"
#include "claude_ui_state.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/router.h"   // appRouter（消费首页发来的「去配置」负载）
#include "core/theme.h"
#include "core/component_theme.h"
#include "core/database.h"
#include "core/event_bus.h"
#include "platform/json.h"
#include "provider_config.h"
#include "quota/quota_credential.h"
#include "quota/quota_registry.h"
#include "quota/quota_refresh.h"
#include "quota/quota_store.h"
#include "upstream_dialect.h"
#include <core/platform/platform.h>   // core::platform::requestUiUpdate（唤醒主循环）
#include <core/window/window_backend.h>
#include <ui_kit/metrics.h>
#include <ui_kit/toast.h>

#include <eui/dsl.h>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace module::claude {

class ClaudeModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::ClaudeProxy; }
    std::string displayName() const override { return "Claude 切换器"; }
    std::string shortName() const override { return "Claude"; }
    std::string iconGlyph() const override { return eui::utf8(0xF021); }  // fa-arrows-rotate

    ~ClaudeModule() override {
        // 必须 join 干净（留 joinable 即 std::terminate）。
        stopQuotaRefresh();
        if (testThread_.joinable()) testThread_.join();
    }

    // UI（骨架：分区委派，各区实现见 claude_ui_*.cpp）

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        // 消费跨模块跳转负载（取走即清，放最前保证同帧画出弹窗）
        consumeQuotaConfigPayload();
        const auto& t = core::theme::current();
        const auto themeTokens = core::theme::componentTokens(t);
        const float pad = t.sizes.spacingXXLarge;        // 40
        const float contentW = screen.width - pad * 2.0f;
        const float contentH = screen.height - pad * 2.0f;
        const std::time_t now = std::time(nullptr);   // 调用时间列「今天/昨天」判定
        const float headerH = ui_kit::kHeaderH;
        const float gap = t.sizes.spacingLarge;
        const float leftW = 400.0f;
        const float rightW = contentW - leftW - gap;

        // 读真实状态（ClaudeService 全局单例，由 main.cpp 注册）
        ClaudeService* svc = ClaudeService::global();
        const bool proxyRunning = svc ? svc->isProxyRunning() : false;
        const int runningPort = svc ? svc->proxyPort() : 0;
        const std::string portStr = runningPort > 0
            ? (":" + std::to_string(runningPort))
            : std::string("");   // 未运行时不显示假端口

        // 引导条：代理未运行且本会话从未启动过时显示（内存态一次性）
        if (proxyRunning) state_.list.proxyEverSeen_ = true;
        const bool showGuide = !proxyRunning && !state_.list.proxyEverSeen_;
        // 预留高必须与绘制侧一致；bodyH 在 compose 期间就要传给左右子面板。
        const float bodyH = contentH - headerH - t.sizes.spacingLarge -
                            (showGuide ? kGuideBarH + t.sizes.spacingLarge : 0.0f);

        // 页面不自铺不透明底，用主窗口 clearColor（半透/磨砂档才生效）

        // 错误态：加载失败不静默吞错显示假空态
        std::string profilesError;
        std::string callsError;

        // profiles/recent/active 走内存快照（脏标记触发重查，失败保持脏重试）
        std::vector<Profile> profiles;
        std::vector<ClaudeService::CallLogEntry> recent;
        Profile activeProfile;
        bool hasActive = false;
        if (!svc) {
            profilesError = "服务未就绪（ClaudeService 未注册）";
            callsError = "服务未就绪（ClaudeService 未注册）";
            state_.list.dataDirty_ = true;   // 服务未就绪查不了，保持脏等下帧
            // 防御分支：装配已落地，此处理论不可达
            if (!serviceNotReadyWarned_) {
                showToast("Claude 服务未装配",
                          "本会话内开启的模块需重启后才能提供代理服务", true);
                serviceNotReadyWarned_ = true;
            }
        } else {
            refreshDataCache(state_.list, svc);
            profiles = state_.list.cachedProfiles_;
            recent = state_.list.cachedRecent_;
            activeProfile = state_.list.cachedActive_;
            hasActive = state_.list.cachedHasActive_;
            profilesError = state_.list.cachedProfilesError_;
            callsError = state_.list.cachedCallsError_;
        }

        const Profile* shown = nullptr;
        for (const auto& p : profiles) {
            if (p.id == state_.list.selectedId_) { shown = &p; break; }
        }
        if (!shown && hasActive) shown = &activeProfile;

        DetailView dv;   // 右栏只读视图（显式入参，不捕获 this）
        dv.svc = svc;
        dv.shown = shown;
        dv.nameValue = shown ? shown->name : std::string("(未启用)");
        dv.modelValue = shown ? modelsSummary(*shown) : std::string("-");
        dv.urlValue = shown ? shown->baseUrl : std::string("-");
        dv.tokenValue = shown
            ? (shown->apiKeyHint + " [本机库]")
            : std::string("(无激活 profile)");
        dv.recent = &recent;
        dv.callsError = callsError;
        dv.now = now;
        dv.sharedConfigs = &state_.list.cachedSharedConfigs_;
        // 高级配置 pane：生效方言 + 备用 Profile 名（从列表快照解析），pane 内零服务调用
        if (shown != nullptr) {
            const UpstreamDialect effective = UpstreamDialect::forBaseUrl(
                shown->baseUrl, shown->dialectOverride);
            const char* displayName = dialectKindDisplayName(effective.kind);
            if (dialectKindFromText(shown->dialectOverride)) {
                dv.dialectText = std::string(displayName) + "（显式）";
            } else {
                dv.dialectText =
                    std::string("自动（按站点推断）→ ") + displayName;
            }
            if (shown->fallbackProfileId == 0) {
                dv.fallbackName = "无";
            } else {
                dv.fallbackName = "（已删除）";
                for (const auto& p : profiles) {
                    if (p.id == shown->fallbackProfileId) {
                        dv.fallbackName = p.name;
                        break;
                    }
                }
            }
        } else {
            dv.dialectText = "-";
            dv.fallbackName = "-";
        }

        // 生效 JSON = profile ⊕ 片段（片段赢）；解析失败经 note 区分。
        if (shown != nullptr) {
            std::string fragmentJson;
            if (shown->sharedConfigId != 0) {
                const SharedConfig* sc = findSharedById(
                    state_.list.cachedSharedConfigs_, shown->sharedConfigId);
                if (sc != nullptr) fragmentJson = sc->extraSettingsJson;
            }
            // keys 空且 warnings 非空 = 配置存在但失效，显示「解析失败」而非「未配置」
            std::vector<std::string> effWarnings;
            const std::string effective = effectiveExtraSettingsJson(
                shown->extraSettingsJson, fragmentJson, &effWarnings);
            if (effective.empty() && !effWarnings.empty()) {
                dv.extraSettingsNote = "（JSON 解析失败，接管时将忽略）";
            }
            if (!effective.empty()) {
                auto parsed = platform::json::parse(effective);
                if (parsed.isOk() && parsed.value().isObject()) {
                    auto effObj = parsed.value().asObject();
                    std::vector<std::string> fragmentKeys;
                    if (!fragmentJson.empty()) {
                        auto fp = platform::json::parse(fragmentJson);
                        if (fp.isOk() && fp.value().isObject()) {
                            fragmentKeys = fp.value().asObject().keys();
                        }
                    }
                    for (const auto& k : effObj.keys()) {
                        const bool fromFragment =
                            std::find(fragmentKeys.begin(), fragmentKeys.end(),
                                      k) != fragmentKeys.end();
                        dv.extraSettingsKeys.emplace_back(
                            k, fromFragment
                                   ? envSourceLabel(EnvSource::Shared)
                                   : envSourceLabel(EnvSource::Profile));
                        if (k == "model") {
                            const auto mv = effObj.get(k);
                            if (mv.isString() && !mv.asString().empty()) {
                                dv.extraSettingsNote =
                                    "「model」覆盖模型槽位（ANTHROPIC_MODEL 不写入）";
                            }
                        }
                    }
                } else {
                    dv.extraSettingsNote =
                        "（JSON 解析失败，接管时将忽略）";
                }
            }
        }

        ui.column("claude.root")
            .size(screen.width, screen.height)
            .padding(pad)
            .gap(t.sizes.spacingLarge)
            .content([&] {
                drawClaudeHeader(ui, contentW, headerH, t, proxyRunning, portStr,
                                 [this] { doToggleProxy(); });

                if (showGuide) drawClaudeGuideBar(ui, contentW, t);

                ui.row("claude.body")
                    .size(contentW, bodyH)
                    .gap(gap)
                    .content([&] {
                        drawProfileListPanel(ui, leftW, bodyH, t, themeTokens,
                                             state_.list, profiles, profilesError,
                                             state_.list.cachedSharedConfigs_,
                                             state_.list.cachedSharedConfigsError_,
                                             listActions());
                        drawDetailPanel(ui, rightW, bodyH, leftW + gap, t,
                                        themeTokens, state_.detail, dv,
                                        detailActions());
                    })
                    .build();
            })
            .build();

        // 弹窗（画在最上层）
        drawProfileFormDialog(ui, screen, t, themeTokens, state_.form,
                              state_.import, state_.list.cachedSharedConfigs_,
                              state_.list.cachedProfiles_,
                              formHooks());
        drawDeleteConfirmDialog(ui, screen, t, themeTokens, state_.form,
                                [this](int64_t id) { doDelete(id); });
        drawCallsDialog(ui, screen, t, themeTokens, state_.logs, toastHook());
        // 共享配置片段编辑弹窗 + 删除确认；绘制序 = Esc 序链（见 closeTopmostDialog）
        drawSharedConfigDialog(ui, screen, t, themeTokens, state_.shared,
                               sharedHooks());
        drawSharedConfigDeleteConfirmDialog(
            ui, screen, t, themeTokens, state_.shared,
            [this](int64_t id, const std::string& name) {
                doDeleteShared(id, name);
            });
        // 高级设置 JSON 全屏覆盖层（显式 z=1050，盖表单、让 toast）
        drawJsonFullscreenOverlay(ui, screen, t, themeTokens, state_.form,
                                  formHooks(),
                                  state_.list.cachedSharedConfigs_);
        // 余量查询 / cc-switch 导入弹窗，绘制序即 Esc 序链最顶层
        drawQuotaConfigDialog(ui, screen, t, themeTokens, state_.quota,
                              quotaHooks());
        drawCcSwitchImportDialog(ui, screen, t, themeTokens,
                                 state_.ccswitchImport, ccswitchImportHooks());
        harvestQuotaTestResult();   // 后台结果回 UI（每帧轮询）
        harvestTestResult();
        drawToast(ui, screen, themeTokens);

        drawProfileContextMenu(ui, screen, themeTokens, state_.list, profiles,
                               listActions());

        LOG_DEBUG("Claude 模块 compose 完成");
    }

    void onStart() override {
        if (started_) return;   // 重入幂等
        started_ = true;
        LOG_INFO("Claude 模块启动");
        ClaudeService* svc = ClaudeService::global();
        if (!svc) {
            LOG_WARN("onStart: ClaudeService::global() 为 null");
            state_.list.dataDirty_ = true;   // 服务未就绪，保持脏
            return;
        }
        // 首次装载必须同步（onStart 非渲染路径）：compose 恒读快照，异步首刷会闪空帧
        applyDataRefresh(state_.list, runDataRefresh(svc, true, true));
        auto orphan = svc->cleanupOrphanKeychainEntries();
        if (orphan.isFail()) {
            LOG_WARN("onStart: 旧 Keychain 残留清理失败 - " + orphan.errorMessage());
        }
        // 代理默认不启动：由 main.cpp 按设置门控或用户手动启动
        if (svc->isProxyRunning()) {
            LOG_INFO("onStart: 代理已在运行 port=" +
                     std::to_string(svc->proxyPort()));
        } else {
            LOG_INFO("onStart: 代理未运行（默认不自动启动，等待用户手动启动）");
        }
        // 智能体工具注册（claude.* 四工具）；注册失败只记日志
        claudeTools_ = std::make_unique<ClaudeTools>(*svc);
        claudeTools_->registerAll();
        startQuotaRefresh();
        // GitHub Sync 导入覆盖活库期间暂停余量后台刷新（它 60s 一轮写
        // QuotaStore，同一 appDatabase）。end 无论成败都发 → 恢复由此触发；
        // startQuotaRefresh 建新 QuotaRefresher（stopAll 后旧实例不可重启）。
        // 订阅 token 必须持有，丢了就退订。handler 在导入线程同步执行。
        // emit 已把 handler 拷出后才调（退订不拦在途调用）→ handler 先查
        // 原子闸再动线程，否则关停瞬间的迟到事件会复活 QuotaRefresher。
        dbImportHandlersEnabled_.store(true, std::memory_order_release);
        ::core::event::EventBus& bus = ::core::event::bus();
        dbImportSubs_.push_back(bus.subscribe(
            ::core::event::kDbImportBegin, [this](const std::string&) {
                if (dbImportHandlersEnabled_.load(std::memory_order_acquire)) {
                    stopQuotaRefresh();
                }
            }));
        dbImportSubs_.push_back(bus.subscribe(
            ::core::event::kDbImportEnd, [this](const std::string&) {
                if (dbImportHandlersEnabled_.load(std::memory_order_acquire)) {
                    startQuotaRefresh();
                }
            }));
    }

    // 进页标记缓存脏，下一帧 compose 重查
    void onPageEnter() override { state_.list.dataDirty_ = true; }

    void onStop() override {
        if (!started_) return;   // 重入幂等
        LOG_INFO("Claude 模块停止");
        // 先关原子闸再退订再停余量刷新：闸挡住在途 handler，退订挡住后续 emit。
        dbImportHandlersEnabled_.store(false, std::memory_order_release);
        dbImportSubs_.clear();
        // 先停余量刷新（线程里的 QuotaStore 引用会在成员析构后悬垂）；再注销工具、最后停代理。
        stopQuotaRefresh();
        if (claudeTools_) {
            claudeTools_->unregisterAll();
            claudeTools_.reset();
        }
        ClaudeService* svc = ClaudeService::global();
        if (svc && svc->isProxyRunning()) {
            svc->stopProxy();
            LOG_INFO("onStop: 代理已停止");
        }
        started_ = false;
    }

    // 页级快捷键：Ctrl+N 新建 / Ctrl+S 保存表单 / Ctrl+Enter 启停代理。
    // 弹窗打开时 Ctrl+N / Ctrl+Enter 不响应。返回 false = 未消费。
    bool onKeyEvent(const eui::KeyEvent& key) override {
        if (key.action != eui::KeyAction::Press) {
            return false;
        }
        // Esc 关最上层弹窗；焦点元素（表单输入框）聚焦时 Esc 已被 input 收走
        if (key.key == eui::InputKey::Escape) {
            return closeTopmostDialog();
        }
        if (!key.modifiers.shortcut() || key.modifiers.alt) {
            return false;
        }
        const bool anyDialogOpen =
            state_.form.formOpen_ || state_.form.deleteConfirmOpen_ ||
            state_.logs.callsDialogOpen_ ||
            state_.shared.open_ || state_.shared.deleteConfirmOpen_ ||
            state_.quota.open || state_.ccswitchImport.open_;
        if (key.key == eui::InputKey::N) {
            if (anyDialogOpen) return false;
            openCreateForm(state_.form, state_.import);
            LOG_INFO("快捷键 Ctrl+N：打开新建 Profile 表单");
            return true;
        }
        if (key.key == eui::InputKey::S) {
            if (!state_.form.formOpen_) return false;
            submitForm(state_.form, formHooks(),
                       state_.list.cachedSharedConfigs_);
            return true;
        }
        if (key.key == eui::InputKey::Enter) {
            if (anyDialogOpen) return false;
            doToggleProxy();
            return true;
        }
        return false;
    }

private:
    // Esc 关最上层弹窗（顺序 = 绘制序倒序）。全屏覆盖层最优先：Esc = 放弃修改
    //（恢复落库值 + 退全屏），第二次才关表单
    bool closeTopmostDialog() {
        if (state_.form.formJsonFullscreen_ && state_.form.formOpen_) {
            discardJsonFullscreenEdits(state_.form);
            return true;
        }
        if (state_.quota.open) { state_.quota.open = false; return true; }
        if (state_.ccswitchImport.open_) { state_.ccswitchImport.open_ = false; return true; }
        if (state_.shared.deleteConfirmOpen_) { state_.shared.deleteConfirmOpen_ = false; return true; }
        if (state_.shared.open_) { state_.shared.open_ = false; return true; }
        if (state_.logs.callsDialogOpen_) { state_.logs.callsDialogOpen_ = false; return true; }
        if (state_.form.deleteConfirmOpen_) { state_.form.deleteConfirmOpen_ = false; return true; }
        if (state_.form.formOpen_) { state_.form.formOpen_ = false; return true; }
        return false;
    }

    // 每帧接线装配：回调显式绑到本模块操作/状态
    ProfileListActions listActions() {
        ProfileListActions a;
        a.onNewProfile = [this] { openCreateForm(state_.form, state_.import); };
        a.onImportCurrent = [this] {
            startImportCurrentConfig(state_.form, state_.import, formHooks());
        };
        a.onImportCcSwitch = [this] {
            openCcSwitchImportDialog(state_.ccswitchImport);
        };
        a.requestEdit = [this](const Profile& p) {
            openEditForm(state_.form, state_.import, p,
                         state_.list.cachedSharedConfigs_);
        };
        a.requestDuplicate = [this](int64_t id) { doDuplicate(id); };
        a.requestCopyConfig = [this](const Profile& p) { doCopyConfig(p); };
        a.requestDelete = [this](int64_t id, const std::string& name) {
            state_.form.deleteTargetId_ = id;
            state_.form.deleteTargetName_ = name;
            state_.form.deleteConfirmOpen_ = true;
        };
        a.requestSetActive = [this](int64_t id) { doSetActive(id); };
        a.onNewShared = [this] { openSharedCreate(state_.shared); };
        a.requestEditShared = [this](const SharedConfig& c) {
            openSharedEdit(state_.shared, c);
        };
        return a;
    }

    std::function<void(const std::string&, const std::string&, bool)> toastHook() {
        return [this](const std::string& title, const std::string& msg,
                      bool isError) { showToast(title, msg, isError); };
    }

    FormSubmitHooks formHooks() {
        FormSubmitHooks h;
        h.toast = toastHook();
        h.markDataDirty = [this] { state_.list.dataDirty_ = true; };
        h.selectProfile = [this](int64_t id) { state_.list.selectedId_ = id; };
        return h;
    }

    CcSwitchImportDialogHooks ccswitchImportHooks() {
        CcSwitchImportDialogHooks h;
        h.toast = toastHook();
        h.markDataDirty = [this] { state_.list.dataDirty_ = true; };
        return h;
    }

    SharedConfigSubmitHooks sharedHooks() {
        SharedConfigSubmitHooks h;
        h.toast = toastHook();
        h.markDataDirty = [this] { state_.list.dataDirty_ = true; };
        return h;
    }

    DetailActions detailActions() {
        DetailActions a;
        a.warn = [this](const std::string& title, const std::string& msg) {
            showToast(title, msg, true);
        };
        a.enable = [this](int64_t id) { doSetActive(id); };
        a.test = [this](const Profile& p) { doTestConnection(p); };
        a.edit = [this](const Profile& p) {
            openEditForm(state_.form, state_.import, p,
                         state_.list.cachedSharedConfigs_);
        };
        a.run = [this](int64_t id) { doRun(id); };
        a.quota = [this](int64_t id) { openQuotaDialog(id); };
        a.openCalls = [this] { openCallsDialog(state_.logs); };
        a.setOneM = [this](int64_t id, std::size_t slotIdx, bool on) {
            doSetOneM(id, slotIdx, on);
        };
        return a;
    }

    // Toast 走 ui_kit（成功图标 fa-circle-check）
    void drawToast(eui::Ui& ui, const eui::Screen& screen,
                   const components::theme::ThemeColorTokens& themeTokens) {
        ui_kit::drawToast(ui, screen, "claude.toast", toast_, themeTokens,
                          ui_kit::ToastOptions{.okIcon = 0xF058});
    }

    void showToast(const std::string& title, const std::string& msg, bool isError) {
        toast_.show(title, msg, isError);
    }

    // 手动启动 = 前置校验（联网）+ bind + 接管。只有联网校验进后台线程：
    // bind + syncTakeOver 必须回 UI 线程（settings_takeover 契约）。
    void doToggleProxy() {
        ClaudeService* svc = ClaudeService::global();
        if (!svc) {
            showToast("代理操作失败", "服务未就绪（ClaudeService 未注册）", true);
            return;
        }
        // 排在「运行中」判断之前：收割可能刚起代理，反序会误起第二条校验线程
        harvestTestResult();
        if (svc->isProxyRunning()) {
            svc->stopProxy();
            showToast("代理已停止", "本地代理已停止监听", false);
            return;
        }
        if (testThread_.joinable()) {
            showToast("操作进行中", "上一次校验/测试仍在进行，请稍候", false);
            return;
        }
        showToast("正在校验",
                  "检测激活 Profile 与上游连通性（最长 " +
                      std::to_string(ClaudeService::kProxyPreStartProbeTimeoutMs / 1000 + 1) +
                      " 秒）…", false);
        testThread_ = std::thread([this, svc] {
            // 线程里只跑探测；startProxy 由 harvestTestResult 在 UI 线程里调
            auto v = svc->validateProxyPreconditions();
            testResultStartProxy_ = v.isOk();
            if (v.isFail()) {
                testResultTitle_ = "启动代理失败";
                testResultMsg_ = v.errorMessage();
                testResultIsError_ = true;
            }
            testDone_.store(true, std::memory_order_release);
            core::platform::requestUiUpdate();   // 状态落好后再唤醒主循环
        });
    }

    void doSetActive(int64_t id) {
        ClaudeService* svc = ClaudeService::global();
        if (!svc) return;
        state_.list.dataDirty_ = true;   // 写操作标脏
        auto r = svc->setActiveProfile(id);
        if (r.isFail()) {
            showToast("启用失败", r.errorMessage(), true);
            return;
        }
        state_.list.selectedId_ = id;
        showToast("已启用", "该 Profile 已设为激活（ACTIVE）", false);
    }

    // 「1M」开关落库（models_config 里「基础名 + 可选 [1M]」字符串）；归一与表单保存同口径
    void doSetOneM(int64_t id, std::size_t slotIdx, bool on) {
        ClaudeService* svc = ClaudeService::global();
        if (svc == nullptr) return;
        const auto table = modelSlotTable();
        if (id == 0 || slotIdx >= table.size()) return;   // 防御：无选中 / 越界
        state_.list.dataDirty_ = true;
        // 每次重读：同一帧连点两行时不拿陈旧快照互相覆盖
        auto got = svc->getProfile(id);
        if (got.isFail()) {
            showToast("设置失败", got.errorMessage(), true);
            return;
        }
        Profile p = std::move(got).value();
        ModelsConfig cfg = effectiveModelsConfig(p.modelsConfigJson);
        ModelSlot& slot = cfg.*table[slotIdx].slot;
        const std::string before = slot.model;
        slot.model = setOneMMarker(slot.model, on);
        if (slot.model == before) return;   // 幂等：无变化不写库
        normalizeOneMMarkers(cfg);
        auto text = serializeModelsConfig(cfg);
        if (text.isFail()) {
            showToast("设置失败", text.errorMessage(), true);
            return;
        }
        p.modelsConfigJson = text.value();
        for (const auto& m : table) {
            if (std::string(m.jsonKey) == "default") {
                p.model = (cfg.*m.slot).model;
                break;
            }
        }
        auto r = svc->updateProfile(p);
        if (r.isFail()) {
            showToast("设置失败", r.errorMessage(), true);
            return;
        }
        showToast("已更新",
                  std::string("「") + table[slotIdx].label + "」的 1M 上下文标记已" +
                      (on ? "开启" : "关闭"),
                  false);
    }

    // 直连：生成临时 settings + claude --settings 打开新会话（零全局改动）
    void doRun(int64_t id) {
        ClaudeService* svc = ClaudeService::global();
        if (svc == nullptr) {
            showToast("运行失败", "服务未就绪（ClaudeService 未注册）", true);
            return;
        }
        if (id == 0) {
            showToast("运行失败", "请先选择 Profile", true);
            return;
        }
        std::string hint;
        auto r = svc->runProfile(id, &hint);
        if (r.isFail()) {
            showToast("运行失败", r.errorMessage(), true);
            return;
        }
        const std::string head = hint.empty()
            ? std::string("已用该 Profile 直连打开 Claude Code 会话"
                          "（临时配置会话结束后自动删除，未改动任何全局配置）。")
            : (hint + std::string("。"));
        showToast("已启动", head, false);
        state_.list.dataDirty_ = true;   // 下帧重查列表
    }

    // 建副本但不激活
    void doDuplicate(int64_t id) {
        ClaudeService* svc = ClaudeService::global();
        if (!svc) return;
        state_.list.dataDirty_ = true;   // 写操作标脏
        auto r = svc->duplicateProfile(id);
        if (r.isFail()) {
            showToast("建副本失败", r.errorMessage(), true);
            return;
        }
        showToast("已建副本",
                  "已创建副本「" + r.value().name + "」（未激活）", false);
    }

    // 当前 profile → settings.json 形态 JSON → 系统剪贴板。占位 = 脱敏 hint，
    // 不导明文 key；setClipboardText 无回读校验 → 固定 LOG_WARN 留痕。
    void doCopyConfig(const Profile& p) {
        ClaudeService* svc = ClaudeService::global();
        if (svc == nullptr || p.id == 0) {
            showToast("复制配置失败", "请先选择 Profile", true);
            return;
        }
        // 坏 JSON 防「静默丢键」：各域试解析，失败先 LOG_WARN 再继续导出
        if (!p.extraEnvJson.empty()) {
            auto ee = parseExtraEnv(p.extraEnvJson);
            if (ee.isFail()) {
                LOG_WARN("复制配置: extra_env 解析失败（profile id=" +
                         std::to_string(p.id) + "），导出将缺失该部分 env 键 - " +
                         ee.errorMessage());
            }
        }
        if (!p.extraSettingsJson.empty() &&
            validateExtraSettingsJson(p.extraSettingsJson).isFail()) {
            LOG_WARN("复制配置: 高级设置 JSON 解析失败（profile id=" +
                     std::to_string(p.id) + "），导出将缺失高级属性 - " +
                     validateExtraSettingsJson(p.extraSettingsJson).errorMessage());
        }
        // 片段 JSON 走列表/详情同源快照解析
        std::string fragmentJson;
        if (p.sharedConfigId != 0) {
            const SharedConfig* sc = findSharedById(
                state_.list.cachedSharedConfigs_, p.sharedConfigId);
            if (sc != nullptr) {
                fragmentJson = sc->extraSettingsJson;
                if (!fragmentJson.empty() &&
                    validateExtraSettingsJson(fragmentJson).isFail()) {
                    LOG_WARN("复制配置: 片段 JSON 解析失败（profile id=" +
                             std::to_string(p.id) + "），导出将缺失片段叠加 - " +
                             validateExtraSettingsJson(fragmentJson).errorMessage());
                }
            }
        }
        const std::string json = buildProfileExportJson(
            p, fragmentJson, /*keyPlaceholder=*/p.apiKeyHint);
        if (json.empty()) {
            showToast("复制配置失败", "配置序列化失败", true);
            return;
        }
        // 导出脱敏守卫：明文密钥出现在待写文本里即中止。
        if (svc != nullptr) {
            auto secret = svc->loadApiKey(p.id);
            if (secret.isOk() && !secret.value().empty() &&
                json.find(secret.value()) != std::string::npos) {
                LOG_ERROR("复制配置: 导出文本命中明文凭据，已中止（profile id=" +
                          std::to_string(p.id) + "）");
                showToast("复制配置失败", "导出内容含明文凭据，已中止", true);
                return;
            }
        }
        core::window::setClipboardText(json);
        // 无回读下的最低留痕：profile id + 导出键数
        size_t exportedKeys = 0;
        size_t exportedNonEnvKeys = 0;
        if (auto doc = platform::json::parse(json);
            doc.isOk() && doc.value().isObject()) {
            const auto rootObj = doc.value().asObject();
            const auto envVal = rootObj.get("env");
            if (envVal.isObject()) exportedKeys = envVal.asObject().keys().size();
            for (const auto& k : rootObj.keys()) {
                if (k != "env") ++exportedNonEnvKeys;
            }
        }
        LOG_WARN("复制配置: 已调用系统剪贴板写入（void API 无回读校验）" +
                 std::string("profile id=") + std::to_string(p.id) +
                 " env 键数=" + std::to_string(exportedKeys) +
                 " 高级属性键数=" + std::to_string(exportedNonEnvKeys));
        showToast("已复制配置", kCopyConfigToastSubtitle, false);
    }

    void doDelete(int64_t id) {
        ClaudeService* svc = ClaudeService::global();
        if (!svc) return;
        state_.list.dataDirty_ = true;
        bool proxyStopped = false;
        auto r = svc->deleteProfile(id, &proxyStopped);
        if (r.isFail()) {
            showToast("删除失败", r.errorMessage(), true);
            return;
        }
        if (state_.list.selectedId_ == id) state_.list.selectedId_ = 0;
        if (proxyStopped) {
            showToast("已删除（代理已停止）",
                      "删除的是激活 Profile，已停止代理并还原 Claude Code 配置",
                      false);
        } else {
            showToast("已删除", "Profile 已删除（含保存的 API Key）", false);
        }
    }

    // 删除共享配置片段：服务层同事务解绑，这里只做计数 toast + 关弹窗
    void doDeleteShared(int64_t id, const std::string& name) {
        ClaudeService* svc = ClaudeService::global();
        if (!svc) return;
        state_.list.dataDirty_ = true;
        size_t bound = 0;
        auto list = svc->listProfiles();
        if (list.isOk()) {
            for (const auto& p : list.value()) {
                if (p.sharedConfigId == id) ++bound;
            }
        }
        auto r = svc->deleteSharedConfig(id);
        if (r.isFail()) {
            showToast("删除失败", r.errorMessage(), true);
            return;
        }
        state_.shared.open_ = false;   // 编辑目标已删除，弹窗一并关闭
        if (bound > 0) {
            showToast("已删除",
                      "片段「" + name + "」已删除，" +
                          std::to_string(bound) + " 个 Profile 已解绑",
                      false);
        } else {
            showToast("已删除", "片段「" + name + "」已删除", false);
        }
    }

    // 起后台刷新（幂等）；依赖缺失 → 记日志 + 不装
    void startQuotaRefresh() {
        if (quotaRefresher_) return;
        auto* db = ::core::appDatabase();
        ClaudeService* svc = ClaudeService::global();
        if (db == nullptr || svc == nullptr) {
            LOG_WARN("startQuotaRefresh: appDatabase()/ClaudeService 缺位，余量刷新未启动");
            return;
        }
        quotaStore_ = std::make_unique<quota::QuotaStore>(*db);
        quotaCredentials_ = std::make_unique<quota::ProfileCredentialSource>(
            svc->profiles(), *quotaStore_, &quota::readRealCcSwitchProviders);
        quotaRefresher_ = std::make_unique<quota::QuotaRefresher>(
            *quotaStore_, *quotaCredentials_,
            quota::makeRegistryProviderResolver(), quota::makeRealHttpSendFn());
        quotaRefresher_->start();
        LOG_INFO("余量后台刷新已启动");
    }

    // 停并释放（幂等）。顺序固定：先收线程，再按「刷新器 → 凭据源 → store」
    // 反构造序释放——三者互持引用，顺序错了悬垂。
    void stopQuotaRefresh() {
        if (quotaTestThread_.joinable()) quotaTestThread_.join();
        quotaTestDone_.store(false, std::memory_order_release);
        if (quotaRefresher_) {
            quotaRefresher_->stopAll();   // 幂等 + join；析构里还有一次兜底
            quotaRefresher_.reset();
        }
        quotaCredentials_.reset();
        quotaStore_.reset();
    }

    // 打开余量查询弹窗：取 profile + 已存配置预填
    void openQuotaDialog(int64_t profileId) {
        ClaudeService* svc = ClaudeService::global();
        if (svc == nullptr) {
            showToast("余量查询", "服务未就绪", true);
            return;
        }
        auto profile = svc->getProfile(profileId);
        if (profile.isFail()) {
            showToast("余量查询失败", profile.errorMessage(), true);
            return;
        }
        // 明文密钥只取一次、仅供内存；读失败不阻断开窗
        std::string apiKey;
        auto key = svc->loadApiKey(profileId);
        if (key.isOk()) apiKey = key.value();
        quota::QuotaConfigRow existing;
        bool hasExisting = false;
        if (quotaStore_) {
            auto cfg = quotaStore_->loadConfig(profileId);
            if (cfg.isOk()) {
                existing = cfg.value();
                hasExisting = true;
            }
        }
        quota::openQuotaDialog(state_.quota, profileId, profile.value().baseUrl, apiKey,
                               hasExisting ? &existing : nullptr);
    }

    // 消费 Router 挂在本页的「去配置」负载（模块隔离，home 不得 include 本模块）。
    void consumeQuotaConfigPayload() {
        auto* router = ::core::appRouter();
        if (router == nullptr) return;
        const std::string payload =
            router->consumePendingPayload(::core::PageId::ClaudeProxy);
        static const std::string kPrefix = "action:quota_config:";
        if (payload.rfind(kPrefix, 0) != 0) return;   // 不是本模块的负载
        const std::string idText = payload.substr(kPrefix.size());
        if (idText.empty()) return;
        // 全数字才认：atoi 遇非数字返 0 会误开 profile 0
        bool allDigits = true;
        for (char c : idText) {
            if (c < '0' || c > '9') { allDigits = false; break; }
        }
        if (!allDigits) {
            LOG_WARN("余量配置负载 id 非法: " + payload);
            return;
        }
        openQuotaDialog(std::strtoll(idText.c_str(), nullptr, 10));
    }

    // 后台跑一次真 fetch（UI 不等）；结果槽独立于详情页 testResult*
    void runQuotaTestConnection(const quota::ProviderConfig& cfg) {
        harvestQuotaTestResult();
        if (quotaTestThread_.joinable()) {
            state_.quota.testMessage = "上一次测试连接仍在进行，请稍候";
            state_.quota.testIsError = true;
            return;
        }
        const std::string providerId = cfg.providerId;
        const quota::QuotaProvider* provider =
            quota::findProvider(providerId);
        if (provider == nullptr) {
            state_.quota.testMessage = "未选择供应商";
            state_.quota.testIsError = true;
            return;
        }
        quotaTestResult_ = quota::QuotaResult{};
        state_.quota.testMessage.clear();
        state_.quota.testIsError = false;
        quotaTestThread_ = std::thread([this, providerId, cfg] {
            const quota::QuotaProvider* p = quota::findProvider(providerId);
            if (p == nullptr) {
                quotaTestResult_.error = quota::QuotaError::NotConfigured;
                quotaTestResult_.errorMessage = "未注册的余量供应商：" + providerId;
            } else {
                quotaTestResult_ = p->fetch(cfg, quota::makeRealHttpSendFn());
            }
            quotaTestDone_.store(true, std::memory_order_release);
            ::core::platform::requestUiUpdate();
        });
    }

    // 收割测试连接结果（每帧轮询；join 即 happens-before）
    void harvestQuotaTestResult() {
        if (!quotaTestThread_.joinable()) return;
        if (!quotaTestDone_.load(std::memory_order_acquire)) return;
        quotaTestThread_.join();
        quotaTestDone_.store(false, std::memory_order_release);
        state_.quota.testing = false;
        state_.quota.testErrorKind = quotaTestResult_.error;
        state_.quota.testMessage = quota::quotaTestMessage(quotaTestResult_);
        state_.quota.testIsError = quotaTestResult_.error != quota::QuotaError::None;
    }

    // 保存弹窗配置：落库 + 关闭 + toast
    void saveQuotaConfig(const quota::QuotaConfigRow& row) {
        if (!quotaStore_) {
            state_.quota.error = "数据库未就绪，无法保存（应用数据根未初始化）";
            return;
        }
        auto saved = quotaStore_->saveConfig(row);
        if (saved.isFail()) {
            state_.quota.error = "保存失败：" + saved.errorMessage();
            return;
        }
        state_.quota.open = false;
        showToast("已保存",
                  row.providerId.empty()
                      ? std::string("已关闭该 Profile 的余量查询")
                      : ("余量查询配置已保存（供应商：" + row.providerId +
                         "），后台将在 " +
                         std::to_string(quota::kQuotaProductionScanIntervalSec) +
                         " 秒内自动查询"),
                  false);
        // 不在这里同步 refreshNow()：真网络查询会冻 UI 线程
        LOG_INFO("余量配置已保存（profile=" + std::to_string(row.profileId) +
                 "，provider=" + row.providerId + "，ccswitch=" +
                 row.ccswitchProviderId + "）");
    }

    QuotaDialogHooks quotaHooks() {
        QuotaDialogHooks h;
        h.save = [this](const quota::QuotaConfigRow& row) { saveQuotaConfig(row); };
        h.testConnection = [this](const quota::ProviderConfig& cfg) {
            runQuotaTestConnection(cfg);
        };
        h.setField = [this](const std::string& key, const std::string& value) {
            for (quota::QuotaDialogField& f : state_.quota.fields) {
                if (f.spec.key != key) continue;
                f.value = value;
                // 一改即从 cc-switch 脱钩
                f.fromCcSwitch = false;
                f.sourceLabel = "手动填写";
                return;
            }
        };
        h.selectProvider = [this](int idx) {
            state_.quota.providerIdx = idx;
            state_.quota.providerDdOpen = false;
            quota::syncQuotaDialogFields(state_.quota);
            // 换供应商后旧 cc-switch 引用对不上，不清会写到新供应商行上
            state_.quota.ccSwitchIdx = -1;
        };
        h.loadCcSwitch = [this] {
            state_.quota.ccSwitchLoaded = false;   // 允许重读
            quota::loadQuotaCcSwitch(state_.quota);
        };
        h.selectCcSwitch = [this](int idx) {
            quota::selectQuotaCcSwitch(state_.quota, idx);
        };
        h.toast = [this](const std::string& title, const std::string& msg,
                         bool isError) { showToast(title, msg, isError); };
        return h;
    }

    void doTestConnection(const Profile& p) {
        ClaudeService* svc = ClaudeService::global();
        if (!svc) return;
        harvestTestResult();   // 先收上一轮结果（页面不可见时在这里积累）
        if (testThread_.joinable()) {
            showToast("操作进行中", "上一次校验/测试仍在进行，请稍候", false);
            return;
        }
        // 同步 HTTP（15s）挪后台线程避免冻 UI；结果经原子标志回 compose 收割
        showToast("测试中", "正在请求 " + p.baseUrl + "/v1/models，最长 15 秒…", false);
        testThread_ = std::thread([this, p, svc] {
            auto r = svc->testConnection(p);
            if (r.isFail()) {
                testResultTitle_ = "连通失败";
                testResultMsg_ = r.errorMessage();
                testResultIsError_ = true;
            } else {
                const int code = r.value().statusCode;
                if (code >= 200 && code < 300) {
                    testResultTitle_ = "连通成功";
                    testResultMsg_ = "上游返回 HTTP " + std::to_string(code) + "，API 可用";
                    testResultIsError_ = false;
                } else {
                    testResultTitle_ = "连通异常";
                    testResultMsg_ = "上游返回 HTTP " + std::to_string(code) +
                                      "（检查 API Key 或 Base URL）";
                    testResultIsError_ = true;
                }
            }
            testDone_.store(true, std::memory_order_release);
            core::platform::requestUiUpdate();
        });
    }

    // 收割后台结果：join 后弹 toast；代理启动也在这里（join 后无并发写者，
    // bind + syncTakeOver 回 UI 线程）。模块被关时这里不被调，代理不会偷偷起来。
    void harvestTestResult() {
        if (!testThread_.joinable()) return;
        if (!testDone_.load(std::memory_order_acquire)) return;  // 还在跑
        testThread_.join();
        testDone_.store(false);
        if (testResultStartProxy_) {          // 校验通过 → 这时才 bind + 接管
            testResultStartProxy_ = false;
            ClaudeService* svc = ClaudeService::global();
            if (svc != nullptr) {             // 模块已停/析构 → 不起
                auto r = svc->startProxy("127.0.0.1", svc->configuredProxyPort());
                if (r.isFail()) {
                    testResultTitle_ = "启动代理失败";
                    testResultMsg_ = r.errorMessage();
                    testResultIsError_ = true;
                } else {
                    testResultTitle_ = "代理已启动";
                    testResultMsg_ = "监听 http://127.0.0.1:" +
                                     std::to_string(r.value());
                    testResultIsError_ = false;
                }
            }
        }
        showToast(testResultTitle_, testResultMsg_, testResultIsError_);
    }

    ClaudeUiState state_;

    ui_kit::ToastState toast_;

    // 「服务未就绪」toast 只发一次的门
    bool serviceNotReadyWarned_ = false;

    // 测试连通/启动代理后台线程。结果成员为普通变量，由 release/acquire
    //（testDone_）+ join 保证发布序；testResultStartProxy_ 供收割点起代理
    std::thread testThread_;
    std::atomic<bool> testDone_{false};
    std::string testResultTitle_;
    std::string testResultMsg_;
    bool testResultIsError_ = false;
    bool testResultStartProxy_ = false;

    // 余量链装配（析构安全靠 stopQuotaRefresh() 的显式顺序，不靠声明序）
    std::unique_ptr<quota::QuotaStore> quotaStore_;
    std::unique_ptr<quota::ProfileCredentialSource> quotaCredentials_;
    std::unique_ptr<quota::QuotaRefresher> quotaRefresher_;
    // 导入暂停/恢复的订阅 token：token 析构即退订，必须持有到 onStop。
    std::vector<::core::event::EventBus::Subscription> dbImportSubs_;
    // 订阅存活闸（见 onStart 注释）——退订拦不住 emit 已拷出的在途 handler。
    std::atomic<bool> dbImportHandlersEnabled_{false};
    // 弹窗「测试连接」后台线程 + 结果槽（独立于详情页；UI 线程 join 后才读）
    std::thread quotaTestThread_;
    std::atomic<bool> quotaTestDone_{false};
    quota::QuotaResult quotaTestResult_;

    // 智能体工具集（claude.* 四工具）；onStart 注册 / onStop 先注销
    std::unique_ptr<ClaudeTools> claudeTools_;

    // 重入幂等守卫
    bool started_ = false;
};

} // namespace module::claude

extern "C" ::core::Module* create_claude_module() {
    return new module::claude::ClaudeModule();
}
