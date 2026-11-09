// src/module_mcp/mcp_page_module.cpp
// MCP 菜单页实现：页头固定（标题/副标题/状态行/记忆模块行/区头）→ 工具卡片网格
//（ui.flow，列宽自适应 340–420）在列表容器内自滚（池144：外壳不滚、内容区唯一
// 自滚）→ 详情弹窗（components::dialog，关闭 = 按钮 + Esc + backdrop 三通道）→
// 「试一下」（池146 参数表单校验 + execute 后台线程 + compose 轮询收割，禁 UI
// 线程直调）。
// 视觉基准 = 桌面 mcp-menu-demo.html（token 对齐，图标 FA7 码点；页内开关与
// 关闭置灰不照抄——关闭态 = 模块门控整页注销）。
#include "mcp_page_module.h"

#include "core/component_theme.h"
#include "core/logging.h"
#include "core/paths.h"  // need244：~/.claude.json 定位（claudeConfigFile）
#include "core/theme.h"
#include "core/tool.h"
#include "mcp_health_tool.h"
#include "mcp_service_bridge.h"
#include "memory_mcp_bridge.h"

#include <core/platform/platform.h>  // core::platform::requestUiUpdate（唤醒主循环）

#include <components/button.h>
#include <components/dialog.h>
#include <components/input.h>
#include <components/scrollview.h>
#include <components/switch.h>
#include <components/theme.h>

#include <core/render/text.h>  // core::TextPrimitive::measureTextWidth

#include <eui/dsl.h>

#include <platform/http_client.h>  // mcp.health 回环探活（need 219）
#include <platform/json.h>  // 池146：解析 inputSchema / 组装试跑入参

#include <ui_kit/empty_state.h>
#include <ui_kit/glass.h>  // glassBorder / glassShadowColor（池235：卡底补齐玻璃三件套）
#include <ui_kit/grid.h>
#include <ui_kit/materials.h>
#include <ui_kit/wswitch.h>

#include <algorithm>
#include <cstdlib>  // strtoll / strtod（不抛异常的数字解析）
#include <utility>

namespace module::mcp {

namespace {

namespace pj = ::platform::json;

// mcp.health（need 219 F-05）：hub 只读诊断工具。状态源走 app::mcp 桥
//（settings 同款函数指针缝）而非直接依赖 McpService——本 TU 被探针 /
// ui_driver / github_sync_live 直编且链接集没有 devdesk_module_mcp，引库符号
// 即断链。运行中时回环 POST initialize 探活（1.5s 超时，连接拒绝即失败，
// 不挂起）；判据只看 HTTP 200（hub 为本仓实现，JSON-RPC 语义由 mcp_protocol 自管）。
constexpr long kMcpHealthProbeTimeoutMs = 1500;
constexpr const char* kMcpHealthProbeBody =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
    "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
    "\"clientInfo\":{\"name\":\"devdesk-health\",\"version\":\"1.0\"}}}";

class McpHealthTool : public ::core::ITool {
public:
    std::string name() const override { return "mcp.health"; }

    std::string schemaJson() const override {
        return R"({"name":"mcp.health","description":"本机 MCP 服务健康检测（监听状态 / 注册工具数 / 回环探活），只读","input_schema":{"type":"object","properties":{}}})";
    }

