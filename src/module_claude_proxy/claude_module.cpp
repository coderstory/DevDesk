// src/module_claude_proxy/claude_module.cpp
// Claude 配置切换器模块 - Module 接口 + compose 骨架 + 事件回调接线 + 操作
//
// 接线清单（W5）：
//   新建/编辑 Profile（dialog 表单）→ createProfile / updateProfile
//   启用 → setActiveProfile；删除（确认 dialog）→ deleteProfile
//   测试连通 → testConnection；代理开关 → startProxy / stopProxy
//   每次操作后标脏 compose 数据缓存，下一帧发一次刷新意图（审计 perf [13]
//   + H2：命中帧零 SQLite，查询全在后台线程跑，渲染线程只读快照）
//
// R4（CPX-05，spec 2026-09-27-god-class-refactor-design §3）：本文件从「上帝
// 类」退化为**骨架**——可视区域全部搬进同模块的 4 个 UI 单元（自由函数，状态
// 显式入参、不捕获 this）：
//   claude_ui_list.{h,cpp}    顶部 header / 引导条 / 左栏 Profile 列表
//   claude_ui_detail.{h,cpp}  右栏三 pane + 操作按钮组 + 「最近调用」预览
//   claude_ui_form.{h,cpp}    表单弹窗（tabs 三页）+ 删除确认弹窗 + 表单状态机
//   claude_ui_logs.{h,cpp}    完整调用记录弹窗（过滤/分页/后台取数）
//   claude_ui_state.h/.cpp    41 项成员按区域收成 5 个纯值 struct + 数据缓存的
//                             后台刷新（H2 快照，原成员函数搬出）
//   claude_ui_logic.{h,cpp}   4 个纯函数 + 调用记录常量（可无头单测）
// 本文件保留：Module 接口、compose 骨架（分区委派）、事件回调接线、onStart/
// onStop、真接 ClaudeService 的操作。
#include "claude_service.h"
#include "claude_ui_detail.h"
#include "claude_ui_form.h"
#include "claude_ui_list.h"
#include "claude_ui_logic.h"
#include "claude_ui_logs.h"
#include "claude_ui_state.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/theme.h"
#include "core/component_theme.h"
#include "platform/json.h"
#include "provider_config.h"
#include "upstream_dialect.h"
#include <core/window/window_backend.h>
#include <ui_kit/metrics.h>
#include <ui_kit/toast.h>

#include <eui/dsl.h>

