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
#include "claude_ui_state.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/theme.h"
#include "core/component_theme.h"
#include "core/database.h"
#include "core/app_config.h"
#include "platform/folder_picker.h"
#include "platform/json.h"
#include "provider_config.h"
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
        if (testThread_.joinable()) testThread_.join();
    }

    // 测试缝：开关切换确认弹窗（调用后选卡不弹窗，直走选中路径）
    void bypassSwitchConfirm() { state_.list.switchBypassConfirm_ = true; }

    // 测试缝：强制显示引导条（覆盖 proxyEverSeen_/guideDismissed_，图标锚点确定性）
    void forceShowGuide() {
        state_.list.proxyEverSeen_ = false;
        state_.list.guideDismissed_ = false;
    }

    // UI（骨架：分区委派，各区实现见 claude_ui_*.cpp）

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        const auto& t = core::theme::current();
        const auto themeTokens = core::theme::componentTokensWeChat(t);
        // S3.4 顶部收紧为 24；S4.2 弹性骨架：左右 32、底贴底（无下 padding）
        const float padTop = 0.0f;                               // 标题顶格不留空（2026-10-25 拍板）
        const float padSide = t.sizes.spacingLarge * 2.0f;     // 32 —— S4.2 弹性内边距
        const float contentW = screen.width - padSide * 2.0f;   // 宽 = 窗口宽 − 双侧内边距
        const float contentH = screen.height - padTop;           // 高 = 窗口高 − 顶内边距（底贴底）
        const float headerH = ui_kit::kHeaderH;
        const float gap = t.sizes.spacingLarge;
        // S3.4 左栏固定 240px，右栏 flex 自适应。
        const float leftW = 240.0f;
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
        // S3.4 引导条可手动关闭
        const bool showGuide = !proxyRunning && !state_.list.proxyEverSeen_ &&
                               !state_.list.guideDismissed_;
        // 预留高必须与绘制侧一致；bodyH 在 compose 期间就要传给左右子面板。
        const float bodyH = contentH - headerH - t.sizes.spacingLarge -
                            (showGuide ? kGuideBarH + t.sizes.spacingLarge : 0.0f);

        // 页面不自铺不透明底，用主窗口 clearColor（半透/磨砂档才生效）

        // 错误态：加载失败不静默吞错显示假空态
        std::string profilesError;

        // profiles/active 走内存快照（脏标记触发重查，失败保持脏重试）
        std::vector<Profile> profiles;
        Profile activeProfile;
        bool hasActive = false;
        if (!svc) {
            profilesError = "服务未就绪（ClaudeService 未注册）";
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
            activeProfile = state_.list.cachedActive_;
            hasActive = state_.list.cachedHasActive_;
            profilesError = state_.list.cachedProfilesError_;
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
        dv.sharedConfigs = &state_.list.cachedSharedConfigs_;

        ui.column("claude.root")
            .size(screen.width, screen.height)
            .padding(padSide, padTop, padSide, 0.0f)  // left/top/right/bottom: H17 fix——EUII padding 实参序=(left,top,right,bottom)，原（padTop,padSide,0,padSide）被错解为 left=24 top=32 right=0 bottom=32，致左距从 32→24 且顶内距从 24→32，底部整块越出页面区 64px
            .gap(t.sizes.spacingLarge)
            .content([&] {
                drawClaudeHeader(ui, contentW, headerH, t, proxyRunning, portStr,
                                 [this] { doToggleProxy(); });

                if (showGuide) drawClaudeGuideBar(ui, contentW, t, state_.list);

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
                              formHooks());
        drawDeleteConfirmDialog(ui, screen, t, themeTokens, state_.form,
                                [this](int64_t id) { doDelete(id); });
        // 共享配置片段编辑弹窗 + 删除确认；绘制序 = Esc 序链（见 closeTopmostDialog）
        drawSharedConfigDialog(ui, screen, t, themeTokens, state_.shared,
                               sharedHooks());
        drawSharedConfigDeleteConfirmDialog(
            ui, screen, t, themeTokens, state_.shared,
            [this](int64_t id, const std::string& name) {
                doDeleteShared(id, name);
            });
        // cc-switch 导入弹窗，绘制序即 Esc 序链最顶层
        drawCcSwitchImportDialog(ui, screen, t, themeTokens,
                                 state_.ccswitchImport, ccswitchImportHooks());
        drawSwitchConfirmDialog(ui, screen, t, themeTokens,
                                  state_.list);
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
        // 旧凭据回填：老库的 Keychain 凭据迁进 api_key_encrypted（新库无 keychain_account
        // 列 → 零 Keychain 触达，启动路径其余环节不碰钥匙串）
        auto backfill = svc->backfillCredentialsFromKeychain();
        if (backfill.isFail()) {
            LOG_WARN("onStart: 凭据回填失败 - " + backfill.errorMessage());
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
    }

    // 进页标记缓存脏，下一帧 compose 重查
    void onPageEnter() override { state_.list.dataDirty_ = true; }

    void onStop() override {
        if (!started_) return;   // 重入幂等
        LOG_INFO("Claude 模块停止");
        // 先注销工具、最后停代理。
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
            state_.shared.open_ || state_.shared.deleteConfirmOpen_ ||
            state_.ccswitchImport.open_;
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
    // Esc 关最上层弹窗（顺序 = 绘制序倒序）
    bool closeTopmostDialog() {
        if (state_.ccswitchImport.open_) { state_.ccswitchImport.open_ = false; return true; }
        if (state_.shared.deleteConfirmOpen_) { state_.shared.deleteConfirmOpen_ = false; return true; }
        if (state_.shared.open_) { state_.shared.open_ = false; return true; }
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
            openEditForm(state_.form, state_.import, p);
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
            openEditForm(state_.form, state_.import, p);
        };
        a.run = [this](int64_t id) { doRun(id); };
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

    // app_config 键名：上次 claude 运行所选的工作目录
    static constexpr const char* kClaudeRunLastDirKey = "claude.run.lastDir";

    // 直连：生成临时 settings + claude --settings 打开新会话（零全局改动）。
    // 运行前先弹出目录选择对话框（原生模态框），所选目录作为 claude 进程工作目录；
    // 用户取消 = 静默返回，不弹 toast、不启动会话。
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

        // 读上次所选目录作为对话框初始定位
        std::string lastDir;
        if (auto* db = ::core::appDatabase()) {
            auto saved = core::appconfig::get(*db, kClaudeRunLastDirKey);
            if (saved.isOk()) lastDir = saved.value();
        }

        // 弹出目录选择（走注入缝：生产 = 平台原生对话框，测试经 setPickerForTest 替换）
        auto chosen = platform::folderpicker::pickerSlot()("选择工作目录", lastDir);
        if (chosen.isFail()) {
            // 用户取消：静默返回，不启动、不弹 toast
            return;
        }
        const std::string& workingDir = chosen.value();

        // 记录本次所选目录（下次打开默认定位到它）
        if (auto* db = ::core::appDatabase()) {
            core::appconfig::set(*db, kClaudeRunLastDirKey, workingDir);
        }

        std::string hint;
        auto r = svc->runProfile(id, &hint, workingDir);
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
        // 坏 JSON 防「静默丢键」：试解析，失败先 LOG_WARN 再继续导出
        if (!p.extraEnvJson.empty()) {
            auto ee = parseExtraEnv(p.extraEnvJson);
            if (ee.isFail()) {
                LOG_WARN("复制配置: extra_env 解析失败（profile id=" +
                         std::to_string(p.id) + "），导出将缺失该部分 env 键 - " +
                         ee.errorMessage());
            }
        }
        const std::string json = buildProfileExportJson(
            p, /*keyPlaceholder=*/p.apiKeyHint);
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
        if (auto doc = platform::json::parse(json);
            doc.isOk() && doc.value().isObject()) {
            const auto rootObj = doc.value().asObject();
            const auto envVal = rootObj.get("env");
            if (envVal.isObject()) exportedKeys = envVal.asObject().keys().size();
        }
        LOG_WARN("复制配置: 已调用系统剪贴板写入（void API 无回读校验）" +
                 std::string("profile id=") + std::to_string(p.id) +
                 " env 键数=" + std::to_string(exportedKeys));
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

    void doTestConnection(const Profile& p) {
        ClaudeService* svc = ClaudeService::global();
        if (!svc) return;
        harvestTestResult();   // 先收上一轮结果（页面不可见时在这里积累）
        if (testThread_.joinable()) {
            showToast("操作进行中", "上一次校验/测试仍在进行，请稍候", false);
            return;
        }
        // 同步 HTTP（15s）挪后台线程避免冻 UI；结果经原子标志回 compose 收割
        showToast("测试中", "正在请求 " + p.baseUrl + "/v1/messages，最长 15 秒…", false);
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
                    std::string hint = "（检查 API Key 或 Base URL）";
                    if (code == 401 || code == 403) {
                        hint = "（API Key 被上游拒绝）";
                    } else if (code == 404) {
                        hint = "（接口不存在或模型不可用，检查 Base URL 与模型配置）";
                    }
                    testResultMsg_ = "上游返回 HTTP " + std::to_string(code) +
                                      std::move(hint);
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

    // 智能体工具集（claude.* 四工具）；onStart 注册 / onStop 先注销
    std::unique_ptr<ClaudeTools> claudeTools_;

    // 重入幂等守卫
    bool started_ = false;
};

} // namespace module::claude

extern "C" ::core::Module* create_claude_module() {
    return new module::claude::ClaudeModule();
}

// 测试缝：关掉切换确认弹窗，让全量回归测试直走选中路径
extern "C" void claude_test_bypass_switch_confirm(::core::Module* mod) {
    auto* m = dynamic_cast<module::claude::ClaudeModule*>(mod);
    if (!m) return;
    m->bypassSwitchConfirm();
}

// 测试缝：强制显示引导条（重置 proxyEverSeen_/guideDismissed_，图标锚点确定性）
extern "C" void claude_test_force_show_guide(::core::Module* mod) {
    auto* m = dynamic_cast<module::claude::ClaudeModule*>(mod);
    if (!m) return;
    m->forceShowGuide();
}