    ::core::Result<::core::ToolResult> execute(
        const ::core::ToolArgs& args) override {
        (void)args;  // 无参数
        const auto live = app::mcp::status();  // running/port/lastError
        const char* state = live.running ? "running"
                            : !live.lastError.empty() ? "failed"
                                                      : "stopped";

        pj::JsonObject probe;
        if (live.running && live.port > 0) {
            platform::http::Request req;
            req.method = "POST";
            req.url = "http://127.0.0.1:" + std::to_string(live.port) + "/mcp";
            req.body = kMcpHealthProbeBody;
            req.headers["Content-Type"] = "application/json";
            req.headers["Accept"] = "application/json, text/event-stream";
            req.timeoutMs = kMcpHealthProbeTimeoutMs;
            const auto r = platform::http::send(req);
            if (!r.isOk()) {
                probe.set("ok", pj::makeBool(false));
                probe.set("reason", pj::makeString(r.errorMessage()));
            } else {
                const bool ok = r.value().statusCode == 200;
                probe.set("ok", pj::makeBool(ok));
                probe.set("reason",
                          pj::makeString(ok ? ""
                                            : "HTTP " + std::to_string(
                                                          r.value().statusCode)));
            }
        } else {
            probe.set("ok", pj::makeBool(false));
            probe.set("reason",
                      pj::makeString(!live.lastError.empty()
                                         ? live.lastError
                                         : "服务未启动"));
        }

        pj::JsonObject o;
        o.set("state", pj::makeString(state));
        o.set("port", pj::makeInt(static_cast<int64_t>(live.port)));
        o.set("tools", pj::makeInt(static_cast<int64_t>(
                           core::mcp::globalMcpTools().size())));
        o.set("lastError", pj::makeString(live.lastError));
        o.set("probe", pj::JsonValue(std::move(probe)));
        auto text = pj::stringify(pj::JsonValue(std::move(o)));
        if (text.isFail()) {
            return ::core::Result<::core::ToolResult>::fail(
                core::ErrorCode::Internal,
                "健康结果序列化失败: " + text.errorMessage());
        }
        return ::core::Result<::core::ToolResult>::ok(
            ::core::ToolResult{false, text.value()});
    }
};

} // namespace

std::shared_ptr<::core::ITool> makeMcpHealthTool() {
    return std::make_shared<McpHealthTool>();
}

namespace {

// 工具卡列宽算法（2026-11-01 F-13）：
// 原为定宽 340，1600 窗内只排 3 张（1020px），右侧空 210px。现在列宽随内容宽
// 自适应：列数按「能放下多少列最小宽」取，卡宽 = (内容宽 - 列间距×(列数-1)) / 列数。
// 区间锁 340–420：太窄描述挤、太宽一行字飘。
constexpr float kCardMinW = 340.0f;
constexpr float kCardMaxW = 420.0f;
constexpr int kCardMaxCols = 5;
constexpr float kCardH = 120.0f;
constexpr float kDialogW = 600.0f;
constexpr float kDialogH = 480.0f;

// 卡内横向常量
constexpr float kCardPad = 16.0f;       // 卡内边距
constexpr float kCardIcon = 40.0f;      // 图标位
constexpr float kCardTextX = 68.0f;     // 文本列起点（图标右侧 12）
constexpr float kStatusGap = 16.0f;     // 文本列右端 ↔ 状态列左端（F-01 判据 ≥16）
constexpr float kChipW = 40.0f;         // 标签 chip 宽
constexpr float kChipH = 18.0f;
constexpr float kCtaW = 120.0f;         // hover CTA 宽（右对齐）

// 工具卡图标：按名字族派发 FA7 Free-Solid 码点（抓取=globe / 服务信息=server /
// 其余=cube）；码点均在 EUI-NEO 内置 FA7 整字体覆盖内。
unsigned int toolIconGlyph(const std::string& name) {
    if (name.find("fetch") != std::string::npos ||
        name.find("web") != std::string::npos) {
        return 0xF0AC;  // fa-globe
    }
    if (name.find("server") != std::string::npos ||
        name.find("info") != std::string::npos) {
        return 0xF233;  // fa-server
    }
    return 0xF1B2;  // fa-cube
}

// 结果区文案单源：运行中 / 尚未运行 / 成功前缀 / 失败前缀（测试按前缀断言）。
std::string resultDisplayText(bool running, bool hasResult, bool isError,
                              const std::string& text) {
    if (running) return "运行中…";
    if (!hasResult) return "（尚未运行）";
    return std::string(isError ? "失败：\n" : "成功：\n") + text;
}

// 结果文本按行拆分（bug145：输出区按行滚动）；行数上限 500（超长输出防每帧
// 元素爆炸，运行日志卡同口径），超出截断并尾注总行数。
constexpr std::size_t kResultMaxLines = 500;
std::vector<std::string> splitResultLines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (lines.size() < kResultMaxLines) {
        const std::size_t pos = text.find('\n', start);
        if (pos == std::string::npos) {
            lines.push_back(text.substr(start));
            return lines;
        }
        lines.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    const std::size_t total =
        static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
    lines.back() = "…（已截断，共 " + std::to_string(total) + " 行）";
    return lines;
}

} // namespace

McpPageModule::McpPageModule() : McpPageModule(::core::mcp::globalMcpTools()) {}

McpPageModule::McpPageModule(::core::mcp::McpToolRegistry& registry)
    : registry_(&registry) {}

McpPageModule::~McpPageModule() { joinTryThread(); }

::core::PageId McpPageModule::pageId() const { return ::core::PageId::Mcp; }

std::string McpPageModule::displayName() const { return "MCP"; }

std::string McpPageModule::shortName() const { return "MCP"; }

// fa-plug (U+F1E6) 的 UTF-8 字节；须与 app_registry 表行 iconGlyph 列同值。
std::string McpPageModule::iconGlyph() const { return "\xEF\x87\xA6"; }

void McpPageModule::compose(eui::Ui& ui, const eui::Screen& screen) {
    harvestTryRun();
    const auto& t = core::theme::current();
    const auto comp = core::theme::componentTokensWeChat(t);
    const float pad = t.sizes.spacingLarge * 2.0f;  // 32：页面外边距与他页同档
    const auto tools = registry_->list();           // 每帧快照（单锁，量小）

    // 服务状态行数据源：app::mcp 桥只读快照（本页不提供开关，开关在设置页）。
    const app::mcp::LiveStatus svc = app::mcp::status();

    // 池144（口径 = 池140 设计 §2 滚动模型）：整页滚动废除——外壳不滚，页头五行
    //（标题/副标题/状态行/记忆模块行/区头）固定全程可视，工具网格（或空态）是
    // 唯一滚动区、列表容器内自滚。
    const float contentW = std::max(0.0f, screen.width - pad * 2.0f);
    // 固定区高 = 顶 padding(0) + 五行盒（标题32/副标题20/状态20/记忆行64/区头26，
    // 全 scaled）+ 5 个 gap；列表区高 = 余高 − 底 padding，下限 scaled(120)（极矮
    // 窗仍可滚不塌陷）。
    const float headerH =
        core::theme::scaled(32.0f) + core::theme::scaled(20.0f) +
        core::theme::scaled(20.0f) + core::theme::scaled(64.0f) +
        core::theme::scaled(26.0f) + t.sizes.spacingLarge * 5.0f;
    const float listH = std::max(core::theme::scaled(120.0f),
                                 screen.height - headerH -
                                     t.sizes.spacingXLarge);

    // ⚠ 上游坑（home_module 同款）：column 流式子元素只看流式游标，flow 子元素
    // 必须显式 size()。
    ui.column("mcp.root")
        .width(screen.width)
        .height(core::SizeValue::wrapContent())
        .padding(pad, 0.0f, pad, t.sizes.spacingXLarge)
        .gap(t.sizes.spacingLarge)
        .content([&] {
                    ui.text("mcp.title")
                        .size(contentW, core::theme::scaled(32.0f))
                        .text("MCP 服务")
                        .fontSize(t.sizes.fontSizeTitle)
                        .fontWeight(600)
                        .color(t.colors.textPrimary)
                        .build();
                    ui.text("mcp.subtitle")
                        .size(contentW, core::theme::scaled(20.0f))
                        .text("将 DevDesk 内置能力以标准 MCP 协议（Streamable HTTP）"
                              "暴露给本机客户端；仅监听回环地址，不对局域网开放。")
                        .fontSize(core::theme::scaled(12.5f))
                        .color(t.colors.textTertiary)
                        .build();

                    // 服务状态行：静态状态点（pulse 动画明确不做）
                    ui.row("mcp.status")
                        .size(contentW, core::theme::scaled(20.0f))
                        .alignItems(eui::Align::CENTER)
                        .gap(t.sizes.spacingSmall)
                        .content([&] {
                            ui.rect("mcp.status.dot")
                                .size(8.0f, 8.0f)
                                .radius(t.sizes.radiusSmall)
                                .color(svc.running ? t.colors.success
                                                   : t.colors.textDisabled)
                                .build();
                            const std::string statusText =
                                svc.running
                                    ? "运行中 · 127.0.0.1:" +
                                          std::to_string(svc.port)
                                    : (svc.lastError.empty()
                                           ? "已停止"
                                           : "已停止 · " + svc.lastError);
                            ui.text("mcp.status.text")
                                .size(std::max(0.0f, contentW - 16.0f),
                                      core::theme::scaled(20.0f))
                                .text(statusText)
                                .fontSize(core::theme::scaled(12.5f))
                                .color(svc.running ? t.colors.textSecondary
                                                   : t.colors.textTertiary)
                                .build();
                        })
                        .build();

                    // 「记忆」模块行：向 MCP 贡献 memory_search 工具的模块开关
                    // （关 = 立即注销工具；经 app::memory_mcp 桥操作，不触 module_memory）
                    drawMemoryModuleRow(ui, contentW, t, comp);

                    // 区头：标题 + 计数 chip
                    // 2026-11-01 F-13：原标题占满 contentW-40，chip 被推到页面最右，
                    // 与左侧标题相距 700px，关联性为零。改为「量出标题实际宽度 →
                    // chip 紧随其后」，行内间距固定 8px。
                    const std::string sectionTitle = "注册的工具";
                    const float titleW = core::TextPrimitive::measureTextWidth(
                        sectionTitle, "", t.sizes.fontSizeLarge, 600);
                    const std::string countText = std::to_string(tools.size());
                    const float countW =
                        core::TextPrimitive::measureTextWidth(
                            countText, "", core::theme::scaled(11.0f), 400) +
                        14.0f;

                    ui.row("mcp.section")
                        .size(contentW, core::theme::scaled(26.0f))
                        .alignItems(eui::Align::CENTER)
                        .gap(8.0f)
                        .content([&] {
                            ui.text("mcp.section.title")
                                .size(titleW, core::theme::scaled(26.0f))
                                .text(sectionTitle)
                                .fontSize(t.sizes.fontSizeLarge)
                                .fontWeight(600)
                                .color(t.colors.textPrimary)
                                .build();
                            ui.rect("mcp.section.count.bg")
                                .size(countW, core::theme::scaled(18.0f))
                                .radius(t.sizes.radiusPill)
                                .color(t.colors.primarySoftBg)
                                .build();
                            ui.text("mcp.section.count")
                                .size(countW, core::theme::scaled(18.0f))
                                .text(countText)
                                .fontSize(core::theme::scaled(11.0f))
                                .color(t.colors.primaryText)
                                .horizontalAlign(eui::HorizontalAlign::Center)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .build();
                        })
                        .build();

                    // 列表滚动区（池144 唯一滚动区）：网格 / 空态在容器内自滚，
                    // 页头不随内容滚出视野。
                    components::scrollView(ui, "mcp.list.scroll")
                        .size(contentW, listH)
                        .theme(comp)
                        .content([&](eui::Ui& listUi, float listW, float listVh) {
                            (void)listVh;
                            if (tools.empty()) {
                                // 空态：走全站统一组件（图标 + 主句 + 引导句 + 居中）
                                // 2026-11-01 F-14：原为单行居中文字，无图标无引导。
                                ui_kit::EmptyStateOptions emptyOpts;
                                emptyOpts.message = "暂无已注册的 MCP 工具";
                                emptyOpts.guide =
                                    "工具由各模块在启动时注册；启用「记忆」等模块后，"
                                    "对应工具会自动出现在这里。";
                                emptyOpts.icon = 0xF1B2;  // fa-cube
                                ui_kit::emptyState(listUi, "mcp.empty", emptyOpts,
                                                   0.0f, 0.0f, listW,
                                                   core::theme::scaled(220.0f), t);
                                return;
                            }
                            // 网格：列宽随内容宽自适应（340–420），填满可用宽度，
                            // 不再在右侧留大片空白（2026-11-01 F-13）。
                            const auto [cols, cardW] = ui_kit::gridColumns(
                                listW, 8.0f, kCardMinW, kCardMaxW,
                                kCardMaxCols);
                            // 卡间距统一档 8px（列表统一规范 §4.3，need224）：EUI flow
                            // 单 gap 横竖同轴、无 rowGap 能力 → 行包 row 容器（设计 §2 F3
                            // 路径②）：行内/行间 gap 均 8。
                            listUi.column("mcp.grid")
                                .width(listW)
                                .gap(8.0f)
                                .content([&] {
                                    const int rowCount =
                                        (static_cast<int>(tools.size()) +
                                         cols - 1) /
                                        cols;
                                    for (int r = 0; r < rowCount; ++r) {
                                        listUi.row("mcp.grid.row." +
                                                   std::to_string(r))
                                            .size(listW, kCardH)
                                            .gap(8.0f)
                                            .content([&] {
                                                for (int c = 0; c < cols; ++c) {
                                                    const int idx = r * cols + c;
                                                    if (idx >= static_cast<int>(
                                                                   tools.size()))
                                                        break;
                                                    drawToolCard(
                                                        listUi, tools[idx], t,
                                                        comp, svc.running,
                                                        cardW);
                                                }
                                            })
                                            .build();
                                    }
                                })
                                .build();
                        })
                        .build();
                })
                .build();

    drawDetailDialog(ui, screen, t, comp);
}

void McpPageModule::drawToolCard(eui::Ui& ui,
                                 const std::shared_ptr<core::mcp::IMcpTool>& tool,
                                 const core::theme::Tokens& t,
                                 const components::theme::ThemeColorTokens& comp,
                                 bool serviceRunning,
                                 float cardWidth) {
    const std::string& name = tool->name();
    const std::string id = "mcp.card." + name;
    const bool hovered = hoveredCards_.count(name) > 0;
    const eui::Color kTransparent{0.0f, 0.0f, 0.0f, 0.0f};

    // 2026-11-01 自适应列宽下的卡内布局（全部由 cardWidth 推导，不再写死坐标）：
    //   [图标 40][文本列 …… 16px ……][● 状态]  ← 状态组右对齐
    //   [内置][服务]                        [查看详情 ›]
    const std::string statusText = serviceRunning ? "可用" : "未启用";
    const float statusTextW = core::TextPrimitive::measureTextWidth(
        statusText, "", core::theme::scaled(11.0f), 400);
    const float statusTextX = cardWidth - kCardPad - statusTextW;
    const float statusDotX = statusTextX - 12.0f - 8.0f;
    const float textW = std::max(0.0f, statusDotX - kCardTextX - kStatusGap);

    ui.stack(id)
        .size(cardWidth, kCardH)
        .content([&] {
            // 玻璃三件套（池235 裁决：补齐 glassBorder + glassShadowColor）。
            // 规范 §5.2-1 原写「手绘卡面换取色、圆角/描边/阴影参数不变」，§6-C
            // 又写「→ itemSurface + glassBorder + glassShadowColor」，两处互相
            // 矛盾；老板 2026-11-08 拍板按 §6-C 补齐——全站玻璃卡只在填色上
            // 同源、而边缘仍是灰线 + 近乎无影，视觉上会掉出「玻璃」语言掉成
            // 实色块。此处取 §6-C 口径，§5.2-1 相应措辞已在规范文档修订。
            ui.rect(id + ".bg")
                .size(cardWidth, kCardH)
                .color(ui_kit::itemSurface(t))
                .radius(t.sizes.radiusLarge)
                // 阴影几何与 md 玻璃壳 / agent 摘要卡同款（24 模糊 / 8 偏移），
                // 色走 glassShadowColor（亮档冷绿黑，纯黑压绿白底会发灰）
                .shadow(core::Shadow{true, {0.0f, 8.0f}, 24.0f, 0.0f,
                                     ui_kit::glassShadowColor(t), false})
                .border(1.0f, ui_kit::glassBorder(t))
                .build();

            // 图标位：primarySoftBg 圆角底 + FA 码点
            ui.rect(id + ".icon.bg")
                .position(kCardPad, kCardPad)
                .size(kCardIcon, kCardIcon)
                .radius(t.sizes.radius)
                .color(t.colors.primarySoftBg)
                .build();
            ui.text(id + ".icon")
                .position(kCardPad, kCardPad)
                .size(kCardIcon, kCardIcon)
                .icon(eui::utf8(toolIconGlyph(name)))
                .fontSize(core::theme::scaled(16.0f))
                .color(t.colors.primary)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();

            // 名称 + 单行描述（Ellipsis 只在 wrap(false)+maxWidth>0 生效）
            ui.text(id + ".name")
                .position(kCardTextX, kCardPad)
                .size(textW, core::theme::scaled(20.0f))
                .text(name)
                .fontFamily("monospace")
                .fontSize(core::theme::scaled(14.0f))
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            ui.text(id + ".desc")
                .position(kCardTextX, kCardPad + 24.0f)
                .size(textW, core::theme::scaled(16.0f))
                .text(tool->description())
                .fontSize(core::theme::scaled(12.0f))
                .color(t.colors.textTertiary)
                .maxWidth(textW)
                .wrap(false)
                .overflow(core::TextOverflow::Ellipsis)
                .build();

            // 状态点 + 文案（右上；服务运行态只读反映）
            // 2026-11-01 F-01：状态组整体右对齐，与描述列右端保持 ≥16px 间距
            // （原固定 x=254、描述宽 178，间距只有 8px，长描述末尾的「…」与
            //   「未启用」粘成一句话）。
            ui.rect(id + ".status.dot")
                .position(statusDotX, kCardPad + 4.0f)
                .size(8.0f, 8.0f)
                .radius(t.sizes.radiusSmall)
                .color(serviceRunning ? t.colors.success : t.colors.textDisabled)
                .build();
            ui.text(id + ".status.text")
                .position(statusTextX, kCardPad + 14.0f)
                .size(statusTextW, core::theme::scaled(14.0f))
                .text(statusText)
                .fontSize(core::theme::scaled(11.0f))
                // 状态词两档制=600（规范 §2.1；池235 收编：原先无 fontWeight
                // 渲染 400，与 agent/schedule/claude 三页同语义 600 漂移）
                .fontWeight(600)
                .color(t.colors.textTertiary)
                .build();

            // 标签 chips：内置（primary 软底）/ 服务（中性灰底）
            // 2026-11-01 F-12/R6：服务 chip 原用 purple 软底 + purple 字——purple
            // 是用户可选的强调色槽位，借用为类别标签色会让微信绿白体系里凭空冒出
            // 唯一的紫色。改中性灰底 + 主色字。
            ui.rect(id + ".tag.builtin.bg")
                .position(kCardPad, 84.0f)
                .size(kChipW, kChipH)
                .radius(t.sizes.radiusPill)
                .color(t.colors.primarySoftBg)
                .build();
            ui.text(id + ".tag.builtin")
                .position(kCardPad, 84.0f)
                .size(kChipW, kChipH)
                .text("内置")
                .fontSize(core::theme::scaled(11.0f))
                .color(t.colors.primaryText)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            const float svcChipX = kCardPad + kChipW + 6.0f;
            ui.rect(id + ".tag.svc.bg")
                .position(svcChipX, 84.0f)
                .size(kChipW, kChipH)
                .radius(t.sizes.radiusPill)
                .color(t.colors.borderMuted)
                .build();
            ui.text(id + ".tag.svc")
                .position(svcChipX, 84.0f)
                .size(kChipW, kChipH)
                .text("服务")
                .fontSize(core::theme::scaled(11.0f))
                .color(t.colors.primaryText)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();

            // hover CTA：平时 opacity 0，onHover 浮现（app_sidebar 窗控钮先例的
            // .states+onHover 通道）；整卡可点进详情。右对齐随卡宽自适应。
            const float ctaX = cardWidth - kCardPad - kCtaW;
            ui.row(id + ".cta")
                .position(ctaX, 84.0f)
                .size(kCtaW, kChipH)
                .alignItems(eui::Align::CENTER)
                .gap(4.0f)
                .opacity(hovered ? 1.0f : 0.0f)
                .content([&] {
                    ui.text(id + ".cta.label")
                        .size(kCtaW - 18.0f, kChipH)
                        .text("查看详情")
                        .fontSize(core::theme::scaled(12.0f))
                        .color(t.colors.primaryText)
                        .horizontalAlign(eui::HorizontalAlign::Right)
                        .build();
                    ui.text(id + ".cta.arrow")
                        .size(14.0f, kChipH)
                        .icon(0xF061)  // fa-arrow-right
                        .fontSize(core::theme::scaled(11.0f))
                        .color(t.colors.primaryText)
                        .build();
                })
                .build();

            // 命中面：透明三态 + onHover/onClick（顶层，吃整卡点击）
            ui.rect(id + ".hit")
                .size(cardWidth, kCardH)
                .states(kTransparent, kTransparent, kTransparent)
                .onHover([this, name](bool h) {
                    if (h) {
                        hoveredCards_.insert(name);
                    } else {
                        hoveredCards_.erase(name);
                    }
                })
                .onClick([this, name] { openDetail(name); })
                .build();
        })
        .build();
}

// 「记忆」模块行：全宽轻量行卡（状态行同构；不用 340 网格卡，为后续模块行
// 自然堆叠留形）。开关态每帧从桥读——setEnabled 失败时注册态未变，下一帧自动
// 回弹，无需额外状态回写；反馈红字占用说明行槽位，行盒几何不变。
void McpPageModule::drawMemoryModuleRow(
    eui::Ui& ui, float contentW, const core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& comp) {
    const std::string id = "mcp.module.memory";
    // B6/F-69-2：行高与文字行盒随字号派生——放大档字变大而 64 行高不变时，
    // 标题（scaled 18）与描述行叠字、描述底越出卡外。
    const float kRowH = core::theme::scaled(64.0f);
    constexpr float kPad = 12.0f;
    constexpr float kIconSize = 40.0f;
    constexpr float kSwitchW = 44.0f;
    constexpr float kSwitchH = 24.0f;
    // 说明/标题区：图标右侧到开关左侧（130% 字号档靠 Ellipsis 兜底）
    const float textW = std::max(
        0.0f, contentW - kPad - kIconSize - 12.0f - kSwitchW - kPad);

    ui.stack(id)
        .size(contentW, kRowH)
        .content([&] {
            ui.rect(id + ".bg")
                .size(contentW, kRowH)
                .color(eui::Color{t.colors.surface.r, t.colors.surface.g,
                                  t.colors.surface.b, t.colors.surface.a})
                .radius(t.sizes.radiusLarge)
                .border(1.0f, t.colors.borderDefault)
                .build();
            ui.rect(id + ".icon.bg")
                .position(kPad, kPad)
                .size(kIconSize, kIconSize)
                .radius(t.sizes.radius)
                .color(t.colors.primarySoftBg)
                .build();
            ui.text(id + ".icon")
                .position(kPad, kPad)
                .size(kIconSize, kIconSize)
                .icon(eui::utf8(0xF5DC))  // fa-brain，与 app_registry 记忆行同码点
                .fontSize(core::theme::scaled(16.0f))
                .color(t.colors.primary)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            ui.text(id + ".title")
                .position(kPad + kIconSize + 12.0f, kPad)
                .size(textW, core::theme::scaled(18.0f))
                .text("记忆")
                .fontSize(core::theme::scaled(14.0f))
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            const bool hasError = !memorySaveError_.empty();
            // 描述行顶 = 标题行底 + 4（100% = 12+18+4 = 34，与原字面 22 偏移等价）
            ui.text(id + ".desc")
                .position(kPad + kIconSize + 12.0f,
                          kPad + core::theme::scaled(18.0f) +
                              core::theme::scaled(4.0f))
                .size(textW, core::theme::scaled(16.0f))
                .text(hasError ? "保存失败：" + memorySaveError_
                               : "将本机长期记忆以 memory_search 工具开放给 MCP 客户端查询")
                .fontSize(core::theme::scaled(12.0f))
                .color(hasError ? t.colors.errorText : t.colors.textTertiary)
                .maxWidth(textW)
                .wrap(false)
                .overflow(core::TextOverflow::Ellipsis)
                .build();
            components::toggleSwitch(ui, id + ".toggle")
                .position(contentW - kPad - kSwitchW, (kRowH - kSwitchH) * 0.5f)
                .size(kSwitchW, kSwitchH)
                .checked(app::memory_mcp::status().switchOn)
                .theme(comp)
                .onChange([this](bool on) {
                    const auto r = app::memory_mcp::setEnabled(on);
                    memorySaveError_ =
                        r.isFail() ? r.errorMessage() : std::string();
                })
                .build();
        })
        .build();
}

void McpPageModule::refreshClaudeInstallState() {
    const cc::ConfigState st = cc::readConfigState(core::paths::claudeConfigFile());
    ccState_.filePresent = st.filePresent;
    ccState_.installed = st.installed;
    const auto live = app::mcp::status();
    ccState_.serviceRunning = live.running && live.port > 0;
    if (!st.error.empty()) ccError_ = st.error;
}

void McpPageModule::toggleClaudeInstall() {
    if (!ccState_.actionable()) return;  // 置灰态点击不生效（防御：disabled 已拦）
    const auto file = core::paths::claudeConfigFile();
    core::Result<void> r;
    if (ccState_.installed) {
        r = cc::removeServerFile(file);
    } else {
        // 端点拼法同既有先例（手册 §3.8）：单一 HTTP 服务 + /mcp
        const auto live = app::mcp::status();
        r = cc::installServerFile(
            file, "http://127.0.0.1:" + std::to_string(live.port) + "/mcp");
    }
    ccError_ = r.isFail() ? r.errorMessage() : std::string();
    // 审计 P1-4：写入成功 ≠ 立即生效（Claude Code 启动时读配置）——就地提示重启
    if (r.isOk() && !ccState_.installed) {
        ccNotice_ = "已写入，重启 Claude Code 后生效";
    } else {
        ccNotice_.clear();
    }
    refreshClaudeInstallState();  // 立即重读，按钮态与文件同步
    core::platform::requestUiUpdate();
}

void McpPageModule::drawClaudeInstallButton(
    eui::Ui& ui, float x, float y, const CcButtonState& st,
    const core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& comp) {
    // 文案三态：未装=安装 / 已装=卸载；不可操作时保留当前语义文案但置灰
    const std::string label = st.installed ? "从 Claude Code 卸载" : "安装到 Claude Code";
    components::button(ui, "mcp.dialog.detail.btn.claude")
        .theme(comp, false)  // 次要操作：与「关闭」同级灰底，不抢「试一下」主色
        .position(x, y)
        .size(160.0f, 32.0f)
        .text(label)
        .fontSize(core::theme::scaled(12.0f))
        .disabled(!st.actionable())
        .onClick([this] { toggleClaudeInstall(); })
        .build();
}

void McpPageModule::drawDetailDialog(
    eui::Ui& ui, const eui::Screen& screen, const core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& comp) {
    const bool running = tryRunning();
    bool hasResult = false;
    bool isError = false;
    std::string resultText;
    {
        std::lock_guard<std::mutex> lk(tryMutex_);
        hasResult = !tryResultText_.empty();
        isError = tryError_.load(std::memory_order_acquire);
        resultText = tryResultText_;
    }

    components::dialog(ui, "mcp.dialog.detail")
        .open(detailOpen())
        .screen(screen.width, screen.height)
        .size(kDialogW, kDialogH)
        .theme(comp)
        .onOpenChange([this](bool v) {
            if (!v) closeDetail();  // backdrop 点击 / 关闭钮 / Esc 三通道同汇
        })
        .content([&] {
            if (!detailTool_) return;
            // keep-alive 本帧副本：弹窗开着时工具被注销也不会悬垂
            const auto tool = detailTool_;
            const float w = kDialogW;
            const float h = kDialogH;

            // hero：图标位 + 名称（mono）+ 标签行
            ui.rect("mcp.dialog.detail.hero.icon.bg")
                .position(20.0f, 16.0f)
                .size(52.0f, 52.0f)
                .radius(t.sizes.radiusLarge)
                .color(t.colors.primarySoftBg)
                .build();
            ui.text("mcp.dialog.detail.hero.icon")
                .position(20.0f, 16.0f)
                .size(core::theme::scaled(52.0f), core::theme::scaled(52.0f))
                .icon(eui::utf8(toolIconGlyph(tool->name())))
                .fontSize(core::theme::scaled(20.0f))
                .color(t.colors.primary)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            ui.text("mcp.dialog.detail.hero.name")
                .position(88.0f, 20.0f)
                .size(w - 108.0f, core::theme::scaled(24.0f))
                .text(tool->name())
                .fontFamily("monospace")
                .fontSize(core::theme::scaled(18.0f))
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            ui.rect("mcp.dialog.detail.hero.tag.builtin.bg")
                .position(88.0f, 50.0f)
                .size(40.0f, core::theme::scaled(18.0f))
                .radius(t.sizes.radiusPill)
                .color(t.colors.primarySoftBg)
                .build();
            ui.text("mcp.dialog.detail.hero.tag.builtin")
                .position(88.0f, 50.0f)
                .size(40.0f, core::theme::scaled(18.0f))
                .text("内置")
                .fontSize(core::theme::scaled(11.0f))
                .color(t.colors.primaryText)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            ui.rect("mcp.dialog.detail.hero.tag.svc.bg")
                .position(134.0f, 50.0f)
                .size(40.0f, core::theme::scaled(18.0f))
                .radius(t.sizes.radiusPill)
                .color(t.colors.borderMuted)
                .build();
            ui.text("mcp.dialog.detail.hero.tag.svc")
                .position(134.0f, 50.0f)
                .size(40.0f, core::theme::scaled(18.0f))
                .text("服务")
                .fontSize(core::theme::scaled(11.0f))
                .color(t.colors.primaryText)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();

            // 描述（可换行）。bug180：固定高盒（88..136）+ wrap，长描述折行溢出
            // 须按节点框 clip——不变量同 schema.text（折行溢出不越盒压参数区），
            // 否则墨迹直铺压到 148 的「参数 schema」标题/框。
            ui.text("mcp.dialog.detail.desc")
                .position(20.0f, 88.0f)
                .size(w - 40.0f, core::theme::scaled(48.0f))
                .text(tool->description())
                .fontSize(core::theme::scaled(13.0f))
                .lineHeight(core::theme::scaled(19.0f))
                .color(t.colors.textSecondary)
                .wrap(true)
                .clip()
                .build();

            // 参数区（池146）：inputSchema 全部属性为扁平类型（string/number/
            // integer/boolean）且 ≤ 6 个 → 两列镜像参数表单（行高 30 + 行距 8，
            // 3 行占 168..274；label 110 定宽同 formRow 口径（含墨迹 Center 垂直
            // 居中，bug216），输入/开关贴半列右缘，镜像几何对照提醒卡池132）；
            // 否则（嵌套 schema / 无参数 / 超上限）回退 schema JSON monospace
            // 展示块（空参调用，原行为不变）。
            if (!paramSpecs_.empty()) {
                const float paramHalfW = (w - 40.0f - 16.0f) * 0.5f;
                ui.text("mcp.dialog.detail.params.title")
                    .position(20.0f, 148.0f)
                    .size(w - 40.0f, core::theme::scaled(16.0f))
                    .text("参数")
                    .fontSize(core::theme::scaled(11.0f))
                    .color(t.colors.textTertiary)
                    .build();
                constexpr float kParamRowH = 30.0f;
                constexpr float kParamRowGap = 8.0f;
                constexpr float kParamLabelW = 110.0f;
                for (std::size_t i = 0; i < paramSpecs_.size(); ++i) {
                    const ParamSpec& spec = paramSpecs_[i];
                    const float rowY = 168.0f + static_cast<float>(i / 2) *
                                                   (kParamRowH + kParamRowGap);
                    const float colX = 20.0f +
                                       (i % 2 == 0 ? 0.0f
                                                   : paramHalfW + 16.0f);
                    const std::string baseId =
                        "mcp.dialog.detail.param." + spec.name;
                    ui.text(baseId + ".label")
                        .position(colX, rowY)
                        .size(kParamLabelW, kParamRowH)
                        .text(spec.name)
                        .fontFamily("monospace")
                        .fontSize(core::theme::scaled(11.0f))
                        // bug216：墨迹垂直居中（ui_kit::formRow 同口径）——旧
                        // lineHeight(30) 顶置凑数的墨迹落点与 components::input
                        // 盒内居中不一致，参数名与输入框错位（老板 2026-11-04 实测）
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.textSecondary)
                        .maxWidth(kParamLabelW)
                        .overflow(core::TextOverflow::Ellipsis)
                        .clip()
                        .build();
                    if (spec.kind == ParamSpec::Kind::Bool) {
                        // 开关贴半列右缘（44×24 字面口径）；未拨动过不写入参数 JSON
                        ui_kit::wswitch(
                            ui, baseId + ".sw", boolValues_[spec.name],
                            colX + paramHalfW - 44.0f, rowY + 3.0f, 44.0f, 24.0f,
                            t, [this, key = spec.name](bool v) {
                                boolValues_[key] = v;
                                boolTouched_[key] = true;
                            });
                        continue;
                    }
                    const auto draftIt = stringDrafts_.find(spec.name);
                    components::input(ui, baseId + ".input")
                        .position(colX + kParamLabelW + 8.0f, rowY + 2.0f)
                        .size(paramHalfW - kParamLabelW - 8.0f, 26.0f)
                        .value(draftIt != stringDrafts_.end() ? draftIt->second
                                                              : std::string())
                        .placeholder(spec.required ? "必填" : "可选")
                        .fontSize(core::theme::scaled(12.0f))
                        .theme(comp)
                        .onChange([this, key = spec.name](const std::string& v) {
                            stringDrafts_[key] = v;
                        })
                        .build();
                }
                // 校验错误就近红字（表单下方固定槽位 278..294；无错不建不占位）
                if (!paramsError_.empty()) {
                    ui.text("mcp.dialog.detail.params.error")
                        .position(28.0f, 278.0f)
                        .size(w - 56.0f, 16.0f)
                        .text(paramsError_)
                        .fontSize(core::theme::scaled(11.0f))
                        .lineHeight(16.0f)
                        .color(t.colors.errorText)
                        .maxWidth(w - 56.0f)
                        .overflow(core::TextOverflow::Ellipsis)
                        .clip()
                        .build();
                }
            } else {
                // inputSchemaJson：monospace 展示块（浅底 + muted 边）
                ui.text("mcp.dialog.detail.schema.title")
                    .position(20.0f, 148.0f)
                    .size(w - 40.0f, core::theme::scaled(16.0f))
                    .text("参数 schema（JSON）")
                    .fontSize(core::theme::scaled(11.0f))
                    .color(t.colors.textTertiary)
                    .build();
                ui.rect("mcp.dialog.detail.schema.box")
                    .position(20.0f, 168.0f)
                    .size(w - 40.0f, 118.0f)
                    .radius(t.sizes.radius)
                    .color(t.colors.background)
                    .border(1.0f, t.colors.borderMuted)
                    .build();
                // 不变量：schema 文本按节点框 clip，折行溢出不越 schema.box 压「运行结果」标题。
                ui.text("mcp.dialog.detail.schema.text")
                    .position(28.0f, 176.0f)
                    .size(w - 56.0f, core::theme::scaled(102.0f))
                    .text(tool->inputSchemaJson())
                    .fontFamily("monospace")
                    .fontSize(core::theme::scaled(12.0f))
                    .lineHeight(core::theme::scaled(17.0f))
                    .color(t.colors.textSecondary)
                    .wrap(true)
                    .clip()
                    .build();
            }

            // 结果区：成功/失败前缀区分；运行中占位
            ui.text("mcp.dialog.detail.result.title")
                .position(20.0f, 298.0f)
                .size(w - 40.0f, core::theme::scaled(16.0f))
                .text("运行结果")
                .fontSize(core::theme::scaled(11.0f))
                .color(t.colors.textTertiary)
                .build();
            const bool resultEmpty = !running && !hasResult;
            ui.rect("mcp.dialog.detail.result.box")
                .position(20.0f, 318.0f)
                .size(w - 40.0f, h - 318.0f - 64.0f)
                .radius(t.sizes.radius)
                .color(t.colors.background)
                .border(1.0f, t.colors.borderMuted)
                .build();
            // bug145：结果文本按行拆分入滚动区（盒内按行滚动、超长单行裁剪），
            // 不再单文本盒直铺越框；measure cache 键 = 运行序号 + 文本长度（换轮失效）
            const std::string display =
                resultDisplayText(running, hasResult, isError, resultText);
            const std::vector<std::string> resultLines = splitResultLines(display);
            const float resultLineH = core::theme::scaled(17.0f);
            components::scrollView(ui, "mcp.dialog.detail.result.scroll")
                .position(28.0f, 326.0f)
                .size(w - 56.0f, h - 326.0f - 64.0f - 16.0f)
                .gap(0.0f)
                .step(resultLineH)
                .theme(comp)
                .contentKey("mcp.dialog.detail.result.v" +
                            std::to_string(tryRunSeq_) + "." +
                            std::to_string(resultText.size()))
                .content([&](eui::Ui& inner, float cw, float vh) {
                    (void)vh;
                    for (std::size_t i = 0; i < resultLines.size(); ++i) {
                        inner.text("mcp.dialog.detail.result.line." +
                                   std::to_string(i))
                            .position(0.0f, static_cast<float>(i) * resultLineH)
                            .size(cw, resultLineH)
                            .text(resultLines[i])
                            .fontFamily("monospace")
                            .fontSize(core::theme::scaled(12.0f))
                            .lineHeight(resultLineH)
                            .color(isError ? t.colors.errorText
                                           : (resultEmpty ? t.colors.textTertiary
                                                          : t.colors.textSecondary))
                            .maxWidth(cw)
                            .overflow(core::TextOverflow::Clip)
                            .clip()
                            .build();
                    }
                })
                .build();

            // 操作行：「安装到 Claude Code」（need244）+「试一下」（运行中禁用）+「关闭」。
            // 三个钮同行不重叠：claude 20..180、hint 188..396、close 404..476、
            // try 484..580（w=600，右缘留 20）。
            const CcButtonState cc = ccState_;
            drawClaudeInstallButton(ui, 20.0f, h - 52.0f, cc, t, comp);
            {
                // 就地提示槽：无 .claude.json / 服务未运行 / 写失败 / 安装成功待重启，四选一
                std::string hint;
                bool isError = false;
                if (!cc.filePresent) {
                    hint = "未检测到 Claude Code（~/.claude.json 不存在）";
                } else if (!cc.serviceRunning) {
                    hint = "请先启动 MCP 服务";
                } else if (!ccError_.empty()) {
                    hint = ccError_;
                    isError = true;
                } else if (!ccNotice_.empty()) {
                    hint = ccNotice_;
                }
                if (!hint.empty()) {
                    const float hintW = 404.0f - 8.0f - 188.0f;
                    ui.text("mcp.dialog.detail.claude.hint")
                        .position(188.0f, h - 52.0f)
                        .size(hintW, 32.0f)
                        .text(hint)
                        .fontSize(core::theme::scaled(11.0f))
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(isError ? t.colors.errorText
                                       : t.colors.textTertiary)
                        .maxWidth(hintW)
                        .overflow(core::TextOverflow::Ellipsis)
                        .clip()
                        .build();
                }
            }
            components::button(ui, "mcp.dialog.detail.btn.try")
                .theme(comp, true)
                .position(w - 20.0f - 96.0f, h - 52.0f)
                .size(96.0f, 32.0f)
                .text(running ? "运行中…" : "试一下")
                .fontSize(core::theme::scaled(13.0f))
                .disabled(running)
                .onClick([this] { startTryRun(); })
                .build();
            components::button(ui, "mcp.dialog.detail.btn.close")
                .theme(comp, false)
                .position(w - 20.0f - 96.0f - 8.0f - 72.0f, h - 52.0f)
                .size(72.0f, 32.0f)
                .text("关闭")
                .fontSize(core::theme::scaled(13.0f))
                .onClick([this] { closeDetail(); })
                .build();
        })
        .build();
}

