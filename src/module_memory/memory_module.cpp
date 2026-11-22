// memory_module.cpp
// 记忆页模块装配与主控台：生命周期、compose 骨架、引擎卡、健康格。
// 三拆（2026-10-31）：本文件 = 装配 + 主视图；记忆审核/配置 → memory_module_panels.cpp；
// 弹窗 → memory_module_modals.cpp。视觉与交互契约 = docs/mockups/memcpp-panel-demo.html，
// 偏差处（预算环→双横条、来源枚举对齐数据层）在控件旁注明口径。

#include "memory_module_internal.h"

#include <ui_kit/text_metrics.h>  // cachedTextWidth（M9 TAB 估宽统一入口）

#include "core/tool.h"
#include "module_memory/agent_tools.h"
#include "module_memory/engine/telemem_config.h"

namespace module::memory {

::core::PageId MemoryModule::pageId() const { return ::core::PageId::Memory; }

std::string MemoryModule::displayName() const { return "记忆"; }

std::string MemoryModule::shortName() const { return "记忆"; }

// fa-brain (U+F5DC) 的 UTF-8 字节；须与 app_registry 表行 iconGlyph 列同值。

std::string MemoryModule::iconGlyph() const { return "\xEF\x97\x9C"; }

void MemoryModule::pollUsageAlerts() { panel_.pollUsageAlerts(); }

void MemoryModule::onStart() {
    LOG_INFO("记忆模块已启用：引擎未启动时页面显引导态，零后台成本");
    // memory.stats / memory.health：只读诊断工具，注册跟随模块启用（need 219
    // 拍板 3）。路径与 panel_.openStores 同源（memoryDataDir），不依赖页面开过。
    if (!agentToolsRegistered_) {
        memStatsTool_ = makeMemoryStatsTool(
            engine::memoryDataDir() / "proposals.db",
            engine::memoryDataDir() / "usage_ledger.db");
        memHealthTool_ = makeMemoryHealthTool();
        auto regStats =
            ::core::ToolRegistry::instance().registerTool(memStatsTool_);
        auto regHealth =
            ::core::ToolRegistry::instance().registerTool(memHealthTool_);
        if (regStats.isFail()) {
            LOG_WARN("记忆: memory.stats 注册失败: " + regStats.errorMessage());
        }
        if (regHealth.isFail()) {
            LOG_WARN("记忆: memory.health 注册失败: " + regHealth.errorMessage());
        }
        agentToolsRegistered_ = regStats.isOk() && regHealth.isOk();
    }
    // F1 产品接线：开页即同步采集循环——引擎已 Running 则立即起跑
    //（compose 每帧 refresh 也会同步；此处覆盖"启动引擎后未再切页"的首帧）
    panel_.refresh(true);
    refreshLlmKeyState();  // need256：app_secrets → 掩码缓存（配置卡两态数据源）
}

void MemoryModule::onStop() {
    // 工具随模块停用注销（先注销再放实例，避免在途调用悬垂）
    if (agentToolsRegistered_) {
        ::core::ToolRegistry::instance().unregisterTool("memory.stats");
        ::core::ToolRegistry::instance().unregisterTool("memory.health");
        agentToolsRegistered_ = false;
    }
    memStatsTool_.reset();
    memHealthTool_.reset();
    // 页面关闭 ≠ 引擎停止：引擎是应用级常驻服务，生命周期归 TelememEngine。
    LOG_INFO("记忆页已关闭（引擎不受影响）");
}

void MemoryModule::showToast(const std::string& title, const std::string& body,
                             bool error) {
    toast_.show(title, body, error);
}

// ── need256：提炼 API key 配置卡（app_secrets 通道）──

void MemoryModule::refreshLlmKeyState() {
    llmKeyMask_ = engine::maskLlmKey(engine::readLlmKeyFromStore());
    if (llmKeyMask_.empty()) llmKeyEditing_ = true;  // 未配置默认展开输入行
}

void MemoryModule::saveConfiguredLlmKey() {
    if (llmKeyInput_.empty()) {
        showToast("未保存", "请先粘贴 API key 再点保存", true);
        return;
    }
    if (engine::saveLlmKeyToStore(llmKeyInput_)) {
        llmKeyInput_.clear();
        llmKeyEditing_ = false;
        refreshLlmKeyState();
        showToast("已保存", "提炼 API key 已存入本机数据库");
    } else {
        showToast("保存失败", "密钥写入数据库失败，请重试", true);
    }
}

void MemoryModule::clearConfiguredLlmKey() {
    engine::clearLlmKeyFromStore();
    refreshLlmKeyState();
    showToast("已清除", "提炼 API key 已从本机数据库删除");
}

void MemoryModule::compose(eui::Ui& ui, const eui::Screen& screen) {
    const auto& t = core::theme::current();
    const auto ct = core::theme::componentTokensWeChat(t);
    const float pagePad = t.sizes.spacingLarge * 2.0f;
    const float contentW = std::max(0.0f, screen.width - pagePad * 2.0f);

    panel_.refresh();

    ui.text("memory.title")
        .position(pagePad, 0.0f)
        .size(contentW, kTitleH)
        .text("记忆")
        .fontSize(t.sizes.fontSizeTitle)
        .lineHeight(kTitleH)
        .fontWeight(600)  // need219 列表统一规范 §2.1：页头两档制 600，无 700
        .color(t.colors.textPrimary)
        .build();

    const float tabY = kTitleH + t.sizes.spacingLarge;
    const auto snap = panel_.snapshot();
    char reviewLabel[48];
    std::snprintf(reviewLabel, sizeof(reviewLabel), "记忆审核 (%d)",
                  static_cast<int>(snap.pending.size()));
    const char* tabLabels[kViewConfig + 1] = {"主控台", reviewLabel, "配置"};
    // need298 M9：TAB 宽按文字实宽自适应（wallpaper_tabs 同法 +32；估宽统一
    // 入口 cachedTextWidth，字重取选中态 600 最宽档），待审数 4 位不再裁字。
    float tabX = pagePad;
    for (int i = kViewConsole; i <= kViewConfig; ++i) {
        const int view = i;
        const float tabW = reviewTabW(ui_kit::cachedTextWidth(
            tabLabels[i], core::theme::scaled(ui_kit::kWtabFontSize), 600));
        ui_kit::wtab(ui, "memory.tabs." + std::to_string(i), tabLabels[i],
                     activeView_ == i, tabX, tabY, tabW, kTabH, t,
                     [this, view] { activeView_ = view; });
        tabX += tabW + 4.0f;
    }

    const float bodyY = tabY + kTabH + t.sizes.spacingLarge;

    // 三视图各套滚动容器（卡区总高可超小窗可视高；settings 卡区同款）。
    // bug158/160（2026-11-03）：滚动根必须 .position(pagePad, bodyY)——不设
    // position 时根从页顶 (0,0) 铺满，scrollState 置 interactive 使整面可命中、
    // 后建兄弟先命中，盖住 TAB 行吞点击；内容高测量也含 bodyY 偏移，滚动条
    // 轨道从页顶起步、比例失真。内容子元素相应改 0 基相对坐标，滚动位移全交
    // EUI scrollContentFrom 自动平移（内容里再手动减 offset = 双重平移）。
    const char* scrollIds[kViewConfig + 1] = {"memory.console.scroll",
                                              "memory.review.scroll",
                                              "memory.config.scroll"};
    const int view = activeView_;
    auto drawView = [&](eui::Ui& inner, float cw, float vh) {
        // 画布栈：滚动内容列的子元素会被列布局重排，绝对定位必须收进一层 stack
        inner.stack("memory.view.canvas")
            .width(cw)
            .height(core::SizeValue::wrapContent())
            .content([&] {
                if (view == kViewConsole) {
                    composeConsole(inner, t, 0.0f, cw, 0.0f, vh);
                } else if (view == kViewReview) {
                    composeReview(inner, t, 0.0f, cw, 0.0f, vh);
                } else {
                    composeConfig(inner, t, 0.0f, cw, 0.0f, vh);
                }
            })
            .build();
    };
    float* offset = view == kViewConsole
                        ? &consoleScroll_
                        : (view == kViewReview ? &reviewScroll_ : &configScroll_);
    // contentKey 拼入配置 tab 折叠态签名（need:335）：MeasureCache 键 = (宽,
    // 视口高, gap, key)，固定键在折叠组展开/折叠后命中旧测量——内容高不重算、
    // scrollable 不重判。配置视图已由下方 contentHeightProvider 供高（缓存
    // 被绕过），此签名保语义正确：未来若撤 provider，折叠态变化仍触发重测。
    std::string contentKey = "memory.view" + std::to_string(view);
    if (view == kViewConfig) {
        for (bool open : cfgOpen_) {
            contentKey += open ? '1' : '0';
        }
    }
    // 滚动根定位于 TAB 行下方（页头/TAB 不滚；调度页同款），内容 0 基相对坐标
    auto scroll = components::scrollView(ui, scrollIds[view])
        .position(pagePad, bodyY)
        .size(contentW, std::max(120.0f, screen.height - bodyY - 12.0f))
        .step(60.0f)
        .theme(ct)
        .contentKey(contentKey)
        .offset(*offset)
        .onChange([offset](float v) { *offset = v; });
    if (view == kViewConfig) {
        // 内容高供给缝（need:335）：配置 tab 走渲染 pass 回写的实占底，绕开
        // EUI stack wrapContent 对绝对定位子元素的偏小测量（实测声明几何
        // 1424 被解析为 1320，尾组 104px 不可达）。上帧值首帧兜底 vh，
        // 第二帧起与渲染同源；其它视图不挂 provider，原缓存测量路径不变。
        scroll.contentHeightProvider([this](float, float vh) {
            return std::max(vh, cfgContentBottom_);
        });
    }
    scroll.content(drawView).build();

    ui_kit::drawToast(ui, screen, "memory.toast", toast_, ct);

    // 弹窗层（最后画；open 由各自状态位控制）
    composeEditDialog(ui, screen, t, ct);
    composeMaskModal(ui, screen, t, ct);
    composeClearModal(ui, screen, t, ct);
    composeWizardModal(ui, screen, t, ct);
    composeEngineDetailModal(ui, screen, t, ct);
    composeFullTextDialog(ui, screen, t, ct);
}

// ── 视图 1：主控台 ──

void MemoryModule::composeEngineCard(eui::Ui& ui, const core::theme::Tokens& t,
                                     float x, float w, float y) {
    const auto snap = panel_.snapshot();
    const auto es = snap.engine;
    const eui::Color stateColor = engineStateColor(t, es.state);

    // need:181 三 TAB 分层（设计 §2.2）：引擎卡 196 → 64px 状态带。低频内容
    // （说明文案 / python 检测 / 语义档 / 版本号）移入「详情」弹窗（L3）；
    // 异常态前置警示短语到带上（L1 可见，细节进弹窗）。
    const float bandH = core::theme::scaled(kEngineBandH);
    ui.stack("memory.eng.root")
        .position(x, y)
        .size(w, bandH)
        .content([&] {
    ui_kit::cardPanel(ui, "memory.eng.card", 0.0f, 0.0f, w, bandH, t.sizes.radiusLarge, t);
    ui.rect("memory.eng.dot")
        .position(t.sizes.spacingLarge, (bandH - 10.0f) * 0.5f)
        .size(10.0f, 10.0f)
        .color(stateColor)
        .radius(t.sizes.radiusPill)
        .build();
    ui.text("memory.eng.title")
        .position(t.sizes.spacingLarge + 18.0f, 0.0f)
        .size(170.0f, bandH)
        .text("telemem 记忆引擎")
        .fontSize(t.sizes.fontSizeBody)
        .lineHeight(bandH)
        .fontWeight(600)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textPrimary)
        .build();
    ui.text("memory.eng.state")
        .position(212.0f, 0.0f)
        .size(90.0f, bandH)
        .text(engineStateText(es.state))
        .fontSize(core::theme::scaled(12.0f))
        .lineHeight(bandH)
        .fontWeight(600)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(stateColor)
        .build();
    // 异常前置：Failed / 本机模型缺失时警示短语直接上带（细节进详情弹窗）。
    // need298 M4：短语宽度 = 右端按钮组左缘 −8（超长省略），窄窗不再与按钮叠印。
    const bool failed = es.state == TelememEngine::State::Failed;
    const bool wizardInline =
        es.state == TelememEngine::State::Idle && !es.pythonFound;
    if (failed || es.llamaMissing) {
        const float warnW = engineBandWarnW(w, wizardInline);
        const std::string warnText = failed ? "引擎故障 · 点「详情」看原因"
                                            : "本机模型文件缺失";
        ui.text("memory.eng.warn")
            .position(310.0f, 0.0f)
            .size(std::max(0.0f, warnW), bandH)
            .text(truncateByAdvance(warnText, std::max(0.0f, warnW),
                                    core::theme::scaled(11.0f)))
            .fontSize(core::theme::scaled(11.0f))
            .lineHeight(bandH)
            .fontWeight(600)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.warning)
            .build();
    }

    // 右端按钮组（右锚 gap 8，高 36 垂直居中）：详情常驻；启停随状态；
    // 向导仅未安装引导态（Idle + 未检出 Python）直出
    const bool busy = es.state == TelememEngine::State::Preparing;
    const bool running = es.state == TelememEngine::State::Running;
    const float btnH = 36.0f;
    const float btnY = (bandH - btnH) * 0.5f;
    float rightX = w - 16.0f - 120.0f;  // 主操作（启停）右缘对齐
    std::function<void()> toggle;
    if (!busy) {
        if (running) {
            toggle = [] { TelememEngine::global().stopAsync(); };
        } else {
            toggle = [] { TelememEngine::global().startAsync(); };
        }
    }
    if (running) {
        ui_kit::button(ui, "memory.eng.toggle", "停止引擎",
                       ui_kit::ButtonLevel::Danger, x + rightX, y + btnY,
                       120.0f, btnH, t, {toggle});
    } else {
        primaryButton(ui, "memory.eng.toggle", busy ? "启动中…" : "启动引擎",
                      x + rightX, y + btnY, 120.0f, btnH, t,
                      busy ? nullptr : toggle);
    }
    if (wizardInline) {
        rightX -= 8.0f + 150.0f;
        // need294 白卡专项：Secondary 常态 surfaceElevated 落状态带白底不可辨，
        // opt-in 1px borderDefault 描边（见 ui_kit ButtonOptions 注）
        ui_kit::ButtonOptions wizardOpts;
        wizardOpts.onClick = [this] {
            wizardOpen_ = true;
            // 向导③步即时检查（need:202 F-04）：本地文件探测一次，
            // idle 态下刷新 embed* 快照字段（管线态不受影响）
            TelememEngine::global().refreshEmbedProbe();
        };
        wizardOpts.hasBorderColor = true;
        wizardOpts.borderColor = t.colors.borderDefault;
        ui_kit::button(ui, "memory.eng.wizard", "首次安装向导",
                       ui_kit::ButtonLevel::Secondary, x + rightX, y + btnY,
                       150.0f, btnH, t, wizardOpts);
    }
    rightX -= 8.0f + 64.0f;
    ui_kit::button(ui, "memory.eng.detail", "详情",
                   ui_kit::ButtonLevel::Text, x + rightX, y + btnY, 64.0f,
                   btnH, t, {[this] { engineDetailOpen_ = true; }});
    });
}


} // namespace module::memory

// 默认开启的普通页面模块：工厂恒非空（无平台能力门槛）。
extern "C" ::core::Module* create_memory_module() {
    return new module::memory::MemoryModule();
}
