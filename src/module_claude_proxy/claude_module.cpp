// src/module_claude_proxy/claude_module.cpp
// Claude 配置切换器模块 - 完整 UI + 操作按钮全量接线（真实 ClaudeService）
//
// 接线清单（W5）：
//   新建/编辑 Profile（dialog 表单）→ createProfile / updateProfile
//   启用 → setActiveProfile；删除（确认 dialog）→ deleteProfile
//   测试连通 → testConnection；代理开关 → startProxy / stopProxy
//   每次操作后标脏 compose 数据缓存，下一帧发一次刷新意图（审计 perf [13]
//   + H2：命中帧零 SQLite，查询全在后台线程跑，渲染线程只读快照）
#include "claude_service.h"
#include "env_var_hints.h"
#include "provider_config.h"
#include "settings_sync.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/theme.h"
#include "core/time.h"
#include "core/component_theme.h"
#include "platform/json.h"
#include <ui_kit/toast.h>

#include <eui/dsl.h>

#include <components/button.h>
#include <components/dialog.h>
#include <components/tooltip.h>
#include <components/dropdown.h>
#include <components/input.h>
#include <components/scrollview.h>
#include <components/tabs.h>
#include <components/theme.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace module::claude {

namespace {

// 格式化毫秒数为 "1234ms" / "1.2s"
std::string formatLatency(int64_t ms) {
    if (ms <= 0) return "-";
    if (ms < 1000) return std::to_string(static_cast<int>(ms)) + "ms";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fs", ms / 1000.0);
    return std::string(buf);
}

// 按字节截断 UTF-8 串（审计 [11]：错误行尾附 error 摘要，过长会挤爆行宽）。
// 截点回退到字符边界，不把多字节字符切成半个（乱码方块）
std::string truncateUtf8(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "…";
}

// 调用记录弹窗状态码过滤（审计 [11]，spec §8.3 第 6 条「支持过滤、搜索」）：
// 下拉索引 → ClaudeService::CallLogQuery::statusClass 映射
constexpr int kCallsStatusClasses[] = {0, 2, 3, 4, 5, 9};
const char* const kCallsStatusLabels[] = {
    "全部状态", "2xx 成功", "3xx 重定向", "4xx 客户端错误",
    "5xx 服务端错误", "错误（超时等）"};
constexpr int kCallsStatusCount = 6;
// 日志弹窗每页条数（审计 [11]；总设计只承诺「过滤、搜索」，条数由 UI 自定）
constexpr int kCallsPageSize = 20;
// 搜索框防抖窗口（审计 CPX-06）：每次按键顺延窗口，停手满一个窗口才派发一次
// 后台查询（窗口内多次按键合并成一次）；250ms = 常见搜索防抖档
constexpr int kCallsSearchDebounceMs = 250;

// 认证字段下拉选项（**顺序必须与 AuthField 枚举声明一致**：Both/Bearer/ApiKey）
constexpr const char* kAuthFieldLabels[] = {
    "双发（默认）", "只发 Authorization: Bearer", "只发 x-api-key"};
constexpr int kAuthFieldCount = 3;

// 模型映射摘要（§6.2 详情面板一行式）：「默认 <model> · 槽位已配 N/M」
// 与分母 M 都取自槽位表（审计 CPX-08：不再手写槽位清单与硬编码 /6）
std::string modelsSummary(const Profile& p) {
    const ModelsConfig mc = effectiveModelsConfig(p.modelsConfigJson, p.model);
    int configured = 0;
    for (const auto& meta : modelSlotTable()) {
        if (!(mc.*meta.slot).model.empty()) ++configured;
    }
    const std::string total = std::to_string(kModelSlotCount);
    if (mc.def.model.empty()) {
        return configured > 0
            ? ("（默认未配）· 槽位已配 " + std::to_string(configured) + "/" + total)
            : std::string("-");
    }
    return "默认 " + mc.def.model + " · 槽位已配 " +
           std::to_string(configured) + "/" + total;
}

// 「导入当前配置」名称建议：BASE_URL host（剥 scheme/路径/端口），
// 提取不到（无 BASE_URL）→「导入 <日期>」；用户可改
std::string suggestImportName(const std::string& baseUrl) {
    size_t start = baseUrl.find("://");
    start = (start == std::string::npos) ? 0 : start + 3;
    size_t end = baseUrl.find('/', start);
    if (end == std::string::npos) end = baseUrl.size();
    std::string host = baseUrl.substr(start, end - start);
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos) host.resize(colon);
    if (!host.empty()) return "导入 " + host;
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    core::time::localTime(t, &tm);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return "导入 " + std::string(buf);
}

} // namespace

// 审计 CPX-06 测试缝计数器（生产零读取点，只在文件末尾经 extern "C" 导出给
// 无头用例；照 core/proxy_stats.h resetForTest / platform/file_open_win.h
// wideToUtf8ForTest 先例）：调用记录后台查询的累计**派发**次数（在派发点由
// 渲染线程自增 → 同一帧即可读，断言不靠时序）+ 最近一次**实际执行**查询的
// 线程 id 哈希（worker 线程写入，0 = 还没跑过）。用例靠它断言「弹窗打开那一帧
// 只发意图（不发生同步查询）」「搜索防抖把逐键的多次意图合并成一次派发」
std::atomic<int> g_callsQueryDispatches{0};
std::atomic<unsigned long long> g_callsQueryDispatchThread{0};

class ClaudeModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::ClaudeProxy; }
    std::string displayName() const override { return "Claude 切换器"; }
    std::string iconGlyph() const override { return eui::utf8(0xF021); }  // fa-arrows-rotate

    ~ClaudeModule() override {
        // 后台测试线程必须先收（最坏阻塞到 HTTP 超时 15s，避免悬垂 this）
        if (testThread_.joinable()) testThread_.join();
        // 审计 H2：后台刷新经 future 回传（工作线程不触碰 this），成员析构
        // 自动等本轮查询结束，无需显式 join（CPX-06 的调用记录查询同款）
    }

    // ============ UI ============

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        const auto& t = core::theme::current();
        const auto themeTokens = core::theme::componentTokens(t);
        const float pad = t.sizes.spacingXXLarge;        // 40
        const float contentW = screen.width - pad * 2.0f;
        const float contentH = screen.height - pad * 2.0f;
        const std::time_t now = std::time(nullptr);   // 调用时间列「今天/昨天」判定
        const float headerH = 64.0f;
        const float gap = 16.0f;
        const float leftW = 400.0f;
        const float rightW = contentW - leftW - gap;
        // 审计 M6：header 右侧操作区宽（标题列 = contentW - 本值）——header 行
        // gap=0，两段必须相加等于 contentW；字面量写死两处改一处忘一处就右缘错位
        constexpr float kHeaderActionsW = 360.0f;

        // ---- 读真实状态（ClaudeService 全局单例，由 main.cpp 注册）----
        ClaudeService* svc = ClaudeService::global();
        const bool proxyRunning = svc ? svc->isProxyRunning() : false;
        const int runningPort = svc ? svc->proxyPort() : 0;
        const std::string portStr = runningPort > 0
            ? (":" + std::to_string(runningPort))
            : std::string("");   // 未运行时不显示假端口（副标题另有提示）

        // 引导条（§6.4）：代理未运行且本会话从未启动过时显示（内存态一次性）
        if (proxyRunning) proxyEverSeen_ = true;
        const bool showGuide = !proxyRunning && !proxyEverSeen_;
        const float bodyH = contentH - headerH - t.sizes.spacingLarge -
                            (showGuide ? 30.0f : 0.0f);

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
            dataDirty_ = true;   // 服务未就绪查不了，保持脏等下帧
        } else {
            refreshDataCache(svc);
            profiles = cachedProfiles_;
            recent = cachedRecent_;
            activeProfile = cachedActive_;
            hasActive = cachedHasActive_;
            profilesError = cachedProfilesError_;
            callsError = cachedCallsError_;
        }

        // 详情面板展示的 profile：优先用户选中的，其次激活的
        const Profile* shown = nullptr;
        for (const auto& p : profiles) {
            if (p.id == selectedId_) { shown = &p; break; }
        }
        if (!shown && hasActive) shown = &activeProfile;

        const std::string nameValue = shown ? shown->name : std::string("(未启用)");
        const std::string modelValue = shown ? modelsSummary(*shown)
                                             : std::string("-");
        const std::string urlValue = shown ? shown->baseUrl : std::string("-");
        const std::string tokenValue = shown
            ? (shown->apiKeyHint + " [Keychain]")
            : std::string("(无激活 profile)");

        ui.column("claude.root")
            .size(screen.width, screen.height)
            .padding(pad)
            .gap(t.sizes.spacingLarge)
            .content([&] {
                // ========== 顶部 Header ==========
                ui.row("claude.header")
                    .size(contentW, headerH)
                    .alignItems(eui::Align::CENTER)
                    .content([&] {
                        // 左侧标题 + 状态
                        ui.column("claude.title.col")
                            .size(contentW - kHeaderActionsW, headerH)
                            .gap(6.0f)
                            .content([&] {
                                ui.row("claude.title.row")
                                    .size(contentW - kHeaderActionsW, 28.0f)
                                    .gap(12.0f)
                                    .alignItems(eui::Align::CENTER)
                                    .content([&] {
                                        ui.text("claude.h1")
                                            .size(220.0f, 32.0f)
                                            .text("Claude 切换器")
                                            .fontSize(t.sizes.fontSizeTitle)
                                            .lineHeight(32.0f)
                                            .fontWeight(700)
                                            .color(t.colors.textPrimary)
                                            .build();

                                        // 状态指示器（真实状态）
                                        const eui::Color dotColor = proxyRunning
                                            ? t.colors.success
                                            : t.colors.textTertiary;
                                        const std::string statusText = proxyRunning
                                            ? std::string("运行中 ") + portStr
                                            : std::string("已停止");
                                        ui.rect("claude.status.dot")
                                            .size(10.0f, 10.0f)
                                            .color(dotColor)
                                            .radius(5.0f)
                                            .build();
                                        ui.text("claude.status.text")
                                            .size(200.0f, 18.0f)
                                            .text(statusText)
                                            .fontSize(t.sizes.fontSizeSmall)
                                            .lineHeight(18.0f)
                                            .color(proxyRunning ? t.colors.success
                                                                : t.colors.textTertiary)
                                            .fontWeight(500)
                                            .build();
                                    })
                                    .build();

                                // 代理未运行时不显示假地址，改为可操作的提示
                                const std::string subtitle = proxyRunning
                                    ? (std::string("管理多套 Claude API 配置 · 本地代理 http://127.0.0.1") +
                                       portStr)
                                    : std::string("管理多套 Claude API 配置 · 代理已停止，点右上角启动");
                                ui.text("claude.subtitle")
                                    .size(contentW - kHeaderActionsW, 16.0f)
                                    .text(subtitle)
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .lineHeight(16.0f)
                                    .color(t.colors.textSecondary)
                                    .build();
                            })
                            .build();

                        // 右侧操作
                        ui.row("claude.actions")
                            .size(kHeaderActionsW, 40.0f)
                            .justifyContent(eui::Align::END)
                            .alignItems(eui::Align::CENTER)
                            .gap(t.sizes.spacingSmall)
                            .content([&] {
                                // 停止/启动代理（真实开关）
                                components::button(ui, "claude.btn.proxy")
                                    .size(110.0f, 40.0f)
                                    .text(proxyRunning ? "停止代理" : "启动代理")
                                    .icon(proxyRunning ? 0xF04C : 0xF04B)  // fa-pause / fa-play
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .colors(t.colors.surfaceElevated, t.colors.hover,
                                            t.colors.active)
                                    .textColor(t.colors.textSecondary)
                                    .iconColor(t.colors.textSecondary)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .onClick([this] { doToggleProxy(); })
                                    .build();
                            })
                            .build();
                    })
                    .build();

                // 引导条（§6.4）：代理未运行且本会话从未启动过时一次性提示接管语义
                //（ⓘ 换 FA：Icon 字体不能与正文混排，图标/文字拆两个元素）
                if (showGuide) {
                    ui.row("claude.guide.wrap")
                        .size(contentW, 18.0f)
                        .gap(6.0f)
                        .alignItems(eui::Align::CENTER)
                        .content([&] {
                            ui.text("claude.guide.icon")
                                .size(14.0f, 18.0f)
                                .icon(0xF05A)  // fa-circle-info
                                .fontSize(t.sizes.fontSizeSmall)
                                .lineHeight(18.0f)
                                .color(t.colors.textSecondary)
                                .build();
                            ui.text("claude.guide.takeover")
                                .size(contentW - 20.0f, 18.0f)
                                .text("启动代理将接管 Claude Code 配置（" +
                                      settings_sync::defaultPaths().settingsFile.string() +
                                      " 指向本地代理），停止代理时自动还原")
                                .fontSize(t.sizes.fontSizeSmall)
                                .lineHeight(18.0f)
                                .color(t.colors.textSecondary)
                                .build();
                        })
                        .build();
                }

                // ========== 主区: Profile 列表 + 详情 ==========
                ui.row("claude.body")
                    .size(contentW, bodyH)
                    .gap(gap)
                    .content([&] {
                        // ---- 左栏：Profile 列表 ----
                        ui.column("claude.profiles")
                            .size(leftW, bodyH)
                            .gap(0.0f)
                            .content([&] {
                                ui.rect("claude.profiles.bg")
                                    .size(leftW, bodyH)
                                    // 左栏 Profile 列表 = 结构性面板 → background
                                    .color(t.colors.background)
                                    .radius(t.sizes.radiusLarge)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                            })
                            .build();

                        ui.stack("claude.profiles.overlay")
                            .size(leftW, bodyH)
                            .ignoreLayout()  // overlay 不参与 row 流式布局，覆盖在 profiles 列上
                            .content([&] {
                                // 顶部栏
                                ui.text("claude.profiles.title")
                                    .position(20.0f, 20.0f)
                                    .size(leftW - 40.0f, 20.0f)
                                    .text("Profile 列表")
                                    .fontSize(t.sizes.fontSizeBody)
                                    .lineHeight(20.0f)
                                    .fontWeight(600)
                                    .color(t.colors.textPrimary)
                                    .build();

                                // 卡片列表（真实数据；空时显示占位）
                                const float cardY0 = 56.0f;
                                const float cardGap = 12.0f;
                                if (!profilesError.empty()) {
                                    ui.text("claude.profiles.error")
                                        .position(20.0f, cardY0 + 20.0f)
                                        .size(leftW - 40.0f, 60.0f)
                                        .text("Profile 列表加载失败：" + profilesError)
                                        .fontSize(12.0f)
                                        .lineHeight(20.0f)
                                        .color(t.colors.error)
                                        .build();
                                } else if (profiles.empty()) {
                                    ui.text("claude.profiles.empty")
                                        .position(20.0f, cardY0 + 20.0f)
                                        .size(leftW - 40.0f, 60.0f)
                                        .text("暂无 Profile，点击下方按钮新建")
                                        .fontSize(12.0f)
                                        .lineHeight(20.0f)
                                        .color(t.colors.textTertiary)
                                        .build();
                                } else {
                                    // 卡片列表套 scrollView（产品审计 2026-09-23
                                    // [26]）：此前固定 y=56+i*108 平铺，profile 数
                                    // 一多就画出左栏边界、叠在底部按钮上且无法滚
                                    // 动。仿 Todo 列内滚动 / settings 卡片区。
                                    // 滚动区底 = 底部按钮（bodyH-64）上方 12px
                                    const float scrollH = std::max(
                                        0.0f, bodyH - 64.0f - 12.0f - cardY0);
                                    // contentKey = profile id 序列 hash（仿 md.tree，
                                    // W7 教训：数量相同但内容替换也要失效重测）
                                    size_t listKey = 0;
                                    for (const auto& p : profiles) {
                                        listKey = listKey * 1315423911u +
                                                  static_cast<size_t>(p.id);
                                    }
                                    components::scrollView(ui, "claude.profiles.scroll")
                                        .position(0.0f, cardY0)
                                        .size(leftW - 24.0f, scrollH)
                                        .gap(cardGap)
                                        .step(96.0f + cardGap)
                                        .theme(themeTokens)
                                        .contentKey("claude.profiles.scroll." +
                                                    std::to_string(listKey))
                                        .offset(profilesScrollOffset_)
                                        .onChange([this](float v) {
                                            profilesScrollOffset_ = v;
                                        })
                                        .content([&](eui::Ui& inner, float contentWidth,
                                                     float viewportHeight) {
                                            (void)viewportHeight;
                                            for (size_t i = 0; i < profiles.size(); ++i) {
                                                const std::string cardId =
                                                    "claude.profile." + std::to_string(i);
                                                // 行包 stack（position 只在 stack 内
                                                // 生效，仿 todo 列卡片）
                                                inner.stack(cardId + ".wrap")
                                                    .size(contentWidth, 96.0f)
                                                    .content([&] {
                                                        drawProfileCard(
                                                            inner, cardId, 0.0f,
                                                            contentWidth, t, profiles[i]);
                                                    })
                                                    .build();
                                            }
                                        })
                                        .build();
                                }

                                // 底部按钮：新建 + 导入当前配置（读
                                // ~/.claude/settings.json 预填新建表单）
                                const float btnW = (leftW - 40.0f - 12.0f) * 0.5f;
                                components::button(ui, "claude.profiles.new")
                                    .position(20.0f, bodyH - 64.0f)
                                    .size(btnW, 40.0f)
                                    .text("+ 新建 Profile")
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .colors(t.colors.surfaceElevated, t.colors.hover,
                                            t.colors.active)
                                    .textColor(t.colors.textPrimary)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .onClick([this] { openCreateForm(); })
                                    .build();
                                components::button(ui, "claude.profiles.import")
                                    .position(20.0f + btnW + 12.0f, bodyH - 64.0f)
                                    .size(btnW, 40.0f)
                                    .text("导入当前配置")
                                    .icon(0xF019)  // fa-download
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .colors(t.colors.surfaceElevated, t.colors.hover,
                                            t.colors.active)
                                    .textColor(t.colors.textPrimary)
                                    .iconColor(t.colors.textPrimary)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .onClick([this] { doImportCurrentConfig(); })
                                    .build();
                            })
                            .build();

                        // ---- 右栏：详情面板 + 最近调用 ----
                        ui.column("claude.detail")
                            .size(rightW, bodyH)
                            .gap(0.0f)
                            .content([&] {
                                ui.rect("claude.detail.bg")
                                    .size(rightW, bodyH)
                                    // 右栏详情面板 = 结构性面板 → background
                                    .color(t.colors.background)
                                    .radius(t.sizes.radiusLarge)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                            })
                            .build();

                        ui.stack("claude.detail.overlay")
                            .size(rightW, bodyH)
                            .ignoreLayout()  // overlay 不参与 row 流式布局
                            .position(leftW + gap, 0.0f)  // 对齐右栏 detail 列位置
                            .content([&] {
                                const float x0 = 28.0f;
                                const float y0 = 24.0f;
                                const float w0 = rightW - 56.0f;

                                // 标题
                                ui.text("claude.detail.title")
                                    .position(x0, y0)
                                    .size(w0, 24.0f)
                                    .text("当前 Profile 详情")
                                    .fontSize(t.sizes.fontSizeLarge)
                                    .lineHeight(24.0f)
                                    .fontWeight(600)
                                    .color(t.colors.textPrimary)
                                    .build();

                                // 页签栏（2026-09-25：改用 EUI-NEO tabs 组件，自带动画指示器+
                                // 底部分隔线+居中文字；色源走 componentTokens 桥接。tabs 无
                                // .position 故包一层定位 stack 对齐右栏内边距，沿用 settings
                                // 模块 .wrap stack 同款写法）
                                const float tabY = y0 + 40.0f;
                                const float tabH = 36.0f;
                                ui.stack("claude.detail.tabs.wrap")
                                    .position(x0, tabY)
                                    .size(w0, tabH)
                                    .content([&] {
                                        components::tabs(ui, "claude.detail.tabs")
                                            .size(w0, tabH)
                                            .items(std::vector<std::string>{
                                                "基础信息", "模型映射", "环境变量"})
                                            .selected(detailTab_)
                                            .fontSize(t.sizes.fontSizeSmall)
                                            .theme(themeTokens)
                                            .onChange([this](int idx) { detailTab_ = idx; })
                                            .build();
                                    });

                                const float paneY = tabY + tabH + 12.0f;
                                float paneBottom = paneY;  // 各 pane 累加真实底沿，供 actions 定位防重叠
                                if (detailTab_ == 0) {
                                    // ---- 基础信息 pane：名称 + 未激活徽章 + Base URL + API Key + 接管 ----
                                    ui.text("claude.field.name.label")
                                        .position(x0, paneY).size(w0 * 0.5f - 8.0f, 14.0f)
                                        .text("名称").fontSize(11.0f).lineHeight(14.0f)
                                        .color(t.colors.textTertiary).build();
                                    ui.rect("claude.field.name.bg")
                                        .position(x0, paneY + 18.0f).size(w0 * 0.5f - 8.0f, 24.0f)
                                        .color(t.colors.surfaceElevated).radius(t.sizes.radiusSmall)
                                        .border(1.0f, t.colors.borderMuted).build();
                                    ui.text("claude.field.name.value")
                                        .position(x0 + 12.0f, paneY + 18.0f)
                                        .size(w0 * 0.5f - 20.0f, 24.0f)
                                        .text(nameValue).fontSize(14.0f).lineHeight(24.0f)
                                        .color(t.colors.textPrimary).build();
                                    if (shown && !shown->isActive) {
                                        ui.rect("claude.field.name.inactive.bg")
                                            .position(x0 + w0 * 0.5f - 72.0f, paneY + 4.0f)
                                            .size(56.0f, 16.0f).color(t.colors.surfaceElevated)
                                            .radius(t.sizes.radiusPill)
                                            .border(1.0f, t.colors.warning).build();
                                        ui.text("claude.field.name.inactive")
                                            .position(x0 + w0 * 0.5f - 72.0f, paneY + 4.0f)
                                            .size(56.0f, 16.0f).text("未激活")
                                            .fontSize(10.0f).lineHeight(16.0f)
                                            .horizontalAlign(eui::HorizontalAlign::Center)
                                            .color(t.colors.warning).build();
                                    }
                                    const float urlY = paneY + 56.0f;
                                    ui.text("claude.field.url.label")
                                        .position(x0, urlY).size(w0, 14.0f)
                                        .text("Base URL").fontSize(11.0f).lineHeight(14.0f)
                                        .color(t.colors.textTertiary).build();
                                    ui.rect("claude.field.url.bg")
                                        .position(x0, urlY + 18.0f).size(w0, 28.0f)
                                        .color(t.colors.surfaceElevated).radius(t.sizes.radiusSmall)
                                        .border(1.0f, t.colors.borderMuted).build();
                                    ui.text("claude.field.url.value")
                                        .position(x0 + 12.0f, urlY + 18.0f).size(w0 - 24.0f, 28.0f)
                                        .text(urlValue).fontSize(t.sizes.fontSizeSmall).lineHeight(28.0f)
                                        .color(t.colors.textPrimary).build();
                                    const float keyY = urlY + 56.0f;
                                    ui.text("claude.field.token.label")
                                        .position(x0, keyY).size(w0, 14.0f)
                                        .text("API Key").fontSize(11.0f).lineHeight(14.0f)
                                        .color(t.colors.textTertiary).build();
                                    ui.rect("claude.field.token.bg")
                                        .position(x0, keyY + 18.0f).size(w0, 32.0f)
                                        .color(t.colors.surfaceElevated).radius(t.sizes.radiusSmall)
                                        .border(1.0f, t.colors.borderMuted).build();
                                    ui.text("claude.field.token.value")
                                        .position(x0 + 12.0f, keyY + 18.0f).size(w0 - 24.0f, 32.0f)
                                        .text(tokenValue).fontSize(t.sizes.fontSizeSmall).lineHeight(32.0f)
                                        .color(t.colors.textSecondary).build();
                                    const bool ccManaged = svc && svc->isClaudeCodeManaged();
                                    // 审计 CPX-02：接管失败（无 sidecar → 未接管）与上层覆盖
                                    // 警告都要让用户看见，故不再以 ccManaged 为展示门。读的是
                                    // 内存 getter（无 IO/DB，符合 H2），启停代理 / 切 profile
                                    // 后下一帧即刷新
                                    const auto& ccWarnings = svc ? svc->lastSyncWarnings()
                                                                : std::vector<std::string>();
                                    const std::string ccPath =
                                        settings_sync::defaultPaths().settingsFile.string();
                                    const float toY = keyY + 56.0f;
                                    ui.text("claude.field.takeover.value")
                                        .position(x0, toY).size(w0, 16.0f)
                                        .text(ccManaged ? ("● Claude Code 已接管 " + ccPath)
                                                        : std::string("○ Claude Code 未接管（启动代理后自动接管，停止时还原）"))
                                        .fontSize(11.0f).lineHeight(16.0f)
                                        .color(ccManaged ? t.colors.success : t.colors.textTertiary).build();
                                    if (!ccWarnings.empty()) {
                                        // 摘要 = 首条警告（失败路径只有一条 = 失败原因）
                                        // + 总条数；「详见设置页」恒成立——设置页的警告行
                                        // 已改为只看警告（不再按 managed 门控，见
                                        // settings_module.cpp 同审计项），未接管态下
                                        // 接管失败原因同样列在那里
                                        std::string warnText = ccWarnings.front();
                                        if (ccWarnings.size() > 1) {
                                            warnText += "（共 " +
                                                std::to_string(ccWarnings.size()) + " 条";
                                            warnText += "，详见设置页）";
                                        }
                                        ui.text("claude.field.takeover.warn.icon")
                                            .position(x0, toY + 16.0f).size(14.0f, 16.0f).icon(0xF071)
                                            .fontSize(11.0f).lineHeight(16.0f).color(t.colors.warning).build();
                                        ui.text("claude.field.takeover.warn")
                                            .position(x0 + 18.0f, toY + 16.0f).size(w0 - 18.0f, 16.0f)
                                            .text(warnText)
                                            .fontSize(11.0f).lineHeight(16.0f).color(t.colors.warning).build();
                                    }
                                    paneBottom = toY + (!ccWarnings.empty() ? 32.0f : 16.0f);
                                } else if (detailTab_ == 1) {
                                    // ---- 模型映射 pane：槽位逐行（遍历槽位表派生；widget id 序号 = 表序）----
                                    ModelsConfig mc{};
                                    if (shown && !shown->modelsConfigJson.empty()) {
                                        auto mr = parseModelsConfig(shown->modelsConfigJson);
                                        if (mr.isOk()) mc = mr.value();
                                    }
                                    if (mc.def.model.empty() && shown && !shown->model.empty())
                                        mc.def.model = shown->model;  // 旧列 fallback
                                    float sy = paneY;
                                    int slotIdx = 0;
                                    for (const auto& meta : modelSlotTable()) {
                                        const ModelSlot& slot = mc.*meta.slot;
                                        const bool filled = !slot.model.empty();
                                        const std::string idx = std::to_string(slotIdx++);
                                        ui.rect("claude.slot.bg." + idx)
                                            .position(x0, sy).size(w0, 32.0f)
                                            .color(t.colors.surfaceElevated).radius(t.sizes.radiusSmall)
                                            .border(1.0f, t.colors.borderMuted).build();
                                        ui.text("claude.slot.name." + idx)
                                            .position(x0 + 12.0f, sy).size(80.0f, 32.0f)
                                            .text(meta.label).fontSize(12.0f).lineHeight(32.0f)
                                            .color(t.colors.textTertiary).build();
                                        ui.text("claude.slot.val." + idx)
                                            .position(x0 + 100.0f, sy).size(w0 - 112.0f, 32.0f)
                                            .text(filled ? slot.model : std::string("未配置"))
                                            .fontSize(13.0f).lineHeight(32.0f)
                                            .color(filled ? t.colors.textPrimary : t.colors.textTertiary).build();
                                        sy += 38.0f;
                                    }
                                    paneBottom = sy;
                                } else {
                                    // ---- 环境变量 pane：键（截断）+ 值（脱敏）同列不重叠；下方官方说明；未知键红框 ----
                                    std::map<std::string, std::string> envMap;
                                    if (shown && !shown->extraEnvJson.empty()) {
                                        auto er = parseExtraEnv(shown->extraEnvJson);
                                        if (er.isOk()) envMap = er.value();
                                    }
                                    if (envMap.empty()) {
                                        ui.text("claude.env.empty")
                                            .position(x0, paneY).size(w0, 20.0f)
                                            .text("无额外环境变量").fontSize(12.0f).lineHeight(20.0f)
                                            .color(t.colors.textTertiary).build();
                                        paneBottom = paneY + 20.0f;
                                    } else {
                                        const float keyColW = std::min(220.0f, w0 * 0.45f);
                                        const int keyMaxChars = std::max(6, static_cast<int>(keyColW / 7.0f));
                                        // 行数上限（2026-09-27 修）：此前无上限，行一多 paneBottom
                                        // 一路下探，把下方按钮组/最近调用表/「查看全部」顶出面板
                                        // 下沿，被 app.page clip 裁掉点不到。下方固定占用 236px
                                        //（actY = paneBottom+20，查看全部命中区底 = actY+236）+
                                        // 20px「另有 N 项」提示；超出部分不画只提示（不引 scrollView：
                                        // 行绘制用 overlay 绝对坐标，进 content lambda 需整段改相对
                                        // 坐标 + 新增滚动 offset 成员，改动面大；表单 env 编辑器
                                        // 已有 kMaxVisibleRows + 溢出提示同款先例）
                                        const size_t envMaxRows = std::max<size_t>(
                                            1, static_cast<size_t>(
                                                   std::max(0.0f, bodyH - paneY - 276.0f) / 54.0f));
                                        float ey = paneY;
                                        int ei2 = 0;
                                        for (const auto& kv : envMap) {
                                            if (static_cast<size_t>(ei2) >= envMaxRows) break;
                                            const std::string& k = kv.first;
                                            const std::string& v = kv.second;
                                            std::string shownVal = v;
                                            {
                                                std::string kl;
                                                kl.reserve(k.size());
                                                for (char c : k)
                                                    kl += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                                bool sensitive = kl.find("token") != std::string::npos
                                                    || kl.find("key") != std::string::npos
                                                    || kl.find("secret") != std::string::npos;
                                                if (sensitive && v.size() > 12)
                                                    shownVal = v.substr(0, 8) + "…";
                                            }
                                            // 键名过长则截断，避免压到右侧值列
                                            std::string kDisp = k;
                                            if (static_cast<int>(kDisp.size()) > keyMaxChars)
                                                kDisp = kDisp.substr(0, keyMaxChars - 1) + "…";
                                            const char* hint = envVarHint(k);
                                            const std::string rid = std::to_string(ei2);
                                            const float valX = x0 + 12.0f + keyColW + 24.0f;  // 键列↔值列间距 24（2026-09-25：12→24，反馈太挤）
                                            const float valW = std::max(40.0f, w0 - keyColW - 48.0f);
                                            ui.rect(std::string("claude.env.row.bg.") + rid)
                                                .position(x0, ey).size(w0, 48.0f)
                                                .color(t.colors.surfaceElevated).radius(t.sizes.radiusSmall)
                                                .border(1.0f, hint ? t.colors.borderMuted : t.colors.error)
                                                .build();
                                            ui.text(std::string("claude.env.key.") + rid)
                                                .position(x0 + 12.0f, ey).size(keyColW, 20.0f)
                                                .text(kDisp).fontSize(12.0f).lineHeight(20.0f)
                                                .color(t.colors.primary).build();
                                            // 值列单行：超宽（最小窗 valW≈194px）裁掉，
                                            // 不溢出面板/压到右栏外（clip = 自身 frame 取景）
                                            ui.text(std::string("claude.env.val.") + rid)
                                                .position(valX, ey).size(valW, 48.0f)
                                                .clip()
                                                .text(shownVal).fontSize(12.0f).lineHeight(20.0f)
                                                .verticalAlign(eui::VerticalAlign::Center)
                                                .color(t.colors.textSecondary).build();
                                            ui.text(std::string("claude.env.hint.") + rid)
                                                .position(x0 + 12.0f, ey + 24.0f).size(w0 - 24.0f, 18.0f)
                                                .text(hint ? hint : "（未收录官方清单）")
                                                .fontSize(11.0f).lineHeight(18.0f)
                                                .color(t.colors.textTertiary).build();
                                            ey += 54.0f;
                                            ++ei2;
                                        }
                                        if (envMap.size() > static_cast<size_t>(ei2)) {
                                            // 溢出提示（与表单 env 页「另有 N 项」同款口径）：
                                            // 未画出的行全量保存在 Profile 里，编辑时可见
                                            ui.text("claude.env.overflow")
                                                .position(x0, ey).size(w0, 18.0f)
                                                .text("另有 " +
                                                      std::to_string(envMap.size() -
                                                                     static_cast<size_t>(ei2)) +
                                                      " 项未显示（编辑该 Profile 可查看全部）")
                                                .fontSize(11.0f).lineHeight(18.0f)
                                                .color(t.colors.textTertiary).build();
                                            ey += 20.0f;
                                        }
                                        paneBottom = ey;
                                    }
                                }
                                // 操作按钮组（全量接线）：actY 跟随各 pane 真实底沿，避免与上方内容重叠
                                const float actY = paneBottom + 20.0f;
                                ui.rect("claude.act.divider")
                                    .position(x0, actY - 10.0f)
                                    .size(w0, 1.0f)
                                    .color(t.colors.borderDefault).build();
                                const float actGap = 12.0f;
                                // 6 键均分（设计 §4.1，M5 + profile-run：启用/测试连通/
                                // 编辑/删除/复制/运行）——按钮数改一处，gap 数也要跟着改。
                                // 扣 **5** 个 gap：EUI 的 row spacing 只在相邻子项之间生效，
                                // n 项只有 n−1 个间隙；旧 5 键式 (w0-actGap*4)/5 恰好齐右缘。
                                const float actW = (w0 - actGap * 5.0f) / 6.0f;
                                ui.row("claude.detail.actions")
                                    .position(x0, actY)
                                    .size(w0, 36.0f)
                                    .gap(actGap)
                                    .content([&] {
                                        const int64_t shownId = shown ? shown->id : 0;
                                        const Profile shownCopy = shown
                                            ? *shown : Profile{};

                                        // 启用（设为激活）
                                        components::button(ui, "claude.act.enable")
                                            .size(actW, 36.0f)
                                            .text("启用")
                                            .fontSize(t.sizes.fontSizeSmall)
                                            .colors(t.colors.primary,
                                                    t.colors.primaryHover,
                                                    t.colors.primaryPressed)
                                            .textColor(t.colors.onPrimary)
                                            .radius(t.sizes.radius)
                                            .onClick([this, shownId] {
                                                if (shownId == 0) {
                                                    showToast("启用失败",
                                                              "请先新建或选择一个 Profile",
                                                              true);
                                                    return;
                                                }
                                                doSetActive(shownId);
                                            })
                                            .build();
                                        // 测试连通
                                        components::button(ui, "claude.act.test")
                                            .size(actW, 36.0f)
                                            .text("测试连通")
                                            .fontSize(t.sizes.fontSizeSmall)
                                            .colors(t.colors.surfaceElevated,
                                                    t.colors.hover, t.colors.active)
                                            .textColor(t.colors.textPrimary)
                                            .radius(t.sizes.radius)
                                            .border(1.0f, t.colors.borderDefault)
                                            .onClick([this, shownCopy] {
                                                if (shownCopy.id == 0) {
                                                    showToast("测试连通失败",
                                                              "请先新建或选择一个 Profile",
                                                              true);
                                                    return;
                                                }
                                                doTestConnection(shownCopy);
                                            })
                                            .build();
                                        // 编辑（预填现有值）
                                        components::button(ui, "claude.act.edit")
                                            .size(actW, 36.0f)
                                            .text("编辑")
                                            .fontSize(t.sizes.fontSizeSmall)
                                            .colors(t.colors.surfaceElevated,
                                                    t.colors.hover, t.colors.active)
                                            .textColor(t.colors.textPrimary)
                                            .radius(t.sizes.radius)
                                            .border(1.0f, t.colors.borderDefault)
                                            .onClick([this, shownCopy] {
                                                if (shownCopy.id == 0) {
                                                    showToast("编辑失败",
                                                              "请先新建或选择一个 Profile",
                                                              true);
                                                    return;
                                                }
                                                openEditForm(shownCopy);
                                            })
                                            .build();
                                        // 删除（确认 dialog）
                                        components::button(ui, "claude.act.del")
                                            .size(actW, 36.0f)
                                            .text("删除")
                                            .fontSize(t.sizes.fontSizeSmall)
                                            .colors(t.colors.surfaceElevated,
                                                    t.colors.hover, t.colors.active)
                                            .textColor(t.colors.error)
                                            .radius(t.sizes.radius)
                                            .border(1.0f, t.colors.borderDefault)
                                            .onClick([this, shownCopy] {
                                                if (shownCopy.id == 0) {
                                                    showToast("删除失败",
                                                              "请先新建或选择一个 Profile",
                                                              true);
                                                    return;
                                                }
                                                deleteTargetId_ = shownCopy.id;
                                                deleteTargetName_ = shownCopy.name;
                                                deleteConfirmOpen_ = true;
                                            })
                                            .build();
                                        // 复制（第 5 键，设计 §4.1/§4.2）：副本不激活
                                        components::button(ui, "claude.act.dup")
                                            .size(actW, 36.0f)
                                            .text("复制")
                                            .fontSize(t.sizes.fontSizeSmall)
                                            .colors(t.colors.surfaceElevated,
                                                    t.colors.hover, t.colors.active)
                                            .textColor(t.colors.textPrimary)
                                            .radius(t.sizes.radius)
                                            .border(1.0f, t.colors.borderDefault)
                                            .onClick([this, shownId] {
                                                if (shownId == 0) {
                                                    showToast("复制失败",
                                                              "请先选择 Profile",
                                                              true);
                                                    return;
                                                }
                                                doDuplicate(shownId);
                                            })
                                            .build();
                                        // 运行（第 6 键，2026-09-28 profile-run 设计
                                        // D-1/D-2/D-4）：在新终端里用该 Profile 打开
                                        // 一个 claude 会话
                                        components::button(ui, "claude.act.run")
                                            .size(actW, 36.0f)
                                            .text("运行")
                                            .fontSize(t.sizes.fontSizeSmall)
                                            .colors(t.colors.surfaceElevated,
                                                    t.colors.hover, t.colors.active)
                                            .textColor(t.colors.textPrimary)
                                            .radius(t.sizes.radius)
                                            .border(1.0f, t.colors.borderDefault)
                                            .onClick([this, shownId] {
                                                if (shownId == 0) {
                                                    showToast("运行失败",
                                                              "请先选择 Profile",
                                                              true);
                                                    return;
                                                }
                                                doRun(shownId);
                                            })
                                            .build();
                                    })
                                    .build();

                                // 「运行」副提示（设计 §6：D-1 副作用必须明示；§3 措辞
                                // 按「选中 ≠ 激活才切」表述，避免用户以为每次都改全局）
                                ui.text("claude.act.run.hint")
                                    .position(x0, actY + 40.0f)
                                    .size(w0, 16.0f)
                                    .text("以该 Profile 打开新的 Claude Code 会话"
                                          "（起始目录 = 用户主目录）；未激活会先切为激活")
                                    .fontSize(11.0f)
                                    .lineHeight(16.0f)
                                    .color(t.colors.textTertiary)
                                    .build();

                                // 分隔线
                                ui.rect("claude.divider")
                                    .position(x0, actY + 60.0f)
                                    .size(w0, 1.0f)
                                    .color(t.colors.borderMuted)
                                    .build();

                                // 最近调用
                                ui.text("claude.calls.title")
                                    .position(x0, actY + 76.0f)
                                    .size(w0, 20.0f)
                                    .text("最近调用")
                                    .fontSize(t.sizes.fontSizeBody)
                                    .lineHeight(20.0f)
                                    .fontWeight(600)
                                    .color(t.colors.textPrimary)
                                    .build();

                                // 调用表头
                                const float tableY = actY + 108.0f;
                                const float colTimeX = x0;
                                const float colPathX = x0 + 80.0f;
                                const float colStatusX = x0 + w0 - 200.0f;
                                const float colDurX = x0 + w0 - 100.0f;
                                ui.text("claude.calls.h.time")
                                    .position(colTimeX, tableY)
                                    .size(72.0f, 14.0f)
                                    .text("时间")
                                    .fontSize(10.0f)
                                    .lineHeight(14.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                                ui.text("claude.calls.h.path")
                                    .position(colPathX, tableY)
                                    .size(w0 - 280.0f, 14.0f)
                                    .text("方法 / 路径")
                                    .fontSize(10.0f)
                                    .lineHeight(14.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                                ui.text("claude.calls.h.status")
                                    .position(colStatusX, tableY)
                                    .size(60.0f, 14.0f)
                                    .text("状态")
                                    .fontSize(10.0f)
                                    .lineHeight(14.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                                ui.text("claude.calls.h.dur")
                                    .position(colDurX, tableY)
                                    .size(60.0f, 14.0f)
                                    .text("耗时")
                                    .fontSize(10.0f)
                                    .lineHeight(14.0f)
                                    .color(t.colors.textTertiary)
                                    .build();

                                // 最近调用（真实数据，从 claude_call_logs 读）
                                if (!callsError.empty()) {
                                    ui.text("claude.calls.error")
                                        .position(colTimeX, tableY + 24.0f)
                                        .size(w0 - 80.0f, 16.0f)
                                        .text("调用记录加载失败：" + callsError)
                                        .fontSize(11.0f)
                                        .lineHeight(16.0f)
                                        .color(t.colors.error)
                                        .build();
                                } else if (recent.empty()) {
                                    ui.text("claude.calls.empty")
                                        .position(colTimeX, tableY + 24.0f)
                                        .size(w0 - 80.0f, 16.0f)
                                        .text("(暂无调用记录)")
                                        .fontSize(11.0f)
                                        .lineHeight(16.0f)
                                        .color(t.colors.textTertiary)
                                        .build();
                                } else {
                                    for (size_t i = 0; i < recent.size() && i < 3; ++i) {
                                        const ClaudeService::CallLogEntry& c = recent[i];
                                        const float rowY = tableY + 24.0f +
                                            static_cast<float>(i) * 24.0f;

                                        ui.text("claude.calls.t." + std::to_string(i))
                                            .position(colTimeX, rowY)
                                            .size(72.0f, 16.0f)
                                            .text(formatCallTime(c.startedAt, now))
                                            .fontSize(11.0f)
                                            .lineHeight(16.0f)
                                            .color(t.colors.textPrimary)
                                            .build();

                                        const std::string mp = c.method + " " + c.path;
                                        ui.text("claude.calls.p." + std::to_string(i))
                                            .position(colPathX, rowY)
                                            .size(w0 - 280.0f, 16.0f)
                                            .text(mp)
                                            .fontSize(11.0f)
                                            .lineHeight(16.0f)
                                            .color(t.colors.textPrimary)
                                            .build();

                                        const eui::Color statusColor =
                                            (c.statusCode >= 200 && c.statusCode < 300)
                                                ? t.colors.success :
                                            c.statusCode == 429
                                                ? t.colors.warning
                                                : t.colors.error;
                                        const std::string statusStr = c.statusCode > 0
                                            ? std::to_string(c.statusCode)
                                            : std::string("-");
                                        ui.text("claude.calls.s." + std::to_string(i))
                                            .position(colStatusX, rowY)
                                            .size(60.0f, 16.0f)
                                            .text(statusStr)
                                            .fontSize(11.0f)
                                            .lineHeight(16.0f)
                                            .color(statusColor)
                                            .build();

                                        ui.text("claude.calls.d." + std::to_string(i))
                                            .position(colDurX, rowY)
                                            .size(60.0f, 16.0f)
                                            .text(formatLatency(c.latencyMs))
                                            .fontSize(11.0f)
                                            .lineHeight(16.0f)
                                            .color(t.colors.textSecondary)
                                            .build();
                                    }
                                }

                                // 查看全部 →（可点击：弹完整调用记录）
                                ui.rect("claude.calls.viewall.hit")
                                    .position(x0, tableY + 104.0f)
                                    .size(w0, 24.0f)
                                    // 命中区底 = 所在详情面板底（background）
                                    .color(t.colors.background)
                                    .hoverColor(t.colors.hover)
                                    .radius(t.sizes.radiusSmall)
                                    .onClick([this] { openCallsDialog(); })
                                    .build();
                                ui.text("claude.calls.viewall")
                                    .position(x0, tableY + 108.0f)
                                    .size(w0, 18.0f)
                                    .text("查看全部 →")
                                    .fontSize(12.0f)
                                    .lineHeight(18.0f)
                                    .color(t.colors.primary)
                                    .build();
                            })
                            .build();
                    })
                    .build();
            })
            .build();

        // 弹窗（画在最上层）
        drawProfileFormDialog(ui, screen, t, themeTokens);
        drawDeleteConfirmDialog(ui, screen, t, themeTokens);
        drawCallsDialog(ui, screen, t, themeTokens);
        harvestTestResult();  // 后台「测试连通」结果回 UI（每帧轮询）
        drawToast(ui, screen, themeTokens);

        LOG_DEBUG("Claude 模块 compose 完成");
    }

    void onStart() override {
        LOG_INFO("Claude 模块启动");
        ClaudeService* svc = ClaudeService::global();
        if (!svc) {
            LOG_WARN("onStart: ClaudeService::global() 为 null");
            dataDirty_ = true;   // 服务未就绪查不了：保持脏，进页后由后台刷新补上
            return;
        }
        // 审计 H2：首次装载同步做一次（onStart = 启动期，非渲染路径）。compose
        // 恒读快照，首帧必须有值——异步首刷会让页面先闪一帧空列表（硬约束：
        // 不得空帧/闪空）；此后每次刷新都走 refreshDataCache 的后台线程
        applyDataRefresh(runDataRefresh(svc, true, true));
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
    void onPageEnter() override { dataDirty_ = true; }

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
            formOpen_ || deleteConfirmOpen_ || callsDialogOpen_;
        if (key.key == eui::InputKey::N) {
            if (anyDialogOpen) return false;
            openCreateForm();
            LOG_INFO("快捷键 Ctrl+N：打开新建 Profile 表单");
            return true;
        }
        if (key.key == eui::InputKey::S) {
            if (!formOpen_) return false;
            submitForm();
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
    // 审计 M5：Esc 关最上层弹窗（顺序 = 绘制序/z 序倒序：调用记录 > 删除确认
    // > 表单）。返回 true = 本键已消费；无弹窗返回 false
    bool closeTopmostDialog() {
        if (callsDialogOpen_) { callsDialogOpen_ = false; return true; }
        if (deleteConfirmOpen_) { deleteConfirmOpen_ = false; return true; }
        if (formOpen_) { formOpen_ = false; return true; }
        return false;
    }

    // ============ 审计 perf [13] + H2：compose 只读快照，刷新意图走后台 ============
    // 旧实现（perf [13]）把三类查询从「每帧」收敛到「脏帧」，但查询仍同步跑在
    // 渲染线程的 compose 里——与代理 worker 的 logCall INSERT 争同一把
    // recursive_mutex（事务全程持锁，见 core/sqlite_db.cpp），代理忙时整帧被
    // 卡住。现在查询全部挪到后台线程（std::async），渲染线程只做两件无 IO 的
    // 事：搬已就绪结果进快照 + 发下一次刷新意图。
    // 工作线程产物：一次刷新的三类数据（不触碰 this——结果经 future 值回渲染
    // 线程再落成员）。wantProfiles/wantRecent 保留旧语义：脏标记刷三类，
    // 纯版本轮询只刷 recent（不重复查没变的 profile 列表）。
    struct DataRefresh {
        bool wantProfiles = false;
        bool wantRecent = false;
        std::vector<Profile> profiles;
        std::string profilesError;
        Profile active;
        bool hasActive = false;
        std::vector<ClaudeService::CallLogEntry> recent;
        bool recentOk = false;
        std::string callsError;
        uint64_t verBefore = 0;
    };

    // 唯一跑在后台线程的 DB 入口（static：不碰成员，可安全脱离渲染线程）
    static DataRefresh runDataRefresh(ClaudeService* svc, bool wantProfiles,
                                      bool wantRecent) {
        DataRefresh r;
        r.wantProfiles = wantProfiles;
        r.wantRecent = wantRecent;
        if (wantProfiles) {
            auto listR = svc->listProfiles();
            if (listR.isOk()) {
                r.profiles = std::move(listR).value();
            } else {
                r.profilesError = listR.errorMessage();
            }
            // 激活查询失败静默按无激活（旧语义不变）
            auto aR = svc->getActiveProfile();
            if (aR.isOk()) {
                r.active = std::move(aR).value();
                r.hasActive = true;
            }
        }
        if (wantRecent) {
            // 版本号取**查询前**快照（同旧实现）：查询期间落库的调用会让下一帧
            // 版本再对不上，宁可多查一次不漏显
            r.verBefore = svc->callLogVersion();
            auto rR = svc->recentCalls(3);
            if (rR.isOk()) {
                r.recent = std::move(rR).value();
                r.recentOk = true;
            } else {
                r.callsError = rR.errorMessage();
            }
        }
        return r;
    }

    // 落快照（只在渲染线程调：onStart 首次装载 + compose 收割/兜底）
    void applyDataRefresh(DataRefresh r) {
        if (r.wantProfiles) {
            if (r.profilesError.empty()) {
                cachedProfiles_ = std::move(r.profiles);
                cachedProfilesError_.clear();
                dataDirty_ = false;   // list 查询成功才清脏（失败保持脏下帧重试）
                dataLoaded_ = true;   // 快照已装载：此后刷新全走后台
            } else {
                cachedProfiles_.clear();
                cachedProfilesError_ = r.profilesError;
                LOG_ERROR("Profile 列表加载失败: " + cachedProfilesError_);
            }
            cachedActive_ = std::move(r.active);
            cachedHasActive_ = r.hasActive;
        }
        if (r.wantRecent) {
            if (r.recentOk) {
                cachedRecent_ = std::move(r.recent);
                cachedCallsError_.clear();
                seenCallLogVersion_ = r.verBefore;
            } else {
                cachedRecent_.clear();
                cachedCallsError_ = r.callsError;
                LOG_ERROR("调用记录加载失败: " + cachedCallsError_);
            }
        }
    }

    // 收割：后台结果就绪才搬（wait_for(0) 不阻塞；未就绪直接跳过，快照沿用
    // 上次值——不丢不改，无空帧）
    void harvestDataRefresh() {
        if (!refreshFuture_.valid()) return;
        if (refreshFuture_.wait_for(std::chrono::seconds(0)) !=
            std::future_status::ready) {
            return;   // 还在跑
        }
        applyDataRefresh(refreshFuture_.get());
    }

    // 审计 H2：节流窗口。版本轮询在代理高频落库时每帧都能看到新版本，不节流
    // 会退化成每帧一个后台查询（仍与 logCall INSERT 抢同一把 recursive_mutex）；
    // 失败重试同窗口（脏标记不清 → 按窗口重发，不按帧起线程）
    bool refreshDue() const {
        return std::chrono::steady_clock::now() - lastRefreshPost_ >=
               std::chrono::milliseconds(kRefreshIntervalMs);
    }

    // compose 每帧调：只发意图 + 搬快照，零 IO（语义同旧 refreshDataCache：
    // 脏 = 进页/写操作后的重查；recent 另在 callLogVersion 变化时重查）
    void refreshDataCache(ClaudeService* svc) {
        harvestDataRefresh();
        if (!dataLoaded_) {
            // 冷启动兜底：快照从未装载过（服务晚就绪 / 调用方未走 onStart）时
            // 本帧同步装载一次。首帧必须有快照（硬约束：不得空帧），异步首刷
            // 必然先渲染一帧空列表 → 这一帧的同步查询是刻意的、只发生一次；
            // 装载成功即转后台（失败保持本路径逐帧重试 = 旧「每帧查」同退化）
            applyDataRefresh(runDataRefresh(svc, true, true));
            return;
        }
        if (refreshFuture_.valid()) {
            return;   // 上一轮在途：不叠加（脏/版本条件下帧仍成立，意图不丢）
        }
        const uint64_t ver = svc->callLogVersion();
        const bool wantProfiles = dataDirty_;
        const bool verChanged = (ver != seenCallLogVersion_);
        if (!wantProfiles && !verChanged) {
            return;   // 空闲帧零开销（与旧实现一致：版本没动就不查）
        }
        if (!refreshDue()) {
            return;   // 节流窗口内：下帧再判（可见最多晚一个节流 tick + 1 帧）
        }
        lastRefreshPost_ = std::chrono::steady_clock::now();
        refreshFuture_ = std::async(std::launch::async, runDataRefresh, svc,
                                    wantProfiles, wantProfiles || verChanged);
    }

    // ============ Profile 卡片（点击选中，选中/激活高亮） ============
    void drawProfileCard(eui::Ui& ui,
                         const std::string& id,
                         float y,
                         float width,
                         const core::theme::Tokens& t,
                         const Profile& p) {
        const float h = 96.0f;
        const bool selected = (p.id == selectedId_);
        const eui::Color borderColor = (p.isActive || selected)
            ? t.colors.primary : t.colors.borderDefault;
        const float borderW = p.isActive ? 2.0f : 1.0f;
        const eui::Color bg = p.isActive ? t.colors.primarySoftBg : t.colors.surface;

        // 卡片背景（可点击 → 选中后详情面板跟随）
        ui.rect(id + ".bg")
            .position(0.0f, y)
            .size(width, h)
            .states(bg, t.colors.hover, t.colors.active)
            .radius(t.sizes.radiusLarge)
            .border(borderW, borderColor)
            .onClick([this, pid = p.id] { selectedId_ = pid; })
            .build();

        // 头像（圆形 - 用 rect 模拟）
        ui.rect(id + ".avatar")
            .position(16.0f, y + 16.0f)
            .size(40.0f, 40.0f)
            .color(t.colors.surfaceElevated)
            .radius(20.0f)
            .border(1.0f, t.colors.borderDefault)
            .build();
        // 头像首字
        const std::string initial = p.name.empty()
            ? std::string("?")
            : std::string(1, p.name[0]);
        ui.text(id + ".avatar.text")
            .position(16.0f, y + 16.0f)
            .size(40.0f, 40.0f)
            .text(initial)
            .fontSize(16.0f)
            .lineHeight(40.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.textPrimary)
            .fontWeight(600)
            .build();

        // 名称
        ui.text(id + ".name")
            .position(64.0f, y + 18.0f)
            .size(width - 130.0f, 20.0f)
            .text(p.name)
            .fontSize(t.sizes.fontSizeBody)
            .lineHeight(20.0f)
            .fontWeight(600)
            .color(t.colors.textPrimary)
            .build();

        if (p.isActive) {
            ui.rect(id + ".active.bg")
                .position(width - 60.0f, y + 16.0f)
                .size(48.0f, 20.0f)
                .color(t.colors.primary)
                .radius(t.sizes.radiusPill)
                .build();
            ui.text(id + ".active.text")
                .position(width - 60.0f, y + 16.0f)
                .size(48.0f, 20.0f)
                .text("ACTIVE")
                .fontSize(9.0f)
                .lineHeight(20.0f)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(t.colors.onPrimary)
                .fontWeight(700)
                .build();
        }

        // base-url (mono, 12px, tertiary)
        ui.text(id + ".url")
            .position(64.0f, y + 44.0f)
            .size(width - 80.0f, 14.0f)
            .text(p.baseUrl)
            .fontSize(11.0f)
            .lineHeight(14.0f)
            .color(t.colors.textTertiary)
            .build();

        // model（default 槽位，未配则旧 model 列 fallback——§6.2 列表卡片）
        ui.text(id + ".model")
            .position(64.0f, y + 62.0f)
            .size(width - 80.0f, 14.0f)
            .text(std::string("model: ") +
                  effectiveModelsConfig(p.modelsConfigJson, p.model).def.model)
            .fontSize(11.0f)
            .lineHeight(14.0f)
            .color(t.colors.textSecondary)
            .build();
    }

    // ============ 新建/编辑 Profile 表单弹窗（tabs 三页，§6.1） ============
    void openCreateForm() {
        formEditId_ = 0;   // 0 = 新建
        formTab_ = 0;
        formName_.clear();
        formUrl_.clear();
        formKey_.clear();
        formError_.clear();
        formKeyHint_.clear();
        formImportWarn_.clear();
        formModels_ = ModelsConfig{};
        formEnvRows_.clear();
        formAuthField_ = AuthField::Both;
        formAuthFieldOpen_ = false;
        resetImportState();
        formOpen_ = true;
    }

    void openEditForm(const Profile& p) {
        formEditId_ = p.id;
        formTab_ = 0;
        formName_ = p.name;
        formUrl_ = p.baseUrl;
        formKey_.clear();   // 留空 = 不换 key（updateProfile 语义）
        formError_.clear();
        formKeyHint_ = p.apiKeyHint;
        formImportWarn_.clear();
        // 槽位：models_config + 旧 model 列 fallback（§4.2 兼容读取）
        formModels_ = effectiveModelsConfig(p.modelsConfigJson, p.model);
        // 认证字段：编辑必须回显当前值（否则用户以为看到的是默认值，一保存就坐实覆盖）
        formAuthField_ = authFieldFromText(p.authField);
        formAuthFieldOpen_ = false;
        // env 行：非法 JSON 读路径容错为空表（不炸）
        formEnvRows_.clear();
        auto ee = parseExtraEnv(p.extraEnvJson);
        if (ee.isOk()) {
            for (const auto& [k, v] : ee.value()) {
                formEnvRows_.emplace_back(k, v);
            }
        }
        resetImportState();
        formOpen_ = true;
    }

    void resetImportState() {
        importText_.clear();
        importReady_ = false;
        importError_.clear();
        importPreview_ = ImportedConfig{};
    }

    // 导入解析（纯函数消费，Phase D）：失败只写红字，不动表单
    void doParseImport() {
        importError_.clear();
        importReady_ = false;
        auto r = parseImportedEnv(importText_);
        if (r.isFail()) {
            importError_ = r.errorMessage();
            return;
        }
        importPreview_ = std::move(r).value();
        importReady_ = true;
    }

    // 确认导入：按解析结果填充三页表单（BASE_URL/token → 基础页，
    // 槽位非空覆盖，env 键同名替换 / 新键追加）
    void doApplyImport() {
        if (!importReady_) return;
        if (!importPreview_.baseUrl.empty()) formUrl_ = importPreview_.baseUrl;
        if (!importPreview_.authToken.empty()) {
            formKey_ = importPreview_.authToken;
        }
        // 槽位：非空则覆盖（遍历槽位表派生，CPX-08）
        for (const auto& meta : modelSlotTable()) {
            const ModelSlot& src = importPreview_.models.*meta.slot;
            if (!src.model.empty()) formModels_.*meta.slot = src;
        }
        for (const auto& [k, v] : importPreview_.extraEnv) {
            bool replaced = false;
            for (auto& row : formEnvRows_) {
                if (row.first == k) {
                    row.second = v;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) formEnvRows_.emplace_back(k, v);
        }
        // 认证字段：按键名证据填（空串 = 无证据，不覆盖表单现值；设计 §5）
        if (!importPreview_.authFieldFromEnv.empty()) {
            formAuthField_ = authFieldFromText(importPreview_.authFieldFromEnv);
        }
        importReady_ = false;
        importText_.clear();
        formError_.clear();   // 导入成功清掉旧校验错误
    }

    // ============ 导入当前配置（一键读 settings.json → 预填新建表单）============
    // 等价于用户手动「从 cc-switch / settings.json 粘贴导入」但全自动：
    // 读用户级 settings.json → parseImportedEnv → 打开新建弹窗并预填
    // （基础页 BASE_URL/Token、模型映射页槽位、高级 env 页 + 预览标记）。
    // 接管态（sidecar 快照存在）时 settings.json 里是代理托管值
    // （BASE_URL=127.0.0.1:8888）而非原始上游 → 弹窗内给警告，不阻断。
    void doImportCurrentConfig() {
        // 路径解析：service 注入优先（生产 = main 注入的 defaultPaths，
        // 测试 = 临时目录注入），未注入（service 为 null 的防御分支）回落
        // defaultPaths()——与设置页/引导条的路径口径一致
        settings_sync::Paths paths = settings_sync::defaultPaths();
        ClaudeService* svc = ClaudeService::global();
        if (svc && !svc->settingsSyncPaths().settingsFile.empty()) {
            paths = svc->settingsSyncPaths();
        }

        // 读文件：不存在 → toast，不弹窗
        std::error_code ec;
        if (!std::filesystem::exists(paths.settingsFile, ec) || ec) {
            showToast("导入失败",
                      "未找到 ~/.claude/settings.json（" +
                          paths.settingsFile.string() + "）",
                      true);
            return;
        }
        std::ifstream in(paths.settingsFile, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string content = ss.str();
        // 整段 settings.json 没有 env 键 = 没有可导入的配置（区别于粘贴导入
        // 认裸 env 对象的形状启发：文件只会是 settings.json 形状），给明确
        // 提示而非共享解析器的「键名非法: permissions」误报
        if (auto doc = platform::json::parse(content);
            doc.isOk() && doc.value().isObject() &&
            !doc.value().asObject().has("env")) {
            showToast("导入失败",
                      "settings.json 没有 env 块，无配置可导入", true);
            return;
        }
        auto parsed = parseImportedEnv(content);
        if (parsed.isFail()) {
            showToast("导入失败",
                      "settings.json 解析失败: " + parsed.errorMessage(), true);
            return;
        }

        // 全自动「粘贴导入」：新建表单 → 解析结果 → 应用 → 名称建议
        openCreateForm();
        importPreview_ = std::move(parsed).value();
        importReady_ = true;
        doApplyImport();       // 预填三页表单（结束时清 importReady_）
        importReady_ = true;   // 保留高级 env 页预览标记（识别数 + 警告角标）
        formName_ = suggestImportName(importPreview_.baseUrl);

        // 接管态警告：sidecar 存在 → 导入的是代理配置而非原始上游
        if (settings_sync::hasPendingSnapshot(paths)) {
            formImportWarn_ =
                "当前是接管态，导入的是代理配置而非原始上游"
                "（BASE_URL=127.0.0.1:8888）；建议先停止代理再导入";
        }
    }

    // 表单 → models_config / extra_env JSON（全空 = 空串，保持「未配置」语义）
    bool serializeFormData(std::string* modelsJson, std::string* envJson) {
        bool anySlot = false;
        for (const auto& meta : modelSlotTable()) {   // 槽位表派生（CPX-08）
            const ModelSlot& s = formModels_.*meta.slot;
            if (!s.model.empty() || !s.name.empty()) { anySlot = true; break; }
        }
        if (anySlot) {
            auto r = serializeModelsConfig(formModels_);
            if (r.isFail()) {
                formError_ = "模型配置序列化失败: " + r.errorMessage();
                return false;
            }
            *modelsJson = r.value();
        }
        std::map<std::string, std::string> envMap;   // 空键行跳过
        for (const auto& row : formEnvRows_) {
            if (row.first.empty()) continue;
            envMap[row.first] = row.second;   // 同名后行胜（map 语义）
        }
        if (!envMap.empty()) {
            auto r = serializeExtraEnv(envMap);
            if (r.isFail()) {
                formError_ = "env 序列化失败: " + r.errorMessage();
                return false;
            }
            *envJson = r.value();
        }
        return true;
    }

    void submitForm() {
        ClaudeService* svc = ClaudeService::global();
        if (!svc) {
            formError_ = "服务未就绪（ClaudeService 未注册）";
            return;
        }
        dataDirty_ = true;   // 审计 perf [13]：写操作前即标脏（失败也不伤，下帧重查）
        if (formName_.empty() || formUrl_.empty()) {
            formError_ = "名称和 Base URL 不能为空";
            return;
        }
        std::string modelsJson;
        std::string envJson;
        if (!serializeFormData(&modelsJson, &envJson)) return;

        if (formEditId_ == 0) {
            // 新建：API Key 必填
            if (formKey_.empty()) {
                formError_ = "新建 Profile 必须填写 API Key";
                return;
            }
            // 产品审计 2026-09-23 [6]：activateIfNoneActive=true——当前无激活
            // Profile 时 service 层自动激活这条（首次体验闭环）；已有激活则不抢占
            auto r = svc->createProfile(formName_, formUrl_, formKey_,
                                        formModels_.def.model, /*activateIfNoneActive=*/true,
                                        /*sharedConfigId=*/0,
                                        authFieldToText(formAuthField_));
            if (r.isFail()) {
                formError_ = r.errorMessage();
                return;
            }
            // createProfile 无槽位/env 参数（最小 diff 不动签名），二段补写
            if (!modelsJson.empty() || !envJson.empty()) {
                Profile p = r.value();
                p.model = formModels_.def.model;
                p.modelsConfigJson = modelsJson;
                p.extraEnvJson = envJson;
                auto u = svc->updateProfile(p);
                if (u.isFail()) {
                    formError_ = "已创建，但模型/env 写入失败: " +
                                 u.errorMessage();
                    return;
                }
            }
            formOpen_ = false;
            selectedId_ = r.value().id;
            // 审计 2026-09-23 [6]：按是否自动激活分文案——只说「已保存」会让
            // 用户以为点「启动代理」就能用（无激活时 Claude Code 直接 503）
            if (r.value().isActive) {
                showToast("已创建并启用",
                          "Profile「" + formName_ + "」已设为激活（ACTIVE）",
                          false);
            } else {
                showToast("已创建",
                          "Profile「" + formName_ +
                              "」已保存，点击「启用」使其生效",
                          false);
            }
        } else {
            // 编辑：先取现有行（保留 keychain 引用等字段），再覆盖表单值
            auto got = svc->getProfile(formEditId_);
            if (got.isFail()) {
                formError_ = got.errorMessage();
                return;
            }
            Profile p = std::move(got).value();
            p.name = formName_;
            p.baseUrl = formUrl_;
            p.model = formModels_.def.model;   // 旧列与 default 槽位同步（§4.2）
            p.modelsConfigJson = modelsJson;
            p.extraEnvJson = envJson;
            p.apiKeyPlain = formKey_;   // 空 = 不换 key
            p.authField = authFieldToText(formAuthField_);
            auto r = svc->updateProfile(p);
            if (r.isFail()) {
                formError_ = r.errorMessage();
                return;
            }
            formOpen_ = false;
            showToast("已保存", "Profile「" + formName_ + "」已更新", false);
        }
    }

    // ---- 基础页（§6.1 线框）----
    void drawFormBasicTab(eui::Ui& ui, float w,
                          const core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& themeTokens,
                          bool isEdit) {
        ui.text("claude.form.basic.name.label")
            .position(28.0f, 104.0f).size(w - 56.0f, 16.0f)
            .text("名称").fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
        components::input(ui, "claude.dialog.form.name")
            .position(28.0f, 122.0f).size(w - 56.0f, 40.0f)
            .value(formName_).placeholder("例如：官方 / 中转 A")
            .fontSize(14.0f).theme(themeTokens)
            .onChange([this](const std::string& v) { formName_ = v; })
            // 审计 [21]：Esc=取消（关表单不提交，同「取消」按钮）；表单各输入框统一
            .onCancel([this] { formOpen_ = false; })
            .build();

        ui.text("claude.form.basic.url.label")
            .position(28.0f, 174.0f).size(w - 56.0f, 16.0f)
            .text("Base URL").fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
        components::input(ui, "claude.dialog.form.url")
            .position(28.0f, 192.0f).size(w - 56.0f, 40.0f)
            .value(formUrl_).placeholder("https://api.anthropic.com")
            .fontSize(14.0f).theme(themeTokens)
            .onChange([this](const std::string& v) { formUrl_ = v; })
            .onCancel([this] { formOpen_ = false; })  // 审计 [21]：Esc=取消
            .build();

        ui.text("claude.form.basic.key.label")
            .position(28.0f, 244.0f).size(w - 56.0f, 16.0f)
            .text("API Key（存入 Keychain，不落数据库）")
            .fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
        const std::string keyPlaceholder = isEdit
            ? ("留空 = 保持现有 Key（" + formKeyHint_ + "）")
            : std::string("sk-ant-...");
        components::input(ui, "claude.dialog.form.key")
            .position(28.0f, 262.0f).size(w - 56.0f, 40.0f)
            .value(formKey_).placeholder(keyPlaceholder)
            .fontSize(14.0f).theme(themeTokens)
            .onChange([this](const std::string& v) { formKey_ = v; })
            .onCancel([this] { formOpen_ = false; })  // 审计 [21]：Esc=取消
            .build();

        // 认证字段（鉴权头三态；2026-09-27 profile-auth-field 设计 §6）：
        // 标签与灰字说明同行，下拉紧随其后；其下三块（副提示/错误/接管警告）
        // 整体下移 64px 让位
        ui.text("claude.form.basic.authfield.label")
            .position(28.0f, 314.0f).size(72.0f, 16.0f)
            .text("认证字段").fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
        ui.text("claude.form.basic.authfield.hint")
            .position(104.0f, 314.0f).size(w - 132.0f, 16.0f)
            .text("网关只接受一种鉴权头时切换；默认两种都发")
            .fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
        ui.stack("claude.form.basic.authfield.ddwrap")
            .position(28.0f, 332.0f)
            .size(w - 56.0f, 40.0f)
            .content([&] {
                components::dropdown(ui, "claude.dialog.form.authfield.dd")
                    .size(w - 56.0f, 40.0f)
                    .theme(themeTokens)
                    .items(std::vector<std::string>(
                        kAuthFieldLabels, kAuthFieldLabels + kAuthFieldCount))
                    .selected(static_cast<int>(formAuthField_))
                    .open(formAuthFieldOpen_)
                    .onOpenChange([this](bool v) { formAuthFieldOpen_ = v; })
                    .onChange([this](int idx) {
                        formAuthField_ = static_cast<AuthField>(
                            std::clamp(idx, 0, kAuthFieldCount - 1));
                    })
                    .build();
            })
            .build();

        ui.text("claude.form.basic.hint.icon")
            .position(28.0f, 380.0f).size(14.0f, 18.0f)
            .icon(0xF05A).fontSize(12.0f).lineHeight(18.0f)
            .color(t.colors.textTertiary).build();
        ui.text("claude.form.basic.hint")
            .position(46.0f, 380.0f).size(w - 74.0f, 18.0f)
            .text("模型映射和高级 env 在另外两个标签页配置")
            .fontSize(12.0f).lineHeight(18.0f)
            .color(t.colors.textTertiary).build();

        if (!formError_.empty()) {
            ui.text("claude.dialog.form.error")
                .position(28.0f, 404.0f).size(w - 56.0f, 18.0f)
                .text(formError_).fontSize(12.0f).lineHeight(18.0f)
                .color(t.colors.error).build();
        }

        // 「导入当前配置」接管态警告（不阻断，仅提示；⚠ 图标 + 文字两元素）
        if (!formImportWarn_.empty()) {
            ui.text("claude.dialog.form.importwarn.icon")
                .position(28.0f, 426.0f).size(14.0f, 18.0f)
                .icon(0xF071).fontSize(12.0f).lineHeight(18.0f)
                .color(t.colors.warning).build();
            ui.text("claude.dialog.form.importwarn")
                .position(46.0f, 426.0f).size(w - 74.0f, 36.0f)
                .text(formImportWarn_).fontSize(12.0f).lineHeight(18.0f)
                .color(t.colors.warning).build();
        }
    }

    // ---- 模型映射页（槽位逐行 × model/name，§6.1 线框；行 = 槽位表派生，CPX-08）----
    void drawFormModelsTab(eui::Ui& ui, float w,
                            const core::theme::Tokens& t,
                            const components::theme::ThemeColorTokens& themeTokens) {
        const auto slotTable = modelSlotTable();

        // 表头
        ui.text("claude.form.slots.h.slot")
            .position(28.0f, 104.0f).size(110.0f, 14.0f)
            .text("槽位").fontSize(11.0f).lineHeight(14.0f)
            .color(t.colors.textTertiary).build();
        ui.text("claude.form.slots.h.model")
            .position(150.0f, 104.0f).size(330.0f, 14.0f)
            .text("上游模型名（发往上游）").fontSize(11.0f).lineHeight(14.0f)
            .color(t.colors.textTertiary).build();
        ui.text("claude.form.slots.h.name")
            .position(492.0f, 104.0f).size(240.0f, 14.0f)
            .text("显示名（CC 界面）").fontSize(11.0f).lineHeight(14.0f)
            .color(t.colors.textTertiary).build();

        for (size_t i = 0; i < slotTable.size(); ++i) {
            const float y = 126.0f + static_cast<float>(i) * 52.0f;
            const auto& row = slotTable[i];
            const ModelSlot& slot = formModels_.*row.slot;
            const std::string idx = std::to_string(i);
            ui.text("claude.form.slot.label." + idx)
                .position(28.0f, y + 10.0f).size(110.0f, 16.0f)
                .text(row.formLabel).fontSize(12.0f).lineHeight(16.0f)
                .color(t.colors.textSecondary).build();
            components::input(ui, "claude.form.slot." + idx + ".model")
                .position(150.0f, y).size(330.0f, 36.0f)
                .value(slot.model).placeholder("fangzhou/glm-5.3[1M]")
                .fontSize(13.0f).theme(themeTokens)
                .onChange([this, m = row.slot](const std::string& v) {
                    (formModels_.*m).model = v;
                })
                .onCancel([this] { formOpen_ = false; })  // 审计 [21]：Esc=取消
                .build();
            // 显示名留空 → placeholder 渲染派生预览（§6.1）
            const std::string derived = deriveDisplayName(slot.model);
            components::input(ui, "claude.form.slot." + idx + ".name")
                .position(492.0f, y).size(240.0f, 36.0f)
                .value(slot.name)
                .placeholder(derived.empty() ? "（自动派生）" : derived)
                .fontSize(13.0f).theme(themeTokens)
                .onChange([this, m = row.slot](const std::string& v) {
                    (formModels_.*m).name = v;
                })
                .onCancel([this] { formOpen_ = false; })  // 审计 [21]：Esc=取消
                .build();
        }

        ui.text("claude.form.slots.hint1.icon")
            .position(28.0f, 446.0f).size(14.0f, 16.0f)
            .icon(0xF05A).fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
        ui.text("claude.form.slots.hint1")
            .position(46.0f, 446.0f).size(w - 74.0f, 16.0f)
            .text("显示名留空时自动剥除模型名的 [1M]/[200K] 等上下文标记")
            .fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
        ui.text("claude.form.slots.hint2")
            .position(28.0f, 464.0f).size(w - 56.0f, 16.0f)
            .text("    槽位留空 = 不写入该 env 键，Claude Code 用内置映射")
            .fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
    }

    // ---- 高级 env 页（键值编辑器 + cc-switch 粘贴导入，§6.1 线框）----
    // ponytail: 可见行数上限 5（760×600 弹窗高度约束），超出部分保存在
    // 状态里全量保存；需要编辑超长列表时再引入 scrollView
    void drawFormEnvTab(eui::Ui& ui, float w,
                        const core::theme::Tokens& t,
                        const components::theme::ThemeColorTokens& themeTokens) {
        constexpr size_t kMaxVisibleRows = 5;

        ui.text("claude.form.env.label")
            .position(28.0f, 104.0f).size(w - 56.0f, 16.0f)
            .text("额外环境变量（原样写入 settings.json env 块）")
            .fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();

        const size_t visible =
            formEnvRows_.size() < kMaxVisibleRows ? formEnvRows_.size()
                                                  : kMaxVisibleRows;
        for (size_t i = 0; i < visible; ++i) {
            const float y = 124.0f + static_cast<float>(i) * 38.0f;
            const std::string idx = std::to_string(i);
            components::input(ui, "claude.form.env.row." + idx + ".key")
                .position(28.0f, y).size(220.0f, 32.0f)
                .value(formEnvRows_[i].first).placeholder("ENV_KEY")
                .fontSize(12.0f).theme(themeTokens)
                .onChange([this, i](const std::string& v) {
                    formEnvRows_[i].first = v;
                })
                .onCancel([this] { formOpen_ = false; })  // 审计 [21]：Esc=取消
                .build();
            // 变量名识别（2026-09-24，官方 env-vars 文档全量清单）：
            // 已知 → hover 显示说明 tooltip；未知且非空 → 红边框提示
            //（仍可导入，键名格式校验已在解析侧放开）
            if (const char* hint = envVarHint(formEnvRows_[i].first)) {
                components::tooltip(ui, "claude.form.env.row." + idx + ".tip")
                    .source("claude.form.env.row." + idx + ".key.hit")
                    .value(hint)
                    .anchor(138.0f, y + 16.0f)
                    .bounds(w, 420.0f)
                    .theme(themeTokens)
                    .zIndex(60)
                    .build();
            } else if (!formEnvRows_[i].first.empty()) {
                ui.rect("claude.form.env.row." + idx + ".key.unknown")
                    .position(28.0f, y).size(220.0f, 32.0f)
                    .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                    .border(1.0f, t.colors.error)
                    .radius(t.sizes.radiusSmall)
                    .build();
            }
            components::input(ui, "claude.form.env.row." + idx + ".value")
                .position(258.0f, y).size(414.0f, 32.0f)
                .value(formEnvRows_[i].second).placeholder("值（字符串）")
                .fontSize(12.0f).theme(themeTokens)
                .onChange([this, i](const std::string& v) {
                    formEnvRows_[i].second = v;
                })
                .onCancel([this] { formOpen_ = false; })  // 审计 [21]：Esc=取消
                .build();
            components::button(ui, "claude.form.env.row." + idx + ".del")
                .position(682.0f, y).size(50.0f, 32.0f)
                .icon(0xF00D)  // fa-xmark（✕ 换 FA）
                .fontSize(12.0f)
                .colors(t.colors.surfaceElevated, t.colors.hover,
                        t.colors.active)
                .textColor(t.colors.error)
                .iconColor(t.colors.error)
                .radius(t.sizes.radiusSmall)
                .border(1.0f, t.colors.borderDefault)
                .onClick([this, i] {
                    if (i < formEnvRows_.size()) {
                        formEnvRows_.erase(formEnvRows_.begin() +
                                           static_cast<long>(i));
                    }
                })
                .build();
        }

        // 底部：添加按钮 + 溢出说明
        components::button(ui, "claude.form.env.add")
            .position(28.0f, 314.0f).size(110.0f, 30.0f)
            .text("+ 添加变量").fontSize(12.0f)
            .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
            .textColor(t.colors.textPrimary)
            .radius(t.sizes.radiusSmall)
            .border(1.0f, t.colors.borderDefault)
            .onClick([this] { formEnvRows_.emplace_back(); })
            .build();
        if (formEnvRows_.size() > kMaxVisibleRows) {
            ui.text("claude.form.env.overflow")
                .position(150.0f, 320.0f).size(w - 150.0f - 28.0f, 16.0f)
                .text("另有 " + std::to_string(formEnvRows_.size() -
                                               kMaxVisibleRows) +
                      " 项已保存（保存时全量写入，列表仅显示前 " +
                      std::to_string(kMaxVisibleRows) + " 项）")
                .fontSize(11.0f).lineHeight(16.0f)
                .color(t.colors.textTertiary).build();
        }

        // ---- 粘贴导入（cc-switch / settings.json）----
        ui.text("claude.form.import.label")
            .position(28.0f, 356.0f).size(w - 56.0f, 16.0f)
            .text("── 从 cc-switch / settings.json 粘贴导入 ──")
            .fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
        components::input(ui, "claude.form.import.text")
            .position(28.0f, 376.0f).size(w - 56.0f, 82.0f)
            .value(importText_)
            .placeholder(R"({ "env": { "ANTHROPIC_BASE_URL": "...", ... } }  粘贴整段 settings.json 或仅 env 对象)")
            .multiline(true).scrollbar(true)
            .fontSize(12.0f).theme(themeTokens)
            .onChange([this](const std::string& v) { importText_ = v; })
            .onCancel([this] { formOpen_ = false; })  // 审计 [21]：Esc=取消
            .build();

        components::button(ui, "claude.form.import.parse")
            .position(28.0f, 468.0f).size(110.0f, 32.0f)
            .text("解析导入").fontSize(12.0f)
            .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
            .textColor(t.colors.textPrimary)
            .radius(t.sizes.radiusSmall)
            .border(1.0f, t.colors.borderDefault)
            .onClick([this] { doParseImport(); })
            .build();
        if (importReady_) {
            components::button(ui, "claude.form.import.apply")
                .position(148.0f, 468.0f).size(110.0f, 32.0f)
                .text("应用到表单").fontSize(12.0f)
                .colors(t.colors.primary, t.colors.primaryHover,
                        t.colors.primaryPressed)
                .textColor(t.colors.onPrimary)
                .radius(t.sizes.radiusSmall)
                .onClick([this] { doApplyImport(); })
                .build();
        }

        // 解析结果：失败红字 / 成功预览（槽位/env 识别数 + 别名/未识别提示）
        //（✕/⚠ 换 FA：图标/文字拆两个元素）
        if (!importError_.empty()) {
            ui.text("claude.form.import.error.icon")
                .position(270.0f, 468.0f).size(13.0f, 16.0f)
                .icon(0xF00D).fontSize(11.0f).lineHeight(16.0f)
                .color(t.colors.error).build();
            ui.text("claude.form.import.error")
                .position(286.0f, 468.0f).size(w - 286.0f - 28.0f, 48.0f)
                .text(importError_).fontSize(11.0f).lineHeight(16.0f)
                .color(t.colors.error).build();
        } else if (importReady_) {
            ui.text("claude.form.import.preview")
                .position(270.0f, 468.0f).size(w - 270.0f - 28.0f, 16.0f)
                .text("识别：槽位 " + std::to_string(importPreview_.recognizedSlots) +
                      "/" + std::to_string(kModelSlotCount) +
                      " · env " + std::to_string(importPreview_.extraEnv.size()) +
                      " 项 · 别名映射 " +
                      std::to_string(importPreview_.aliasMappedKeys.size()) +
                      " · 未识别 " +
                      std::to_string(importPreview_.unrecognizedKeys.size()))
                .fontSize(11.0f).lineHeight(16.0f)
                .color(t.colors.textSecondary).build();
            // 警告角标透传（§4.3 规则 6）：别名映射 / 未识别键 / 解析警告，
            // 最多 3 条（弹窗高度约束——再往下会压到底部按钮；全量信息在
            // 保存后的 settings.json 与日志可查）
            std::vector<std::string> notes;
            for (size_t i = 0; i < importPreview_.aliasMappedKeys.size() && i < 2; ++i) {
                notes.push_back("已映射为官方变量: " +
                                importPreview_.aliasMappedKeys[i]);
            }
            for (size_t i = 0;
                 i < importPreview_.unrecognizedKeys.size() && notes.size() < 3;
                 ++i) {
                notes.push_back("未识别键将透传: " +
                                importPreview_.unrecognizedKeys[i]);
            }
            for (size_t i = 0;
                 i < importPreview_.warnings.size() && notes.size() < 3; ++i) {
                notes.push_back(importPreview_.warnings[i]);
            }
            // 角标与底部校验错误行（dialog 里 y=h-104，formTab_!=0 时画）
            // 同屏重叠：有校验错误时让位，只显示校验错误（提交失败更可行动；
            // 角标信息全量在保存后的 settings.json 与日志可查）
            if (formError_.empty()) {
                for (size_t i = 0; i < notes.size(); ++i) {
                    ui.text("claude.form.import.note.icon." + std::to_string(i))
                        .position(270.0f, 486.0f + static_cast<float>(i) * 15.0f)
                        .size(13.0f, 15.0f)
                        .icon(0xF071).fontSize(10.0f).lineHeight(15.0f)
                        .color(t.colors.warning).build();
                    ui.text("claude.form.import.note." + std::to_string(i))
                        .position(286.0f, 486.0f + static_cast<float>(i) * 15.0f)
                        .size(w - 286.0f - 28.0f, 15.0f)
                        .text(notes[i]).fontSize(10.0f).lineHeight(15.0f)
                        .color(t.colors.warning).build();
                }
            }
        } else {
            ui.text("claude.form.import.hint.icon")
                .position(270.0f, 468.0f).size(13.0f, 16.0f)
                .icon(0xF05A).fontSize(11.0f).lineHeight(16.0f)
                .color(t.colors.textTertiary).build();
            ui.text("claude.form.import.hint")
                .position(286.0f, 468.0f).size(w - 286.0f - 28.0f, 32.0f)
                .text("BASE_URL/AUTH_TOKEN 归基础页，模型键归模型映射页，"
                      "其余进入上表")
                .fontSize(11.0f).lineHeight(16.0f)
                .color(t.colors.textTertiary).build();
        }
    }

    void drawProfileFormDialog(eui::Ui& ui, const eui::Screen& screen,
                               const core::theme::Tokens& t,
                               const components::theme::ThemeColorTokens& themeTokens) {
        const float w = 760.0f;
        const float h = 600.0f;
        const bool isEdit = (formEditId_ != 0);

        components::dialog(ui, "claude.dialog.form")
            .open(formOpen_)
            .screen(screen.width, screen.height)
            .size(w, h)
            .theme(themeTokens)
            .onOpenChange([this](bool v) { formOpen_ = v; })
            .content([&] {
                ui.text("claude.dialog.form.title")
                    .position(28.0f, 20.0f)
                    .size(w - 56.0f, 26.0f)
                    .text(isEdit ? "编辑 Profile" : "新建 Profile")
                    .fontSize(20.0f)
                    .lineHeight(26.0f)
                    .fontWeight(700)
                    .color(t.colors.textPrimary)
                    .build();

                // tabs（§6.1：基础 / 模型映射 / 高级 env）。
                // TabsBuilder 无 position，套 stack 定位（字号下拉同款包装）
                ui.stack("claude.form.tabs.wrap")
                    .position(28.0f, 52.0f)
                    .size(w - 56.0f, 34.0f)
                    .content([&] {
                        components::tabs(ui, "claude.form.tabs")
                            .size(w - 56.0f, 34.0f)
                            .items({"基础", "模型映射", "高级 env"})
                            .selected(formTab_)
                            .fontSize(13.0f)
                            .theme(themeTokens)
                            .onChange([this](int idx) { formTab_ = idx; })
                            .build();
                    })
                    .build();

                if (formTab_ == 0) {
                    drawFormBasicTab(ui, w, t, themeTokens, isEdit);
                } else if (formTab_ == 1) {
                    drawFormModelsTab(ui, w, t, themeTokens);
                } else {
                    drawFormEnvTab(ui, w, t, themeTokens);
                }

                // 错误信息（基础页之外也要能看到校验错误）
                if (!formError_.empty() && formTab_ != 0) {
                    ui.text("claude.dialog.form.error")
                        .position(28.0f, h - 104.0f)
                        .size(w - 56.0f, 18.0f)
                        .text(formError_)
                        .fontSize(12.0f)
                        .lineHeight(18.0f)
                        .color(t.colors.error)
                        .build();
                }

                // 底部按钮
                components::button(ui, "claude.dialog.form.cancel")
                    .position(28.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("取消")
                    .fontSize(14.0f)
                    .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] { formOpen_ = false; })
                    .build();
                components::button(ui, "claude.dialog.form.confirm")
                    .position(28.0f + (w - 68.0f) * 0.5f + 12.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text(isEdit ? "保存" : "创建")
                    .fontSize(14.0f)
                    .colors(t.colors.primary, t.colors.primaryHover,
                            t.colors.primaryPressed)
                    .textColor(t.colors.onPrimary)
                    .radius(t.sizes.radius)
                    .onClick([this] { submitForm(); })
                    .build();
            })
            .build();
    }

    // ============ 删除确认弹窗 ============
    void drawDeleteConfirmDialog(eui::Ui& ui, const eui::Screen& screen,
                                 const core::theme::Tokens& t,
                                 const components::theme::ThemeColorTokens& themeTokens) {
        const float w = 420.0f;
        const float h = 190.0f;

        components::dialog(ui, "claude.dialog.delete")
            .open(deleteConfirmOpen_)
            .screen(screen.width, screen.height)
            .size(w, h)
            .theme(themeTokens)
            .onOpenChange([this](bool v) { deleteConfirmOpen_ = v; })
            .content([&] {
                ui.text("claude.dialog.delete.title")
                    .position(28.0f, 24.0f)
                    .size(w - 56.0f, 28.0f)
                    .text("确认删除？")
                    .fontSize(20.0f)
                    .lineHeight(28.0f)
                    .fontWeight(700)
                    .color(t.colors.textPrimary)
                    .build();
                ui.text("claude.dialog.delete.msg")
                    .position(28.0f, 58.0f)
                    .size(w - 56.0f, 40.0f)
                    .text("将删除 Profile「" + deleteTargetName_ +
                          "」及其 Keychain 中的 API Key，不可恢复。")
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(20.0f)
                    .color(t.colors.textSecondary)
                    .build();

                components::button(ui, "claude.dialog.delete.cancel")
                    .position(28.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("取消")
                    .fontSize(14.0f)
                    .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] { deleteConfirmOpen_ = false; })
                    .build();
                components::button(ui, "claude.dialog.delete.confirm")
                    .position(28.0f + (w - 68.0f) * 0.5f + 12.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("确认删除")
                    .fontSize(14.0f)
                    .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                    .textColor(t.colors.error)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] {
                        const int64_t id = deleteTargetId_;
                        deleteConfirmOpen_ = false;
                        doDelete(id);
                    })
                    .build();
            })
            .build();
    }

    // ============ 完整调用记录弹窗（「查看全部」入口） ============
    // 产品审计 2026-09-23 [11]：spec §8.3 第 6 条承诺「支持过滤、搜索」，
    // 此前是 recentCalls(20) 写死的 20 行静态列表。现走 CallLogQuery：
    // 搜索框（path/method 模糊）+ 状态码下拉（2xx/3xx/4xx/5xx/错误）+
    // 分页（每页 kCallsPageSize 条，页内 scrollView 兜住小窗口）。
    // 审计 CPX-06：取数改「future 刷新」模式（H2 同一套：置脏 → compose 发一次
    // 读意图 → 下一帧收割；settings 日志 tail 同款）。原实现在回调里同步查库
    // ——开弹窗 / 搜索框逐键各跑 queryCalls + countCalls 两条 SQL，代理 worker
    // 持同一把 SQLite 锁时开弹窗/打字会卡帧。现在渲染路径零 DB：快照 allCalls_
    // / callsTotalMatches_ 只读，重查全在后台线程；搜索框按键另加防抖，
    // 窗口内多次按键合并成一次派发。
    //
    // 后台查询结果包（值语义：经 future 回渲染线程，工作线程不触碰 this）
    struct CallsPage {
        std::vector<ClaudeService::CallLogEntry> rows;  // 当页行
        int total = 0;                                  // 满足过滤的总条数
        std::string error;                              // 空串 = 成功
    };

    // 唯一跑在后台线程的调用记录 DB 入口（static：不碰成员，可安全脱离渲染
    // 线程）。当页行 + 总匹配数在**同一次派发**内取（原先渲染路径每键两次同步
    // 查询）。queryCalls 失败 → error 回传（调用方清空当页 + toast）；countCalls
    // 失败按本页行数兜底（同一 WHERE 走同一连接，几乎只会连带 queryCalls 一起
    // 失败，不让已取到的行配一个「0 条匹配」）
    static CallsPage runCallsQuery(ClaudeService* svc,
                                   ClaudeService::CallLogQuery q) {
        // 测试缝：本函数跑在哪个线程（派发计数在派发点记，见 refreshCallsCache）
        g_callsQueryDispatchThread.store(
            static_cast<unsigned long long>(
                std::hash<std::thread::id>{}(std::this_thread::get_id())),
            std::memory_order_relaxed);
        CallsPage p;
        auto r = svc->queryCalls(q);
        if (r.isFail()) {
            p.error = r.errorMessage();
            return p;
        }
        p.rows = std::move(r).value();
        auto c = svc->countCalls(q);
        p.total = c.isOk() ? c.value() : static_cast<int>(p.rows.size());
        return p;
    }

    // 当前过滤条件 → 查询（渲染线程构造、值语义进后台；搜索/下拉/翻页共用）
    ClaudeService::CallLogQuery callsQueryNow() const {
        ClaudeService::CallLogQuery q;
        q.search = callsSearch_;
        q.statusClass =
            kCallsStatusClasses[std::clamp(callsStatusIdx_, 0, kCallsStatusCount - 1)];
        q.limit = kCallsPageSize;
        q.offset = callsPage_ * kCallsPageSize;
        return q;
    }

    // 置脏：下一次 compose（弹窗打开时）发一次读意图。debounce=true（搜索框按键）
    // 时把最早派发时刻顺延一个窗口——窗口内继续按键只顺延，停手后派发一次；
    // 翻页 / 状态下拉 / 开弹窗走 false（下一帧即派发）
    void markCallsStale(bool debounce) {
        callsPageDirty_ = true;
        callsEarliestQueryAt_ = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(debounce ? kCallsSearchDebounceMs : 0);
    }

    // 过滤条件变化：回第一页重查（搜索框按键 → 防抖；状态码下拉 → 立即）
    void applyCallsFilter(bool debounce) {
        callsPage_ = 0;
        callsScrollOffset_ = 0;
        markCallsStale(debounce);
    }

    // 落快照（只在渲染线程调：compose 收割）。成功替换行 + 总数；失败清空当页
    // 并提示（不留上次过滤条件的残留行，同原 reloadCallsPage 失败分支语义）
    void applyCallsPage(CallsPage p) {
        callsPageLoaded_ = true;   // 本轮已判定（失败也不再显示「加载中…」）
        if (!p.error.empty()) {
            allCalls_.clear();
            callsTotalMatches_ = 0;
            LOG_ERROR("调用记录加载失败: " + p.error);
            showToast("加载失败", p.error, true);
            return;
        }
        allCalls_ = std::move(p.rows);
        callsTotalMatches_ = p.total;
    }

    // 审计 CPX-06：弹窗打开时 compose 每帧调，只做两件零 IO 的事——
    // ①在途读已就绪 → 搬进快照（wait_for(0) 不阻塞；未就绪直接跳过，快照沿用
    //   上次值，不丢不改不闪空）；②脏 + 无在途 + 过了防抖窗口 → 发一次读意图
    //   （std::async 后台线程跑 runCallsQuery，结果经 future 值回渲染线程）。
    // 失败不自动重试（同原实现：开弹窗失败即止、翻页失败静默），再次操作即重新置脏
    void refreshCallsCache() {
        if (callsFuture_.valid() &&
            callsFuture_.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready) {
            applyCallsPage(callsFuture_.get());
        }
        if (!callsPageDirty_ || callsFuture_.valid()) {
            return;   // 空闲帧零开销 / 上一轮在途不叠加（脏标记留到下帧再判）
        }
        if (std::chrono::steady_clock::now() < callsEarliestQueryAt_) {
            return;   // 搜索防抖窗口内：下帧再判（停手后一帧内派发）
        }
        ClaudeService* svc = ClaudeService::global();
        if (!svc) {
            callsPageDirty_ = false;
            CallsPage p;
            p.error = "服务未就绪（ClaudeService 未注册）";
            applyCallsPage(std::move(p));
            return;
        }
        callsPageDirty_ = false;
        // 测试缝：派发点计数（在渲染线程上做 → 同帧可读，断言不靠时序）
        g_callsQueryDispatches.fetch_add(1, std::memory_order_relaxed);
        callsFuture_ = std::async(std::launch::async, runCallsQuery, svc,
                                  callsQueryNow());
    }

    // 「查看全部 →」：开弹窗 + 置脏（下一帧后台查第一页；渲染路径不再同步查库）。
    // 首帧显示「加载中…」占位，查询失败由收割出的 toast 提示（不再拒开弹窗）
    void openCallsDialog() {
        callsPage_ = 0;
        callsScrollOffset_ = 0;
        callsDialogOpen_ = true;
        markCallsStale(false);
    }

    void drawCallsDialog(eui::Ui& ui, const eui::Screen& screen,
                         const core::theme::Tokens& t,
                         const components::theme::ThemeColorTokens& themeTokens) {
        // 审计 CPX-06：只在弹窗打开时维持快照（关着不查库）。本帧只做零 IO 的
        // 两件事——搬已就绪结果 + 发一次读意图（见 refreshCallsCache）
        if (callsDialogOpen_) refreshCallsCache();

        const float w = 720.0f;
        const float h = 560.0f;
        const int rowH = 22;

        components::dialog(ui, "claude.dialog.calls")
            .open(callsDialogOpen_)
            .screen(screen.width, screen.height)
            .size(w, h)
            .theme(themeTokens)
            .onOpenChange([this](bool v) { callsDialogOpen_ = v; })
            .content([&] {
                ui.text("claude.dialog.calls.title")
                    .position(28.0f, 20.0f)
                    .size(w - 56.0f, 28.0f)
                    .text("调用记录")
                    .fontSize(20.0f)
                    .lineHeight(28.0f)
                    .fontWeight(700)
                    .color(t.colors.textPrimary)
                    .build();

                // ---- 工具行：搜索框 + 状态码过滤 + 匹配总数 ----
                components::input(ui, "claude.dialog.calls.search")
                    .position(28.0f, 58.0f)
                    .size(260.0f, 32.0f)
                    .value(callsSearch_)
                    .placeholder("搜索路径 / 方法…")
                    .fontSize(12.0f)
                    .theme(themeTokens)
                    .onChange([this](const std::string& v) {
                        if (v == callsSearch_) return;   // 无变化不重查（IME 组字等）
                        callsSearch_ = v;
                        // 审计 CPX-06：逐键只置脏 + 顺延防抖窗口，停手后派发
                        // 一次后台查询（原先每键同步两条 SQL）
                        applyCallsFilter(true);
                    })
                    .onEnter([this] {
                        // Enter/Esc 清空（EUI-NEO input 把 Escape 映射到 onEnter，
                        // 与 todo.search 交互一致）；显式动作 → 不防抖，立即重查
                        if (!callsSearch_.empty()) {
                            callsSearch_.clear();
                            applyCallsFilter(false);
                        }
                    })
                    .build();

                // dropdown 无 position 接口 → 包定位 stack（仿 settings 下拉）
                ui.stack("claude.dialog.calls.status.ddwrap")
                    .position(298.0f, 58.0f)
                    .size(150.0f, 32.0f)
                    .content([&] {
                        components::dropdown(ui, "claude.dialog.calls.status.dd")
                            .size(150.0f, 32.0f)
                            .theme(themeTokens)
                            .items(std::vector<std::string>(
                                kCallsStatusLabels,
                                kCallsStatusLabels + kCallsStatusCount))
                            .selected(callsStatusIdx_)
                            .open(callsStatusDdOpen_)
                            .onOpenChange([this](bool v) { callsStatusDdOpen_ = v; })
                            .onChange([this](int idx) {
                                callsStatusIdx_ =
                                    std::clamp(idx, 0, kCallsStatusCount - 1);
                                applyCallsFilter(false);   // 下拉是明确动作：立即重查
                            })
                            .build();
                    })
                    .build();

                ui.text("claude.dialog.calls.count")
                    .position(460.0f, 58.0f)
                    .size(w - 460.0f - 28.0f, 32.0f)
                    // 快照未装载（弹窗首帧，后台取数在路上）时不显示「0 条匹配」
                    .text(callsPageLoaded_
                              ? (std::to_string(callsTotalMatches_) + " 条匹配")
                              : std::string())
                    .fontSize(12.0f)
                    .lineHeight(32.0f)
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .color(t.colors.textTertiary)
                    .build();

                // ---- 列表区：当页行 + scrollView（440 > 视口时出滚动条） ----
                if (!callsPageLoaded_) {
                    // 弹窗首帧：读意图已发出、查询在途 → 显示加载态（不闪假空态）
                    ui.text("claude.dialog.calls.loading")
                        .position(28.0f, 112.0f)
                        .size(w - 56.0f, 20.0f)
                        .text("加载中…")
                        .fontSize(t.sizes.fontSizeSmall)
                        .lineHeight(20.0f)
                        .color(t.colors.textTertiary)
                        .build();
                } else if (allCalls_.empty()) {
                    ui.text("claude.dialog.calls.empty")
                        .position(28.0f, 112.0f)
                        .size(w - 56.0f, 20.0f)
                        .text("(无匹配的调用记录)")
                        .fontSize(t.sizes.fontSizeSmall)
                        .lineHeight(20.0f)
                        .color(t.colors.textTertiary)
                        .build();
                } else {
                    // contentKey = 当页 id 序列 hash（行替换也失效重测，W7 教训）
                    size_t rowsKey = 0;
                    for (const auto& c : allCalls_) {
                        rowsKey = rowsKey * 1315423911u + static_cast<size_t>(c.id);
                    }
                    const std::time_t now = std::time(nullptr);
                    const float scrollY = 100.0f;
                    const float scrollBottom = h - 60.0f - 12.0f;  // 底部按钮上方
                    components::scrollView(ui, "claude.dialog.calls.scroll")
                        .position(28.0f, scrollY)
                        .size(w - 56.0f, scrollBottom - scrollY)
                        .gap(0.0f)
                        .step(static_cast<float>(rowH))
                        .theme(themeTokens)
                        .contentKey("claude.dialog.calls.scroll." +
                                    std::to_string(rowsKey) + "." +
                                    std::to_string(allCalls_.size()))
                        .offset(callsScrollOffset_)
                        .onChange([this](float v) { callsScrollOffset_ = v; })
                        .content([&](eui::Ui& inner, float contentWidth,
                                     float viewportHeight) {
                            (void)viewportHeight;
                            for (size_t i = 0; i < allCalls_.size(); ++i) {
                                const ClaudeService::CallLogEntry& c = allCalls_[i];
                                const std::string rowId =
                                    "claude.dialog.calls.row." + std::to_string(i);
                                // 行包 stack（position 只在 stack 内生效，仿 todo）
                                inner.stack(rowId + ".wrap")
                                    .size(contentWidth, static_cast<float>(rowH))
                                    .content([&] {
                                        std::string line =
                                            formatCallTime(c.startedAt, now) + "  " +
                                            c.method + " " + c.path + "  " +
                                            (c.statusCode > 0
                                                 ? std::to_string(c.statusCode)
                                                 : std::string("-")) +
                                            "  " + formatLatency(c.latencyMs);
                                        // 无状态码行（超时/网络错误）行尾附错误摘要，
                                        // 截 48 字节防挤爆行宽
                                        if (c.statusCode <= 0 && !c.error.empty()) {
                                            line += "  " + truncateUtf8(c.error, 48);
                                        }
                                        const eui::Color rowColor =
                                            (c.statusCode >= 200 && c.statusCode < 300)
                                                ? t.colors.textSecondary
                                                : t.colors.error;
                                        inner.text(rowId)
                                            .position(0.0f, 2.0f)
                                            .size(contentWidth, 18.0f)
                                            .text(line)
                                            .fontSize(12.0f)
                                            .lineHeight(18.0f)
                                            .color(rowColor)
                                            .build();
                                    })
                                    .build();
                            }
                        })
                        .build();
                }

                // ---- 底部：翻页 + 页码 + 关闭 ----
                const int pages = std::max(
                    1, (callsTotalMatches_ + kCallsPageSize - 1) / kCallsPageSize);
                const bool hasNext =
                    (callsPage_ + 1) * kCallsPageSize < callsTotalMatches_;
                components::button(ui, "claude.dialog.calls.prev")
                    .position(28.0f, h - 60.0f)
                    .size(88.0f, 40.0f)
                    .text("← 上一页")
                    .fontSize(14.0f)
                    .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .disabled(callsPage_ == 0)
                    .onClick([this] {
                        if (callsPage_ == 0) return;
                        --callsPage_;
                        callsScrollOffset_ = 0;
                        markCallsStale(false);   // 翻页：下一帧后台重查（不防抖）
                    })
                    .build();
                components::button(ui, "claude.dialog.calls.next")
                    .position(128.0f, h - 60.0f)
                    .size(88.0f, 40.0f)
                    .text("下一页 →")
                    .fontSize(14.0f)
                    .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .disabled(!hasNext)
                    .onClick([this] {
                        if ((callsPage_ + 1) * kCallsPageSize >= callsTotalMatches_) return;
                        ++callsPage_;
                        callsScrollOffset_ = 0;
                        markCallsStale(false);   // 翻页：下一帧后台重查（不防抖）
                    })
                    .build();
                ui.text("claude.dialog.calls.page")
                    .position(228.0f, h - 60.0f)
                    .size(332.0f, 40.0f)
                    .text("第 " + std::to_string(callsPage_ + 1) + " / " +
                          std::to_string(pages) + " 页")
                    .fontSize(12.0f)
                    .lineHeight(40.0f)
                    .color(t.colors.textSecondary)
                    .build();
                components::button(ui, "claude.dialog.calls.close")
                    .position(w - 28.0f - 120.0f, h - 60.0f)
                    .size(120.0f, 40.0f)
                    .text("关闭")
                    .fontSize(14.0f)
                    .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] { callsDialogOpen_ = false; })
                    .build();
            })
            .build();
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
        dataDirty_ = true;   // 审计 perf [13]：写操作标脏
        auto r = svc->setActiveProfile(id);
        if (r.isFail()) {
            showToast("启用失败", r.errorMessage(), true);
            return;
        }
        selectedId_ = id;
        showToast("已启用", "该 Profile 已设为激活（ACTIVE）", false);
    }

    // 运行：确保代理 + 必要时切激活 + 在终端里开一个新的 claude 会话（D-1）
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
        // 成功：D-5 的退化提示（hint 非空时）+ D-9 的「会话不受 DevDesk 管理」
        const std::string head = hint.empty()
            ? std::string("已在新终端里打开 Claude Code 会话（起始目录 = 用户主目录）。")
            : (hint + std::string("。"));
        showToast("已启动",
                  head + "运行中的会话不受 DevDesk 管理，停代理/退出后可能失败。",
                  false);
        dataDirty_ = true;   // 可能刚起代理 / 刚切激活 → 下帧重查（状态点、ACTIVE 徽标）
    }

    // 复制（设计 §4.1/§4.2，M5）：建副本但不激活 → 选中态/激活态都不动
    void doDuplicate(int64_t id) {
        ClaudeService* svc = ClaudeService::global();
        if (!svc) return;
        dataDirty_ = true;   // 审计 perf [13]：写操作标脏
        auto r = svc->duplicateProfile(id);
        if (r.isFail()) {
            showToast("复制失败", r.errorMessage(), true);
            return;
        }
        showToast("已复制",
                  "已创建副本「" + r.value().name + "」（未激活）", false);
    }

    void doDelete(int64_t id) {
        ClaudeService* svc = ClaudeService::global();
        if (!svc) return;
        dataDirty_ = true;   // 审计 perf [13]：写操作标脏
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
        if (selectedId_ == id) selectedId_ = 0;
        if (proxyStopped) {
            showToast("已删除（代理已停止）",
                      "删除的是激活 Profile，已停止代理并还原 Claude Code 配置",
                      false);
        } else {
            showToast("已删除", "Profile 已删除（含 Keychain 条目）", false);
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

    int64_t selectedId_ = 0;   // 0 = 跟随激活 profile
    int detailTab_ = 0;       // 详情页签：0 基础 / 1 模型映射 / 2 环境变量（2026-09-24）

    // 审计 perf [13]：compose 数据缓存（刷新语义见 refreshDataCache 注释）。
    // 初值脏 = 首帧 compose 必查；seenCallLogVersion_ 初值 ~0 保证 recent
    // 首帧必查（真实版本号从 0 起，不会撞上）。
    std::vector<Profile> cachedProfiles_;
    std::string cachedProfilesError_;
    std::vector<ClaudeService::CallLogEntry> cachedRecent_;
    std::string cachedCallsError_;
    Profile cachedActive_;
    bool cachedHasActive_ = false;
    bool dataDirty_ = true;
    bool dataLoaded_ = false;   // 快照是否装载过（冷启动兜底判据，见 refreshDataCache）
    uint64_t seenCallLogVersion_ = ~0ull;
    // 审计 H2：后台刷新在途 future（工作线程不触碰 this，结果经值回渲染线程；
    // 成员析构 = future 析构，自动等本轮查询结束 → 无悬垂）
    std::future<DataRefresh> refreshFuture_;
    std::chrono::steady_clock::time_point lastRefreshPost_{};  // 节流窗口起点
    static constexpr int64_t kRefreshIntervalMs = 200;         // 刷新节流窗口

    // 新建/编辑表单（tabs 三页，§6.1）
    bool formOpen_ = false;
    int64_t formEditId_ = 0;   // 0 = 新建，非 0 = 编辑该 id
    int formTab_ = 0;          // 0 基础 / 1 模型映射 / 2 高级 env
    std::string formName_;
    std::string formUrl_;
    std::string formKey_;
    std::string formKeyHint_;  // 编辑时显示现有 key 脱敏提示
    std::string formError_;
    std::string formImportWarn_;  // 导入当前配置的接管态警告（弹窗内展示，不阻断）
    AuthField formAuthField_ = AuthField::Both;  // 认证字段（鉴权头三态）
    bool formAuthFieldOpen_ = false;             // 下拉展开态
    ModelsConfig formModels_;  // 槽位表全行（默认模型 = def 槽位，旧 formModel_ 并入）
    std::vector<std::pair<std::string, std::string>> formEnvRows_;  // env 键值行

    // cc-switch 粘贴导入（高级 env 页）
    std::string importText_;       // 多行粘贴框原文
    bool importReady_ = false;     // 解析成功待应用
    std::string importError_;      // 解析失败信息（红字）
    ImportedConfig importPreview_; // 解析结果预览

    // 引导条一次性标记（§6.4：会话内代理从未启动过才显示；内存态不持久化）
    bool proxyEverSeen_ = false;

    // 删除确认
    bool deleteConfirmOpen_ = false;
    int64_t deleteTargetId_ = 0;
    std::string deleteTargetName_;

    // 完整调用记录弹窗（审计 [11]：搜索/状态码过滤/翻页共用以下查询态）
    bool callsDialogOpen_ = false;
    std::vector<ClaudeService::CallLogEntry> allCalls_;  // 快照：当页行（渲染只读）
    std::string callsSearch_;        // 搜索框文本（path/method 模糊）
    int callsStatusIdx_ = 0;         // 状态下拉索引（→ kCallsStatusClasses）
    bool callsStatusDdOpen_ = false; // 状态下拉展开态
    int callsPage_ = 0;              // 当前页（0-based）
    int callsTotalMatches_ = 0;      // 快照：满足过滤的总条数（countCalls）
    float callsScrollOffset_ = 0.0f; // 列表区滚动位置（翻页时归零）
    // 审计 CPX-06：调用记录取数的 future 刷新态（同上面 refreshFuture_ 一组，
    // 语义见 refreshCallsCache）。初值脏 = 首次打开弹窗下一帧必查
    bool callsPageDirty_ = true;     // 置脏 → 下一帧（弹窗打开时）发一次读意图
    bool callsPageLoaded_ = false;   // 快照是否装载过（未装载显示「加载中…」）
    std::chrono::steady_clock::time_point callsEarliestQueryAt_{};  // 最早派发时刻
    // 后台查询在途 future（工作线程不触碰 this，结果经值回渲染线程；成员析构
    // = future 析构，自动等本轮查询结束 → 无悬垂）
    std::future<CallsPage> callsFuture_;

    // Profile 列表滚动位置（审计 [26]：卡片超界可滚，不再压底部按钮）
    float profilesScrollOffset_ = 0.0f;

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

// ============ 审计 CPX-06 测试缝（生产零调用点） ============
// 照 core/proxy_stats.h resetForTest / platform/file_open_win.h
// wideToUtf8ForTest 先例：把「调用记录后台查询」的派发次数与执行线程暴露给
// 无头用例，好让「弹窗打开那一帧只发意图（不发生同步查询）」「搜索防抖把逐键
// 的多次意图合并成一次派发」成为计数断言而不靠墙钟。extern "C" 免名字修饰，
// 测试侧写同名声明即可链接（计数器定义见文件头 g_callsQueryDispatches）
extern "C" int claude_calls_query_dispatches_for_test() {
    return module::claude::g_callsQueryDispatches.load();
}

extern "C" unsigned long long claude_calls_query_last_thread_for_test() {
    return module::claude::g_callsQueryDispatchThread.load();
}

extern "C" ::core::Module* create_claude_module() {
    return new module::claude::ClaudeModule();
}