bool McpPageModule::onKeyEvent(const eui::KeyEvent& event) {
    if (event.action != eui::KeyAction::Press &&
        event.action != eui::KeyAction::Repeat) {
        return false;
    }
    if (event.key == eui::InputKey::Escape && detailOpen()) {
        closeDetail();
        return true;
    }
    return false;
}

void McpPageModule::onStart() {
    // mcp.health：只读诊断工具（need 219 F-05），注册跟随模块启停；
    // 注册失败只留痕不中断（agent 模块同口径）
    if (!healthRegistered_) {
        healthTool_ = makeMcpHealthTool();
        auto reg = ::core::ToolRegistry::instance().registerTool(healthTool_);
        if (reg.isFail()) {
            LOG_WARN("MCP: mcp.health 注册失败: " + reg.errorMessage());
            return;
        }
        healthRegistered_ = true;
    }
}

void McpPageModule::onStop() {
    // 先停后注销的 join 收束点：在途 execute 完整跑完（web_fetch 自限 10s），
    // 不硬杀不悬垂
    joinTryThread();
    tryRunning_.store(false, std::memory_order_release);
    // mcp.health 随模块停用注销（need 219 F-05）
    if (healthRegistered_) {
        ::core::ToolRegistry::instance().unregisterTool("mcp.health");
        healthRegistered_ = false;
    }
    healthTool_.reset();
}