#include <algorithm>
#include <atomic>
#include <ctime>
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
        // 后台测试线程必须先收（最坏阻塞到 HTTP 超时 15s，避免悬垂 this）
        if (testThread_.joinable()) testThread_.join();
        // 审计 H2：后台刷新经 future 回传（工作线程不触碰 this），成员析构
        // 自动等本轮查询结束，无需显式 join（CPX-06 的调用记录查询同款）
    }

    // ============ UI（骨架：分区委派，各区实现见 claude_ui_*.cpp） ============

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
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

        // ---- 读真实状态（ClaudeService 全局单例，由 main.cpp 注册）----
        ClaudeService* svc = ClaudeService::global();
        const bool proxyRunning = svc ? svc->isProxyRunning() : false;
        const int runningPort = svc ? svc->proxyPort() : 0;
        const std::string portStr = runningPort > 0
            ? (":" + std::to_string(runningPort))
            : std::string("");   // 未运行时不显示假端口（副标题另有提示）

        // 引导条（§6.4）：代理未运行且本会话从未启动过时显示（内存态一次性）
        if (proxyRunning) state_.list.proxyEverSeen_ = true;
        const bool showGuide = !proxyRunning && !state_.list.proxyEverSeen_;
        // 引导条的预留高 = 它的渲染高（kGuideBarH，绘制侧同一常量）+ 根 column
        // 给它的那一个 spacingLarge 间距。此前这里写死 30（绘制侧实际 18 + 间距
        // 16 = 34），预留与绘制各写一份 → 根 column 子项总高超出可用高 4px，
        // 底部 padding 被压缩（macOS 布局审计 finding: claude_module.cpp:86）。
        // 为什么不改成让 body 行 flexGrow 吃剩余高：body 行的高度（bodyH）在
        // **compose 期间**就要传给左右两个子面板（claude_ui_list / claude_ui_detail
        // 都按它排内部几何），而 flexGrow 的结果要 layout 之后才知道 → 需要两遍
        // 布局，改动面远大于收益。故取「同一常量」这条修法。
        const float bodyH = contentH - headerH - t.sizes.spacingLarge -
                            (showGuide ? kGuideBarH + t.sizes.spacingLarge : 0.0f);

        // 页面底色 = 主窗口 clearColor（随主题/窗口效果档运行时跟随，
        // 磨砂 Phase B/D）——不自铺不透明底，半透/磨砂档主体才生效

        // 错误态（仿 Home statsError_）：加载失败不静默吞错显示假空态
        std::string profilesError;
        std::string callsError;

        // 审计 perf [13] + H2：profiles/recent/active 缓存为成员，重组合只从
        // 内存拷贝，SQLite 查询全部在后台线程（refreshDataCache 只发意图 /
        // 搬已就绪结果，零 IO）。脏标记（onStart/onPageEnter/写操作）触发三类
        // 重查；recent 另按 ClaudeService::callLogVersion 感知代理 worker 异步
        // 落库（刷新最多晚一个 200ms 节流 tick + 1 帧，不丢数据）；
        // 查询失败保持脏、按节流窗口重试（错误态照常显示），同旧退化路径。
        std::vector<Profile> profiles;
        std::vector<ClaudeService::CallLogEntry> recent;
        Profile activeProfile;
        bool hasActive = false;
        if (!svc) {
            profilesError = "服务未就绪（ClaudeService 未注册）";
            callsError = "服务未就绪（ClaudeService 未注册）";
            state_.list.dataDirty_ = true;   // 服务未就绪查不了，保持脏等下帧
        } else {
            refreshDataCache(state_.list, svc);
            profiles = state_.list.cachedProfiles_;
            recent = state_.list.cachedRecent_;
            activeProfile = state_.list.cachedActive_;
            hasActive = state_.list.cachedHasActive_;
            profilesError = state_.list.cachedProfilesError_;
            callsError = state_.list.cachedCallsError_;
        }

        // 详情面板展示的 profile：优先用户选中的，其次激活的
        const Profile* shown = nullptr;
        for (const auto& p : profiles) {
            if (p.id == state_.list.selectedId_) { shown = &p; break; }
        }
        if (!shown && hasActive) shown = &activeProfile;

        DetailView dv;   // 右栏只读视图（R4：显式入参取代自由函数捕获 this）
        dv.svc = svc;
        dv.shown = shown;
        dv.nameValue = shown ? shown->name : std::string("(未启用)");
        dv.modelValue = shown ? modelsSummary(*shown) : std::string("-");
        dv.urlValue = shown ? shown->baseUrl : std::string("-");
        dv.tokenValue = shown
            ? (shown->apiKeyHint + " [Keychain]")
            : std::string("(无激活 profile)");
        dv.recent = &recent;
        dv.callsError = callsError;
        dv.now = now;
        // 注：DetailView 不再带 bodyH（审计 F-01）；容器可用高只由下面
        // drawDetailPanel 的 bodyH 形参进详情面板
        dv.sharedConfigs = &state_.list.cachedSharedConfigs_;   // 批 2：详情绑定行
        // 高级配置 pane（2026-09-30 batch-fix B，设计 §3.2④）：生效方言 +
        // 备用 Profile 名。方言：显式覆盖 → 「名（显式）」；空 →
        // 「自动（按站点推断）→ 生效名」（推断结果可审计，正是本 pane 的价值）；
        // 显示名统一走 dialectKindDisplayName（§3.2① 单一来源，不直接用
        // dialectKindToText 内部值）。备用名：从既有 profiles 快照按
        // fallbackProfileId 解析（悬空/0 → 「无」/「（已删除）」），与列表
        // 快照同源、pane 内不做服务调用（避免每帧 DB 查询）
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

        // 高级配置 pane 的键来源清单（advanced-json-editor 设计 §8.5，0edd3cf
        // 形态复用）：生效 JSON = profile ⊕ 片段（片段赢，§8.2 生成时叠加）。
        // 逐顶层键给来源：键 ∈ 片段对象 → 片段（envSourceLabel(Shared)——片段
        // 赢的键都来自片段）；其余 → 本 Profile。"model" 键 → 附加说明（覆盖
        // 模型槽位，D-4）。与列表快照同源、compose 内装配（pane 零服务调用）。
        // 空态/解析失败经 keys 空 + note 区分（约定见 DetailView 注释）
        if (shown != nullptr) {
            std::string fragmentJson;
            if (shown->sharedConfigId != 0) {
                const SharedConfig* sc = findSharedById(
                    state_.list.cachedSharedConfigs_, shown->sharedConfigId);
                if (sc != nullptr) fragmentJson = sc->extraSettingsJson;
            }
            // warningsOut（审计 F-03）：非法/超限一侧被合并纯函数按空处理时
            // 在此捕获——keys 空且 warnings 非空 = 配置存在但失效，pane 显示
            // 「解析失败」提示态而非「未配置」（§8.5 验收点；接管 WARN 日志
            // 之外的展示层口径）
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
                    // 片段对象键集（片段赢的键 = 片段里出现过的键）
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
                            // 说明只在抑制真发生时给：applyExtraSettingsTo-
                            // SettingsDoc 的 ANTHROPIC_MODEL 抑制仅对**非空
                            // 字符串** model 生效；空串 / 非字符串时该键照写
                            // settings.json，此说明不适用（审计 R1-1）
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

                // 引导条（§6.4）：代理未运行且本会话从未启动过时一次性提示接管语义
                if (showGuide) drawClaudeGuideBar(ui, contentW, t);

                // ========== 主区: Profile 列表 + 详情 ==========
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
                              state_.list.cachedProfiles_,   // 批 3：备用下拉数据源
                              formHooks());
        drawDeleteConfirmDialog(ui, screen, t, themeTokens, state_.form,
                                [this](int64_t id) { doDelete(id); });
        drawCallsDialog(ui, screen, t, themeTokens, state_.logs, toastHook());
        // 批 2：共享配置片段编辑弹窗 + 删除确认（画在表单弹窗之后 =
        // Esc 序链最顶层，见 closeTopmostDialog）
        drawSharedConfigDialog(ui, screen, t, themeTokens, state_.shared,
                               sharedHooks());
        drawSharedConfigDeleteConfirmDialog(
            ui, screen, t, themeTokens, state_.shared,
            [this](int64_t id, const std::string& name) {
                doDeleteShared(id, name);
            });
        // 高级设置 JSON 全屏编辑覆盖层（2026-09-30 json-fullscreen）：画在全部
        // 弹窗之后 + 显式 z=1050——盖住表单 dialog（默认 1000）、让位 toast
        //（1100，「应用」成功的确认可见）。可见性派生自 formOpen_ &&
        // formJsonFullscreen_（应用成功关表单 → 全屏同帧消失，零孤儿态）
        drawJsonFullscreenOverlay(ui, screen, t, themeTokens, state_.form,
                                  formHooks(),
                                  state_.list.cachedSharedConfigs_);
        harvestTestResult();  // 后台「测试连通」结果回 UI（每帧轮询）
        drawToast(ui, screen, themeTokens);

        // 列表右键菜单（UI 现代化 M1-5）：画在弹窗之上（同 todo 的
        // drawContextMenu 位置——组件自身管理 zIndex）
        drawProfileContextMenu(ui, screen, themeTokens, state_.list, profiles,
                               listActions());

        LOG_DEBUG("Claude 模块 compose 完成");
    }

    void onStart() override {
        LOG_INFO("Claude 模块启动");
        ClaudeService* svc = ClaudeService::global();
        if (!svc) {
            LOG_WARN("onStart: ClaudeService::global() 为 null");
            state_.list.dataDirty_ = true;   // 服务未就绪查不了：保持脏，进页后由后台刷新补上
            return;
        }
        // 审计 H2：首次装载同步做一次（onStart = 启动期，非渲染路径）。compose
        // 恒读快照，首帧必须有值——异步首刷会让页面先闪一帧空列表（硬约束：
        // 不得空帧/闪空）；此后每次刷新都走 refreshDataCache 的后台线程
        applyDataRefresh(state_.list, runDataRefresh(svc, true, true));
        // 一次性迁移（2026-09-30 数据丢失事故根治）：旧全局 service
        // "DevDesk" 的 profile 密钥搬进按数据根派生的独立命名空间。必须
        // 先于孤儿清理——迁移先把条目写进本实例的 service，清理的 keep
        // （来自 DB 行）才与新命名空间对得上
        auto mig = svc->migrateLegacyKeychainService();
        if (mig.isFail()) {
            LOG_WARN("onStart: 旧 Keychain service 迁移失败 - " +
                     mig.errorMessage());
        } else if (mig.value() > 0) {
            LOG_INFO("onStart: 已迁移 " + std::to_string(mig.value()) +
                     " 个密钥到数据根派生 service");
        }
        // 启动时孤儿密钥清理（一次扫描，删除无对应 profile 的 keychain 条目）
        auto orphan = svc->cleanupOrphanKeychainEntries();
        if (orphan.isFail()) {
            LOG_WARN("onStart: 孤儿 Keychain 清理失败 - " + orphan.errorMessage());
        }
        // 硬性需求 1（provider-config Phase B）：代理默认不启动，不再兜底
        // startProxy——由 main.cpp 按设置页「随应用启动」门控，或用户点本页
        // 右上角按钮手动启动；这里只做状态日志
        if (svc->isProxyRunning()) {
            LOG_INFO("onStart: 代理已在运行 port=" +
                     std::to_string(svc->proxyPort()));
        } else {
            LOG_INFO("onStart: 代理未运行（默认不自动启动，等待用户手动启动）");
        }
    }

    // 审计 perf [13]：进入页面（切页回来/同页重入）标记缓存脏，下一帧 compose
    // 重查三类数据（Home 仪表盘进页刷新同款模式）
    void onPageEnter() override { state_.list.dataDirty_ = true; }

    void onStop() override {
        LOG_INFO("Claude 模块停止");
        ClaudeService* svc = ClaudeService::global();
        if (svc && svc->isProxyRunning()) {
            svc->stopProxy();
            LOG_INFO("onStop: 代理已停止");
        }
    }

    // 产品审计 [22]：页级快捷键（通道 = app 级 onKeyEvent → Router 派发到
    // 当前页模块，同 Markdown Ctrl+Shift+P / Todo Ctrl+↑↓ 先例；只在窗口
    // 聚焦时生效——EUI-NEO 无全局 OS hotkey 是已登记限制）。
    //   Ctrl+N      新建 Profile（任一弹窗打开时不响应，防止重置填写中的表单）
    //   Ctrl+S      保存/创建表单中编辑的 Profile（=「保存/创建」按钮，
    //               表单内输入框聚焦时同样生效：input 不消费 Ctrl+字母组合键）；
    //               表单未打开 = 无可保存对象，不消费
    //   Ctrl+Enter  启动/停止代理（仅 Press 消费，按住 Repeat 不会来回启停；
    //               弹窗打开时不生效；输入框聚焦时 Enter 被 input 消费，
    //               天然不触发——正好避免打字中途启停代理）
    // 返回 false = 未消费（键盘事件继续按 EUI-NEO 语义丢弃/传递）。
    bool onKeyEvent(const eui::KeyEvent& key) override {
        if (key.action != eui::KeyAction::Press) {
            return false;
        }
        // 审计 M5：Esc 关最上层弹窗。键走得到本通道 = 焦点元素没消费（表单各
        // 输入框均挂 onCancel，聚焦时 Esc 已被 input 收走 → 天然让位不重复）
        if (key.key == eui::InputKey::Escape) {
            return closeTopmostDialog();
        }
        if (!key.modifiers.shortcut() || key.modifiers.alt) {
            return false;
        }
        const bool anyDialogOpen =
            state_.form.formOpen_ || state_.form.deleteConfirmOpen_ ||
            state_.logs.callsDialogOpen_ ||
            state_.shared.open_ || state_.shared.deleteConfirmOpen_;
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
    // 审计 M5：Esc 关最上层弹窗（顺序 = 绘制序/z 序倒序：全屏 JSON 编辑覆盖层
    //（z=1050，画在所有弹窗之上）> 共享片段删除确认 > 共享片段编辑 > 调用记录
    // > 删除确认 > 表单）。返回 true = 本键已消费；无弹窗返回 false
    bool closeTopmostDialog() {
        // 全屏编辑覆盖层先于一切弹窗（2026-09-30 json-fullscreen：Esc 退全屏
        // 回表单，第二次 Esc 才关表单）
        if (state_.form.formJsonFullscreen_ && state_.form.formOpen_) {
            state_.form.formJsonFullscreen_ = false;
            return true;
        }
        if (state_.shared.deleteConfirmOpen_) { state_.shared.deleteConfirmOpen_ = false; return true; }
        if (state_.shared.open_) { state_.shared.open_ = false; return true; }
        if (state_.logs.callsDialogOpen_) { state_.logs.callsDialogOpen_ = false; return true; }
        if (state_.form.deleteConfirmOpen_) { state_.form.deleteConfirmOpen_ = false; return true; }
        if (state_.form.formOpen_) { state_.form.formOpen_ = false; return true; }
        return false;
    }

    // ============ compose 每帧的接线装配（R4）============
    // 自由函数不捕获 this（spec §5.2）：compose 在调用点把回调显式绑到本模块的
    // 操作 / 状态上；状态写回一律经引用（ClaudeUiState 成员，活到下一帧）。
    ProfileListActions listActions() {
        ProfileListActions a;
        a.onNewProfile = [this] { openCreateForm(state_.form, state_.import); };
        a.onImportCurrent = [this] {
            startImportCurrentConfig(state_.form, state_.import, formHooks());
        };
        // 右键菜单动作（UI 现代化 M1-5）：与 detailActions 既有按钮回调
        // 同源的最小委托（编辑=openEditForm / 复制=doDuplicate /
        // 删除=确认弹窗 / 设为当前=doSetActive），不新写业务逻辑
        a.requestEdit = [this](const Profile& p) {
            openEditForm(state_.form, state_.import, p,
                         state_.list.cachedSharedConfigs_);
        };
        a.requestDuplicate = [this](int64_t id) { doDuplicate(id); };
        // 复制配置（2026-09-30 batch-fix C）：与详情第 6 键同源 doCopyConfig
        a.requestCopyConfig = [this](const Profile& p) { doCopyConfig(p); };
        a.requestDelete = [this](int64_t id, const std::string& name) {
            state_.form.deleteTargetId_ = id;
            state_.form.deleteTargetName_ = name;
            state_.form.deleteConfirmOpen_ = true;
        };
        a.requestSetActive = [this](int64_t id) { doSetActive(id); };
        // 批 2：共享配置片段动作（区块新建 / 卡片编辑）
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

    // 批 2：共享配置片段提交钩子（toast / 标脏与表单同源）
    SharedConfigSubmitHooks sharedHooks() {
        SharedConfigSubmitHooks h;
        h.toast = toastHook();
        h.markDataDirty = [this] { state_.list.dataDirty_ = true; };
        return h;
    }

    DetailActions detailActions() {
        DetailActions a;
        a.warn = [this](const std::string& title, const std::string& msg) {
            showToast(title, msg, true);   // 未选中 Profile 的各键红字提示
        };
        a.enable = [this](int64_t id) { doSetActive(id); };
        a.test = [this](const Profile& p) { doTestConnection(p); };
        a.edit = [this](const Profile& p) {
            openEditForm(state_.form, state_.import, p,
                         state_.list.cachedSharedConfigs_);
        };
        a.requestDelete = [this](int64_t id, const std::string& name) {
            state_.form.deleteTargetId_ = id;
            state_.form.deleteTargetName_ = name;
            state_.form.deleteConfirmOpen_ = true;
        };
        a.duplicate = [this](int64_t id) { doDuplicate(id); };
        // 复制配置（2026-09-30 batch-fix C，设计 §4.2② 第 6 键接线）
        a.copyConfig = [this](const Profile& p) { doCopyConfig(p); };
        a.run = [this](int64_t id) { doRun(id); };
        a.openCalls = [this] { openCallsDialog(state_.logs); };
        return a;
    }

    // ============ Toast 反馈 ============
    // 状态位与绘制接线走 ui_kit（D1 去重）；勾/叹图标仍走 toast 自带 Icon 字体
    // 通道（⚠/✓ 前缀混排正文不可行）——成功用 fa-circle-check（现状逐字保留）
    void drawToast(eui::Ui& ui, const eui::Screen& screen,
                   const components::theme::ThemeColorTokens& themeTokens) {
        ui_kit::drawToast(ui, screen, "claude.toast", toast_, themeTokens,
                          ui_kit::ToastOptions{.okIcon = 0xF058});
    }

    void showToast(const std::string& title, const std::string& msg, bool isError) {
        toast_.show(title, msg, isError);
    }

    // ============ 操作（全部真接 ClaudeService） ============

    void doToggleProxy() {
        ClaudeService* svc = ClaudeService::global();
        if (!svc) {
            showToast("代理操作失败", "服务未就绪（ClaudeService 未注册）", true);
            return;
        }
        if (svc->isProxyRunning()) {
            svc->stopProxy();
            showToast("代理已停止", "本地代理已停止监听", false);
            return;
        }
        // 端口读 service 上的配置值（审计 [14]：main 启动注入 + 设置页改端口
        // 同步；默认 8888）。此前硬编码导致 8888 被占用时用户无处改端口
        auto r = svc->startProxy("127.0.0.1", svc->configuredProxyPort());
        if (r.isFail()) {
            showToast("启动代理失败", r.errorMessage(), true);
            return;
        }
        showToast("代理已启动",
                  "监听 http://127.0.0.1:" + std::to_string(r.value()), false);
    }

    void doSetActive(int64_t id) {
        ClaudeService* svc = ClaudeService::global();
        if (!svc) return;
        state_.list.dataDirty_ = true;   // 审计 perf [13]：写操作标脏
        auto r = svc->setActiveProfile(id);
        if (r.isFail()) {
            showToast("启用失败", r.errorMessage(), true);
            return;
        }
        state_.list.selectedId_ = id;
        showToast("已启用", "该 Profile 已设为激活（ACTIVE）", false);
    }

    // 运行：直连——生成临时 settings + claude --settings 打开新会话（D-1：
    // 零全局改动：不启代理、不切激活、不改 ~/.claude/settings.json）
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
        // 成功：D-5 的退化提示（hint 非空时）+ 直连语义说明（D-1：临时配置
        // 生命周期——会话结束即删；不经代理、不改任何全局配置）
        const std::string head = hint.empty()
            ? std::string("已用该 Profile 直连打开 Claude Code 会话"
                          "（临时配置会话结束后自动删除，未改动任何全局配置）。")
            : (hint + std::string("。"));
        showToast("已启动", head, false);
        state_.list.dataDirty_ = true;   // 下帧重查列表（动作本身零全局改动，成本一帧）
    }

    // 建副本（设计 §4.1/§4.2，M5；2026-09-30 batch-fix C 改名 D-C1）：
    // 建副本但不激活 → 选中态/激活态都不动
    void doDuplicate(int64_t id) {
        ClaudeService* svc = ClaudeService::global();
        if (!svc) return;
        state_.list.dataDirty_ = true;   // 审计 perf [13]：写操作标脏
        auto r = svc->duplicateProfile(id);
        if (r.isFail()) {
            showToast("建副本失败", r.errorMessage(), true);
            return;
        }
        showToast("已建副本",
                  "已创建副本「" + r.value().name + "」（未激活）", false);
    }

    // 复制配置（2026-09-30 batch-fix C，设计 §4.2②）：当前 profile →
    // settings.json 形态 JSON → 系统剪贴板。导出恒不含凭据键
    //（buildProfileExportJson 契约，merge-review 2026-09-30：占位与
    // extra_env 自填 token 一律剔除），**不导明文 key**（§5① 裁决——剪贴板是跨
    // 应用共享面，明文一经粘贴即离开 Keychain 托管）。setClipboardText 返回
    // void、模块层无窗口句柄无法回读校验 → 接线处固定 LOG_WARN 一行留痕
    //（低概率写失败时 toast 已提示成功，属已知限制，audit F5）
    void doCopyConfig(const Profile& p) {
        ClaudeService* svc = ClaudeService::global();
        if (svc == nullptr || p.id == 0) {
            showToast("复制配置失败", "请先选择 Profile", true);
            return;
        }
        // 坏 JSON 防静默丢键（advanced-json-editor §6，两域覆盖——同坏
        // extraEnvJson 的防静默手法 batch-fix §4.2②）：env 域与高级设置 JSON
        // 域（profile 自身 + 片段）各自试解析，失败先 LOG_WARN 一行再继续导出
        //（对应域读路径容错为空/按 profile 自身值），不静默
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
        // 片段 JSON：与列表/详情同源的快照解析（不落服务调用）。v11 起片段
        // 内容 = 高级属性 JSON 原文（§8），坏片段按 profile 自身 JSON 导出
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
        // 占位值：apiKeyHint 即「前6 + *** + 尾4」形态（profile_repository
        // 脱敏生成，同源同口径）。导出恒无凭据键——buildProfileExportJson
        // 契约（merge-review 2026-09-30）把两个 token 键无条件剔除，hint
        // 空或不空、extra_env 是否自填 token 均不进剪贴板
        const std::string json = buildProfileExportJson(
            p, fragmentJson, /*keyPlaceholder=*/p.apiKeyHint);
        if (json.empty()) {
            showToast("复制配置失败", "配置序列化失败", true);
            return;
        }
        core::window::setClipboardText(json);
        // void API 无回读下的最低留痕（§4.2②）：profile id + 导出键数
        //（advanced-json-editor §6：env 键数 + 非 env 顶层键数两域合计）
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
        state_.list.dataDirty_ = true;   // 审计 perf [13]：写操作标脏
        // 产品审计 2026-09-23 [1]（§7 安全第 5 条）：删除激活 Profile 且代理
        // 运行中 → service 层已停代理并还原 settings.json，proxyStopped 出参
        // 告知这里，toast 明示（否则用户只见「已删除」，不知道 Claude Code
        // 接入配置已被还原）
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
            showToast("已删除", "Profile 已删除（含 Keychain 条目）", false);
        }
    }

    // 批 2：删除共享配置片段（确认弹窗回调）。删除 = 同事务解绑绑定它的
    // profile（服务层已落地 + 激活绑定下的 re-takeover）；这里只负责计数
    // toast 文案 + 关闭编辑弹窗（目标已不存在）
    void doDeleteShared(int64_t id, const std::string& name) {
        ClaudeService* svc = ClaudeService::global();
        if (!svc) return;
        state_.list.dataDirty_ = true;   // 审计 perf [13]：写操作标脏
        // 计数绑定该片段的 profile（事件驱动直查，给 toast「N 个已解绑」）
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
        state_.shared.open_ = false;   // 编辑目标已删除，编辑弹窗一并关闭
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
        // 先收上一轮结果（页面不可见时 compose 不跑，会在这里积累）
        harvestTestResult();
        if (testThread_.joinable()) {
            showToast("测试进行中", "请等待上一轮测试完成", false);
            return;
        }
        // ponytail 升级路径已落地：testConnection 是同步 HTTP（timeout 15s），
        // 在 UI 线程直接调最坏冻结界面 15s → 挪后台线程，立即 toast「测试中」，
        // 结果经原子标志回 compose 每帧收割（模块析构时 join 防悬垂）
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
        });
    }

    // 收割后台测试结果：线程跑完后 join 并弹结果 toast（compose 每帧调）
    void harvestTestResult() {
        if (!testThread_.joinable()) return;
        if (!testDone_.load(std::memory_order_acquire)) return;  // 还在跑
        testThread_.join();
        testDone_.store(false);
        showToast(testResultTitle_, testResultMsg_, testResultIsError_);
    }

    // ============ 状态 ============
    // R4（P-B2）：41 项原成员按区域收进 5 个纯值 struct（claude_ui_state.h）；
    // 顶层只剩「状态容器 + toast + 测试连通后台线程」三组（收口断言 ≤12 项）
    ClaudeUiState state_;

    // Toast（状态位 + 绘制接线统一走 ui_kit，任务 D / R1）
    ui_kit::ToastState toast_;

    // 测试连通后台线程（结果经 testDone_ 原子标志回 compose 收割；
    // 普通成员的发布顺序由 release/acquire + join 保证）
    std::thread testThread_;
    std::atomic<bool> testDone_{false};
    std::string testResultTitle_;
    std::string testResultMsg_;
    bool testResultIsError_ = false;
};

} // namespace module::claude

// ============ 模块工厂 ============
extern "C" ::core::Module* create_claude_module() {
    return new module::claude::ClaudeModule();
}