void McpPageModule::openDetail(const std::string& name) {
    auto found = registry_->find(name);
    if (found.isFail()) return;  // 工具已被注销：不开空弹窗
    detailTool_ = found.value();
    buildParamSpecs();  // 池146：按当前工具 inputSchema 重建参数表单规格
    // need244：详情页每次打开都重读 ~/.claude.json（不跨会话缓存），并清上一轮写
    // 失败原因与「已写入」提示（提示只属于本次点击后的窗口期，重开视为过期）
    ccError_.clear();
    ccNotice_.clear();
    refreshClaudeInstallState();
    // 换工具打开：清上一工具的残留结果（同工具重开保留在途/已完成态）
    if (tryToolName_ != name && !tryRunning()) {
        std::lock_guard<std::mutex> lk(tryMutex_);
        tryResultText_.clear();
        tryDone_.store(false, std::memory_order_release);
        tryError_.store(false, std::memory_order_release);
        tryToolName_.clear();
        // 换工具同步清参数草稿与校验错误（同工具重开保留草稿，便于重试）
        stringDrafts_.clear();
        boolValues_.clear();
        boolTouched_.clear();
        paramsError_.clear();
    }
}

void McpPageModule::closeDetail() { detailTool_.reset(); }

void McpPageModule::startTryRun() {
    if (!detailTool_ || tryRunning()) return;
    if (tryThread_.joinable()) {
        tryThread_.join();  // 上一轮已完成未收割的先收（防御重复点击竞态）
    }
    // 池146：先校验 + 组装入参（UI 线程）；失败不发起 execute，错误就近显示
    auto args = buildArgsJson();
    if (args.isFail()) {
        paramsError_ = args.errorMessage();
        return;
    }
    paramsError_.clear();
    ++tryRunSeq_;
    const auto tool = detailTool_;  // keep-alive：注销瞬间不悬垂
    tryToolName_ = tool->name();
    tryRunning_.store(true, std::memory_order_release);
    tryDone_.store(false, std::memory_order_release);
    tryError_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(tryMutex_);
        tryResultText_.clear();
    }
    tryThread_ = std::thread([this, tool, argsJson = args.value()] {
        auto r = tool->execute(argsJson);
        bool err = false;
        std::string text;
        if (r.isFail()) {
            err = true;
            text = r.errorMessage();
        } else {
            err = r.value().isError;
            text = r.value().text;
        }
        {
            std::lock_guard<std::mutex> lk(tryMutex_);
            tryResultText_ = std::move(text);
        }
        tryError_.store(err, std::memory_order_release);
        tryDone_.store(true, std::memory_order_release);
        // worker 置 done 后主动要帧，不等用户输入（settings 后台任务同口径）
        core::platform::requestUiUpdate();
    });
}

// 池146：解析当前工具 inputSchema → 参数规格。properties 缺失/为空、含非扁平类型
//（object/array/未知）或参数 > 6（两列 3 行上限）→ 表单不建，回退 schema JSON 框。
// 只重建规格，不动草稿（换工具的草稿清理在 openDetail）。
void McpPageModule::buildParamSpecs() {
    paramSpecs_.clear();
    if (!detailTool_) return;
    auto parsed = platform::json::parse(detailTool_->inputSchemaJson());
    if (parsed.isFail() || !parsed.value().isObject()) return;
    const platform::json::JsonObject schema = parsed.value().asObject();
    std::set<std::string> required;
    const platform::json::JsonValue reqValue = schema.get("required");
    if (reqValue.isArray()) {
        for (int i = 0; i < reqValue.size(); ++i) {
            const platform::json::JsonValue item = reqValue[i];
            if (item.isString()) required.insert(item.asString());
        }
    }
    const platform::json::JsonValue propsValue = schema.get("properties");
    if (!propsValue.isObject()) return;  // 无参数工具：无表单（schema 框兜底）
    const platform::json::JsonObject props = propsValue.asObject();
    std::vector<ParamSpec> specs;
    for (const auto& key : props.keys()) {
        const platform::json::JsonValue propValue = props.get(key);
        if (!propValue.isObject()) return;
        const platform::json::JsonValue typeValue =
            propValue.asObject().get("type");
        if (!typeValue.isString()) return;
        ParamSpec spec;
        spec.name = key;
        spec.required = required.count(key) > 0;
        const std::string type = typeValue.asString();
        if (type == "string") {
            spec.kind = ParamSpec::Kind::String;
        } else if (type == "number") {
            spec.kind = ParamSpec::Kind::Number;
        } else if (type == "integer") {
            spec.kind = ParamSpec::Kind::Integer;
        } else if (type == "boolean") {
            spec.kind = ParamSpec::Kind::Bool;
        } else {
            return;  // object/array/未知类型：不可简单渲染 → 整体回退
        }
        specs.push_back(std::move(spec));
    }
    if (specs.empty() || specs.size() > 6) return;
    paramSpecs_ = std::move(specs);
}

// 池146：草稿 → 入参 JSON 对象文本；校验失败文案点名参数与错因（可执行修正）。
// string 空串 = 未填；boolean 未拨动 = 不写入（工具侧默认值语义不被显式 false 覆盖）。
core::Result<std::string> McpPageModule::buildArgsJson() {
    platform::json::JsonObject args;
    static const std::string kEmpty;
    for (const auto& spec : paramSpecs_) {
        if (spec.kind == ParamSpec::Kind::Bool) {
            const auto touched = boolTouched_.find(spec.name);
            if (touched != boolTouched_.end() && touched->second) {
                args.set(spec.name, platform::json::makeBool(
                                        boolValues_[spec.name]));
            }
            continue;
        }
        const auto draftIt = stringDrafts_.find(spec.name);
        const std::string& raw =
            draftIt != stringDrafts_.end() ? draftIt->second : kEmpty;
        if (raw.empty()) {
            if (spec.required) {
                return core::Result<std::string>::fail(
                    core::ErrorCode::ValidationError,
                    "缺少必填参数：" + spec.name);
            }
            continue;  // 可选留空 = 不写入
        }
        char* end = nullptr;
        if (spec.kind == ParamSpec::Kind::Integer) {
            const long long value = std::strtoll(raw.c_str(), &end, 10);
            if (end == raw.c_str() || *end != '\0') {
                return core::Result<std::string>::fail(
                    core::ErrorCode::ValidationError,
                    "参数 " + spec.name + " 需为整数：当前「" + raw + "」");
            }
            args.set(spec.name, platform::json::makeInt(
                                    static_cast<std::int64_t>(value)));
        } else if (spec.kind == ParamSpec::Kind::Number) {
            const double value = std::strtod(raw.c_str(), &end);
            if (end == raw.c_str() || *end != '\0') {
                return core::Result<std::string>::fail(
                    core::ErrorCode::ValidationError,
                    "参数 " + spec.name + " 需为数字：当前「" + raw + "」");
            }
            args.set(spec.name, platform::json::makeDouble(value));
        } else {
            args.set(spec.name, platform::json::makeString(raw));
        }
    }
    auto serialized = platform::json::stringify(
        platform::json::JsonValue(std::move(args)));
    if (serialized.isFail()) {
        return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                               "入参序列化失败");
    }
    return core::Result<std::string>::ok(serialized.value());
}

void McpPageModule::harvestTryRun() {
    if (!tryThread_.joinable()) return;
    if (!tryDone_.load(std::memory_order_acquire)) return;
    tryThread_.join();  // join 即 happens-before，之后读 tryResultText_ 安全
    tryRunning_.store(false, std::memory_order_release);
}

void McpPageModule::joinTryThread() {
    if (tryThread_.joinable()) {
        tryThread_.join();
    }
}

} // namespace module::mcp

extern "C" ::core::Module* create_mcp_page_module() {
    return new module::mcp::McpPageModule();
}
