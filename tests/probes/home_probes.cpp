// home_probes.cpp — DevDesk probes：home 模块 6 组断言
//（spec docs/superpowers/specs/2026-09-28-devdesk-probes-design.md
//  §3-probe-full 的 home_probes 组，类 1b/2a/2b/4a/5a/6a）
//
// 组 A（类 1b sparkline z，P1）：sparkline 根 frame 与 label frame 不相交。
//   修复 046beaa：sparkline 固定进卡内顶带（y0..16 = label 16 之上，整盒高 16），
//   旧 32px 盒以 (0,0) 构建叠 label 文字带（home_module.cpp:156-164 注释）；
//   歧义记录：spec 写的「gap≥2px」系实现前估量——修复落地形态为顶带与 label
//   [16,32] **邻接**（gap=0），断言按修复后不变式 = 不相交 + 顶带严格在 label
//   之上（gap≥0，容差 0.5px）；回归 32 盒 → 相交 → 必红。
//   曲线段（linechart.h:147-167）空闲态 {0.0f} 零长度段被跳过（:150-152），
//   且平线贴 bottomY 时胶囊半宽会把段 frame 探进 label 带 → 不以段为断言。
//
// 组 B（类 2a 空转重绘，P1）：静止 UI 不请求重绘。
//   修复 046beaa：周期 onTimer 仅「有速率活动」时挂 1s（home_module.cpp:462-477
//   的 seriesLive/tokenLive 门控 + :147-154 挂载点）——旧 0.5s 常挂 onTimer →
//   整页 2Hz 重排重绘 + hover 0.1s 过渡反复打断（用户报「5 卡 hover 闪」）。
//   断言 = 稳定后连跑 60 帧 pollOnce，runtime.update() 返 true（= 帧末返回
//   paintRequested_，runtime_lifecycle.h:229-233）的帧数 ≤ 1；回归常驻 timer →
//   60 帧（1s）内 ≥2 → 红。
//
// 组 C（类 2b hover token，P1）：colors.hover != colors.surface。
//   修复 046beaa：浅色档 hover 由 #F5F2EC（== surface）降一步 #EDE8DF
//   （theme.cpp:59-64）——Home 卡 .press hover 态在浅色档曾完全不可见。
//   深色档（默认 current()）+ 浅色档各验一次，验完还原深色。
//
// 组 D（类 4a barChart 几何，P1）：bar 面板 plot 区总高 ≥ 36px。
//   审核 #3/#4/#5 修：卡高 208→260（wrap 128→196），plot 16px→84px
//   （home_module.cpp:567-570；barchart.h:84-88 plotY=70、plotHeight=H-112）。
//   度量 = grid.3.y - grid.0.y（barchart.h:114-122 四条横线 y = plotY + k·H/3
//   → grid.0 在 plotY、grid.3 在 bottomY）——不依赖 bar 高度（有 0.16s 帧
//   过渡动画）；数据种入 1 条今日调用 → 非空态 → 图表必建。
//
// 组 E（类 5a API 卡对称，P3）：home.api.chart 卡内 title/wrap 左右间隙相等。
//   审核 #5 修：title/wrap 去二次 position(16,..)（旧 content 已含 inset → 左
//   32 右 0；home_module.cpp:570 注释）→ 卡 content inset 16 + (0,·) → 左右
//   各 16。断言 |左间隙 - 右间隙| ≤ 2px。
//
// 组 F（类 6a stat 溢出，P1）：最小窗（--size 1200x720）下统计数字/badge 不溢。
//   数据注入走 proxy_stats 直连（spec 搭法）：测试时钟 + 单流累计 40M →
//   2s 窗口 delta 20M → rateTps=10M → formatRateTps → 「10000000」（8 位）。
//   判别字段（spec「落点未定则 measureTextSize」分支）：修复实际落点 = **字号
//   收缩**（statNumberFont，home_module.cpp:34-42，8 位 36px ≈ 173 ≤ cw 176；
//   旧 48px 固定 ≈ 230 > 176 必溢），**未加 clip/wrap**（home_module.cpp:118-146
//   无 .clip()/.wrap() 调用）→ 断言两段式：clip||wrap 任一 true → 过；否则
//   TextPrimitive::measureTextSize（dsl.h:1729 同款）内在宽 ≤ 元素 frame 宽。
//   注意：badge 判据相对当前元素 frame（cw）度量，旧长文案按当前元素宽已能
//   收纳，属弱守卫；数字（8 位）才是强判别。
//
// 组 I（G7 余量卡错误行，2026-10-03）：错误文案**只出现一次**且**不溢出内容列**。
//   缺陷：错误文案被渲染两遍——head row 的 `.status`（statusTextFor 在
//   showsErrorLine 时返回 lastError）+ 专属 `.error` 行；且 `.status` 无
//   clip/wrap（maxWidth 只在 wrap=true 时生效，EUI-NEO text.cpp），第一遍
//   溢出卡片右缘被截断（真机图 /tmp/af-home-quota-live.png 的 MiniMax 行）。
//   I(a) 数「整棵余量树里携带该错误文案的 Text 元素个数」== 1 且必须是 .error
//        —— 钉字符串在树中的出现次数，不是「某格是否为空」。
//   I(b) 错误行 / head .status 的**右缘 ≤ 内容列右缘**（宽度比较，非字面量）；
//        且无 clip/wrap 时**按元素真实样式**（含 maxWidth/overflow）量文案宽。
//   I(c) 超长错误文案以「…」(U+2026) 收尾（2026-10-03，EUI-NEO `cbd0b48`）。
//        上一批的 `.clip()` 只把溢出部分硬剪掉——用户看到半句话却不知道后面还有
//        内容。改成单行省略号后要钉住三件事：① 这一格确实溢出（否则整组空跑成
//        恒真假绿）；② 产品那格真的接了 overflow==Ellipsis + maxWidth>0 +
//        !wrap（漏一条 = 框架侧**静默 no-op**，不报错）；③ 渲染行宽逐位等于
//        「保留前缀 + …」且 ≤ maxWidth，即末尾那个字形确实是省略号。
//   ⚠ 门禁那条（ctest -L probe）不带 --quota-fixture，只走离线假种子，故假种子
//   里**必须**有一条错误行（i==8），否则本组空跑成恒真假绿——已由组内
//   「错误行 < 1 即红」硬钉住。
//
// 运行：home_probes.exe [--frames N] [--size 1200x720]（尺寸矩阵最小窗跑
// 1200x720；statW 只随窗**宽**变 → 各组几何在默认 1200x1000 下同型）。

#include "probes/probe_harness.h"
#include "test_support.h"
#include "app_registry.h"
#include "core/module_interface.h"
#include "core/proxy_stats.h"
#include "module_home/home_quota_card.h"   // computeQuotaCardMetrics（组 H(a) 判据）
#include "module_home/home_service.h"  // startOfDayEpoch（逐时 token 种子的本地零点锚）
#include "quota/quota_store.h"  // G7：余量卡种子走真落库口（编解码单点）
#include "platform/json.h"
#include <fstream>
#include <sstream>
#include <vector>
#include "core/theme.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <ios>
#include <iostream>
#include <memory>
#include <string>

namespace {

// RGB 通道差异（>1/255 视为不同色；alpha 不参与——hover/surface 语义比色）
bool colorsDiffer(const eui::Color& a, const eui::Color& b) {
    constexpr float kEps = 0.004f;
    return std::fabs(a.r - b.r) > kEps || std::fabs(a.g - b.g) > kEps ||
           std::fabs(a.b - b.b) > kEps || std::fabs(a.a - b.a) > kEps;
}

// 两 frame 在 y 上的间隙（b.y - (a.y + a.height)）：正 = a 在 b 上方分离，
// 0 = 邻接，负 = a 探进 b
float frameGapY(const core::dsl::Element& a, const core::dsl::Element& b) {
    return b.frame.y - (a.frame.y + a.frame.height);
}

bool framesIntersect(const core::dsl::Element& a, const core::dsl::Element& b) {
    return a.frame.x < b.frame.x + b.frame.width &&
           b.frame.x < a.frame.x + a.frame.width &&
           a.frame.y < b.frame.y + b.frame.height &&
           b.frame.y < a.frame.y + a.frame.height;
}

// 统计卡 id 表（THS-08 元数据表 statCardElementId 同值五个）
constexpr const char* kStatCardIds[] = {
    "home.stat.tokens", "home.stat.notes", "home.stat.todo",
    "home.stat.overdue", "home.stat.api",
};

// 文本防溢判别（组 F）：clip/wrap 任一 true = 已显式保护；否则用
// TextPrimitive::measureTextSize（layout 同款，dsl.h:1729）量内在宽，
// 内在宽 ≤ 元素 frame 宽 + 1px（rounding）视为放得下。
bool textFitsOrGuarded(const core::dsl::Element& el) {
    if (el.clip || el.wrap) {
        return true;
    }
    core::TextStyle style;
    style.text = el.text;
    style.fontFamily = el.fontFamily;
    style.fontSize = el.fontSize;
    style.fontWeight = el.fontWeight;
    style.maxWidth = el.maxWidth;
    style.wrap = el.wrap;
    style.lineHeight = el.lineHeight;
    const core::Vec2 intrinsic = core::TextPrimitive::measureTextSize(style);
    return intrinsic.x <= el.frame.width + 1.0f;
}

// ============ --quota-fixture：真机数据种子（2026-10-03 final 批）============
//
// 默认（不给开关）走离线假种子：门禁 `ctest -L probe` 跑的是那一条，纯离线、
// 可复现、不打任何供应商接口。给了开关就改用 `quota_live_probe --dump` 落的
// JSON —— 即**四家真供应商真查回来的结果**，从而把
// 「真网络 → 真 QuotaStore 落库 → 真中立桥 → 真卡片绘制 → 真落图」这条链
// 一次跑通并留下 PNG 证据（G8 的诚实边界正是「UI 呈现层没被真机点验过」）。
//
// **为什么落库仍走真 QuotaStore::applyFetchResult**：fixture 只提供「这一次
// fetch 返回了什么」，之后 encode/decode、失败韧性、五态判据、标签/裁剪
// 全部是产品代码。若图好看而落库/解码绕过了产品代码，这张图证明不了任何事。
//
// ⚠ 凭据：fixture 里**没有**凭据（探针侧就不导出 apiKey / AK / SK）。这里也不
// 需要——种子只落 config + result 两行，凭据对渲染是无关字段。
//
// 返回值 = 失败原因（空串 = 成功）。**失败必须让整个探针红**，不许退化成
// 「悄悄改用假种子」——那会让这张图的身份变得不可知。

std::string loadQuotaFixture(const std::string& path, core::data::SqliteDb& sq,
                             module::claude::quota::QuotaStore& store,
                             std::vector<int64_t>* createdProfileIds) {
    namespace q = module::claude::quota;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return "打不开 fixture 文件 " + path;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    const auto parsed = platform::json::parse(buf.str());
    if (parsed.isFail()) {
        return "fixture JSON 解析失败: " + parsed.errorMessage();
    }
    if (!parsed.value().isObject()) {
        return "fixture 根不是对象";
    }
    const platform::json::JsonValue providers =
        parsed.value().asObject().get("providers");
    if (!providers.isArray() || providers.size() == 0) {
        return "fixture 的 providers 不是非空数组";
    }
    const int64_t fetchedAt = parsed.value().asObject()
                                  .get("fetchedAt").asInt();

    for (int i = 0; i < providers.size(); ++i) {
        const platform::json::JsonValue pv = providers[i];
        if (!pv.isObject()) {
            return "fixture providers[" + std::to_string(i) + "] 不是对象";
        }
        const platform::json::JsonObject po = pv.asObject();
        const std::string providerId = po.get("providerId").asString();
        const std::string displayName = po.get("displayName").asString();
        // profile 名取 cc-switch 条目名（= 用户在 cc-switch 里看到的那个名字），
        // 而不是 displayName：后者是**供应商中文名**，拿它当 profile 名会让卡片
        // 两列显示同一个词，看着像渲染重复——那是种子的锅，不是产品的锅。
        std::string profileName = po.get("ccswitchName").asString();
        if (profileName.empty()) profileName = displayName;
        if (providerId.empty()) {
            return "fixture providers[" + std::to_string(i) + "] 缺 providerId";
        }

        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        if (!sq.exec("INSERT INTO claude_profiles (name, base_url, api_key_hint, "
                     "api_key_check, model, created_at, updated_at) VALUES ('" +
                         profileName +
                         "', 'https://example.invalid', 'sk-***', 'chk', 'm', " +
                         std::to_string(now) + ", " + std::to_string(now) + ")")
                 .isOk()) {
            return "fixture profile 插入失败: " + profileName;
        }
        auto st = sq.prepare("SELECT last_insert_rowid() AS id");
        auto rows = st.isOk() ? st.value().fetchAll()
                              : core::Result<std::vector<core::data::SqlRow>>::fail(
                                    st.errorCode(), st.errorMessage());
        if (rows.isFail() || rows.value().empty()) {
            return "取 fixture profileId 失败: " + profileName;
        }
        const int64_t pid = rows.value()[0].getInt64("id").value();
        createdProfileIds->push_back(pid);

        q::QuotaConfigRow cfg;
        cfg.profileId = pid;
        cfg.providerId = providerId;
        cfg.enabled = true;
        cfg.autoRefresh = true;
        cfg.intervalMinutes = 15;
        if (store.saveConfig(cfg).isFail()) {
            return "fixture quota config 落库失败: " + profileName;
        }

        // 枚举按**列文本**反解（与探针侧 dump 时的正向映射同一对函数，不可能
        // 各写一份）。kind 的两个取值直接比 quotaKindName 的输出，不另写字面量。
        q::QuotaResult r;
        r.kind = po.get("kind").asString() ==
                         std::string(q::quotaKindName(q::QuotaKind::Balance))
                     ? q::QuotaKind::Balance
                     : q::QuotaKind::Tiered;
        r.balanceRemaining = po.get("balanceRemaining").asDouble();
        r.currency = po.get("currency").asString();
        r.planName = po.get("planName").asString();
        r.error = q::quotaErrorFromName(po.get("error").asString());
        r.errorMessage = po.get("errorMessage").asString();
        const platform::json::JsonValue wins = po.get("windows");
        if (wins.isArray()) {
            for (int j = 0; j < wins.size(); ++j) {
                const platform::json::JsonObject wo = wins[j].asObject();
                q::QuotaWindow w;
                w.key = wo.get("key").asString();
                w.usedPercent = wo.get("usedPercent").asDouble();
                w.resetsAt = wo.get("resetsAt").asString();
                r.windows.push_back(std::move(w));
            }
        }
        if (store.applyFetchResult(pid, r, fetchedAt).isFail()) {
            return "fixture quota result 落库失败: " + profileName;
        }
    }
    return std::string();
}

// ============ 组 I(c)：单行省略号（core::TextOverflow::Ellipsis）===========
//
// 背景（2026-10-03）：`.error` 格上一批只加了 `.clip()`，超长错误文案被**静默
// 剪掉**——用户看到半句话，不知道后面还有内容。EUI-NEO `cbd0b48` 落地了单行省略号，
// 这里的活是「把它用起来 + 钉住它真的在用」。
//
// 取证路径与 runtime 同源：`runtime_render.h` 的 prepareText 就是把 Element 的
// 字段逐条喂给 TextPrimitive。所以本组**不重画整张卡**，而是把 `.error` 那一格的
// 元素字段喂进一个裸 TextPrimitive，量 `prepare()` 之后的行宽——断的是
// 「产品那一格有没有正确接线 maxWidth / overflow」，而「框架能不能画省略号」是
// EUI-NEO 自己 tests/unit/text_ellipsis.cpp 的职责，不在这里重复。
//
// 三条断言，逐条可独立致红：
//   (1) **前提**：原文按 Clip 量出来的宽 > 内容列宽。不成立 = 这一格压根没溢出，
//       整组退化成「没溢出也不红」的恒真假绿（种子不够长时的经典坑）。
//   (2) **接线**：产品那格声明 overflow==Ellipsis 且 maxWidth>0 且 !wrap。
//       漏任何一条，EUI-NEO 侧 ellipsis 是**静默 no-op**（不是报错）。
//   (3) **结果**：渲染行宽 == measureTextWidth(保留前缀 + "…")，即末尾那个字形
//       **就是** U+2026（而不是「多留一个字」或「硬剪」）；且行宽 ≤ maxWidth。
//
// 变异口径（两条都真能改变行为）：
//   摘 `.overflow(Ellipsis)` → (3) 红（行宽变成整句宽，与「前缀+…」不等）。
//   摘 `.maxWidth(innerW)`   → (3) 红（ellipsis 静默不生效，同上）。
// 两者都不是等价变异：它们分别关掉「开哪个模式」和「给多宽」，都会让行宽回到
// 整句宽这一种**不同的**取值。
//
// ⚠ **实测踩过的坑（这一组第一版写成恒假）**：前提 (1) 一开始用
// `styleOfElement()` 去量「原文宽」，而那个 helper 会把元素上的 maxWidth /
// overflow 一起带进去 —— 量回来的是**已经被省略号截断过**的行宽（1055.96 ≤
// maxWidth 1064），于是「原文宽 > 可用宽」永远不成立，errRowsOverflowing 恒 0。
// 症状极具欺骗性：改长种子后数字**一动不动**。故前提 (1) 必须用下面
// `rawWidthOfElement()`（强制 Clip + maxWidth=0）单量，两者不可混用。

// U+2026 HORIZONTAL ELLIPSIS（UTF-8 = E2 80 A6），与 EUI-NEO 同一常量。
constexpr const char* kEllipsisGlyph = "\xE2\x80\xA6";

// 把 DSL 元素里与文本排版相关的字段原样搬进 TextStyle —— 与 runtime_render.h
// prepareText 的赋值列表一一对应（少搬一个就是「量了个不存在的东西」）。
core::TextStyle styleOfElement(const core::dsl::Element& el) {
    core::TextStyle st;
    st.text = el.text;
    st.fontFamily = el.fontFamily;
    st.fontSize = el.fontSize;
    st.fontWeight = el.fontWeight;
    st.maxWidth = el.maxWidth;
    st.wrap = el.wrap;
    st.overflow = el.overflow;
    st.lineHeight = el.lineHeight;
    return st;
}

// **未经任何截断的**整句宽度：强制 Clip 且 maxWidth=0（Clip 下 maxWidth 本就
// 不参与约束，见 EUI-NEO text.cpp measureTextSize），故这条路径与元素有没有
// 接 maxWidth / overflow 完全无关。专门用于组 I(c) 的前提 (1) 与「有没有被
// 截断」的判断——用它去量「原文宽」是本组第一版的真实 bug，勿复用
// styleOfElement。
float rawWidthOfElement(const core::dsl::Element& el) {
    core::TextStyle st = styleOfElement(el);
    st.maxWidth = 0.0f;
    st.overflow = core::TextOverflow::Clip;
    return core::TextPrimitive::measureTextSize(st).x;
}

// 按**渲染路径**量一行：裸 TextPrimitive + prepare() + measuredSize()。
// 不用静态 measureTextSize —— 那是 DSL 布局路径；两条路径必须同值，但断言要
// 断的是用户真正看到的那条（渲染），故这里走渲染，并在下面单独断言两者一致。
float renderedLineWidth(const core::dsl::Element& el) {
    core::TextPrimitive primitive;
    primitive.initialize();
    primitive.setText(el.text);
    primitive.setFontFamily(el.fontFamily);
    primitive.setFontSize(el.fontSize);
    primitive.setFontWeight(el.fontWeight);
    primitive.setMaxWidth(el.maxWidth);
    primitive.setWrap(el.wrap);
    primitive.setOverflow(el.overflow);
    primitive.setLineHeight(el.lineHeight);
    primitive.prepare();
    return primitive.measuredSize().x;
}

// 期望保留的字形数：最大的 keep 使 caretX[keep] + 省略号 advance <= maxWidth。
// caretX 与 shaped glyph 的 advance 序列同源（makeTextMetrics 逐字累加），
// byteIndices[k] = 前 k 个字形的 UTF-8 字节数 → 前缀 = substr(0, byteIndices[k])。
struct EllipsisExpectation {
    std::string prefix;   // 应当被保留的原文前缀（码点边界上）
    float expectedWidth = 0.0f;   // 「前缀 + …」的宽
    std::size_t keep = 0;
    std::size_t glyphCount = 0;
};

EllipsisExpectation expectedEllipsis(const core::dsl::Element& el) {
    const core::TextPrimitive::TextMetrics metrics =
        core::TextPrimitive::measureTextMetrics(
            el.text, el.fontFamily, el.fontSize, el.fontWeight);
    const float ellipsisWidth = core::TextPrimitive::measureTextWidth(
        kEllipsisGlyph, el.fontFamily, el.fontSize, el.fontWeight);
    std::size_t keep = 0;
    for (std::size_t index = 1; index < metrics.caretX.size(); ++index) {
        if (metrics.caretX[index] + ellipsisWidth > el.maxWidth) break;
        keep = index;
    }
    EllipsisExpectation out;
    out.keep = keep;
    out.glyphCount = metrics.caretX.empty() ? 0u : metrics.caretX.size() - 1u;
    const std::size_t byteEnd =
        keep < metrics.byteIndices.size() ? static_cast<std::size_t>(metrics.byteIndices[keep])
                                          : el.text.size();
    out.prefix = el.text.substr(0, byteEnd);
    out.expectedWidth = core::TextPrimitive::measureTextWidth(
        out.prefix + kEllipsisGlyph, el.fontFamily, el.fontSize, el.fontWeight);
    return out;
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }
    auto cfg = test_support::tempConfigFile("pfx_", "cfg");
    test_support::ScopedConfigPath scopedCfg(cfg);

    // 数据底座（组 D/E/F 前提）：已 migrate 的临时库注册为进程级
    // appDatabase（home onStart 拿它建 HomeService；null 则 statsError、
    // API 分布全零 → 分布图走空态分支不建图，组 D 结构前提失效）。
    // 种子 = 1 条 profile（claude_call_logs FK）+ 今日 2 条**带用量**的调用。
    // 2026-09-29 ③ 改口径后纵轴 = 每小时 output_tokens 合计：没有 output_tokens
    // 的行合计为 0（= 没数据）→ 依然走空态，组 D 的结构前提就没了，故这里必须
    // 给用量；两条落在 9 时与 17 时 → 去头去尾后恰 9 根柱（9..17 闭区间）。
    test_support::TestEnv env("hprobe_", "home", true);  // 注册为 appDatabase
    {
        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        const int64_t dayStart = module::home::startOfDayEpoch(
            static_cast<std::time_t>(now));
        auto& sq = env.db->db();
        sq.exec(
            "INSERT INTO claude_profiles (name, base_url, api_key_hint, "
            "api_key_check, model, created_at, updated_at) "
            "VALUES ('p1', 'http://x', 'sk-***', 'chk', 'm', " +
            std::to_string(now) + ", " + std::to_string(now) + ")");
        sq.exec(
            "INSERT INTO claude_call_logs (profile_id, method, path, "
            "status_code, latency_ms, output_tokens, started_at) VALUES "
            "(1, 'POST', '/v1/messages', 200, 2000, 400, " +
            std::to_string(dayStart + 3600 * 9) + "), "
            "(1, 'POST', '/v1/messages', 200, 2400, 600, " +
            std::to_string(dayStart + 3600 * 17) + ")");
    }

    core::proxy_stats::resetForTest();  // 进程内确定性：空闲无采样/无流

    std::unique_ptr<::core::Module> mod(create_home_module());
    mod->onStart();  // service_ + refreshStats（读种子库）

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    const std::string pageId = "home";
    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    bool passed = true;

    // ---- G7 余量卡种子（组 G/H 前提）----
    // p1（上一段的种子，无 quota 配置行）= 未配置态。
    // 默认再补 8 个**离线假** profile 走「正常 Tiered」+ 一个 Balance 型，逐行
    // 都是「行首 + 条 + （错误行）」。行数刻意超过卡片视口上限 → 组 H 能断言
    // 「出滚动条」。`--quota-fixture <json>` 则改用**真机数据**（探针 dump）。
    // 两种模式的其余流程逐字相同 —— 断言不因模式而改。
    std::vector<int64_t> fixtureProfileIds;  // 组 H(b) 收尾按 id 删（名字是供应商名）
    {
        using namespace module::claude::quota;
        QuotaStore store(env.db->db());
        const int64_t base = static_cast<int64_t>(std::time(nullptr));
        if (!args.quotaFixturePath.empty()) {
            const std::string err = loadQuotaFixture(
                args.quotaFixturePath, env.db->db(), store, &fixtureProfileIds);
            if (!err.empty()) {
                std::cerr << "home_probes: --quota-fixture 加载失败: " << err
                          << "（不退回假种子：图的身份必须可查）\n";
                passed = false;
            } else {
                std::cout << "home_probes: --quota-fixture 已种入 "
                          << fixtureProfileIds.size() << " 个真机 profile（"
                          << args.quotaFixturePath << "）\n";
            }
        } else {
            for (int i = 0; i < 9; ++i) {
                const std::string name = "quota-p" + std::to_string(i);
                if (!env.db->db()
                         .exec("INSERT INTO claude_profiles (name, base_url, "
                               "api_key_hint, api_key_check, model, created_at, "
                               "updated_at) VALUES ('" +
                                   name +
                                   "', 'https://example.invalid', 'sk-***', "
                                   "'chk', 'm', " +
                                   std::to_string(base) + ", " +
                                   std::to_string(base) + ")")
                         .isOk()) {
                    std::cerr << "home_probes: 余量种子 profile 插入失败\n";
                    passed = false;
                    break;
                }
                auto st = env.db->db().prepare("SELECT last_insert_rowid() AS id");
                auto rows = st.isOk()
                                ? st.value().fetchAll()
                                : core::Result<std::vector<core::data::SqlRow>>::fail(
                                      st.errorCode(), st.errorMessage());
                if (rows.isFail() || rows.value().empty()) {
                    std::cerr << "home_probes: 取余量种子 profileId 失败\n";
                    passed = false;
                    break;
                }
                const int64_t pid = rows.value()[0].getInt64("id").value();

                QuotaConfigRow cfg;
                cfg.profileId = pid;
                cfg.providerId = (i == 7) ? "deepseek" : "opencode_go";
                cfg.enabled = true;
                cfg.autoRefresh = true;
                cfg.intervalMinutes = 15;
                store.saveConfig(cfg);

                QuotaResult r;
                if (i == 8) {
                    // 组 I 前提：**唯一一条错误行**（Transient + 从未成功过
                    // → FailedNever → showsErrorLine 真）。没有它组 I 在门禁
                    // 里就是空跑（`ctest -L probe` 不带 --quota-fixture，只走
                    // 这批假种子）。文案刻意给长的：上游 detail 跟在 headline
                    // 后面，真机上就是这样（见 /tmp/af-home-quota-live.png 的
                    // MiniMax 行）—— 断言要钉的是「长文案也不越出内容列」。
                    //
                    // ⚠ **长度是实测钉出来的，不是拍脑袋**（1200×1000 实测）：
                    //   · 原种子（…可到控制台确认后再试）整句 1334.27px > 可用
                    //     1064px —— 确实溢出，但只有 1.25 倍，省略号只吃掉尾巴
                    //     ~25%，落图上**肉眼分不出**「省略号」与「硬剪」。
                    //   · 追加后半段后整句 2338.97px ≈ 可用宽的 2.2 倍，保留
                    //     114/232 字形、行宽 1055.96 ≤ 1064 —— 截掉一半，肉眼
                    //     一眼可辨，且不因换字号/换窗口宽就翻回不溢出。
                    //   · 真机 MiniMax 原文只有 224 字节 = 1049.63px < 1064
                    //     （**不溢出**），所以「能看见 …」的落图必须另造场景，
                    //     不能拿真机数据当证据。
                    // 组 I(c) 每次运行都打印这组实时数字，空跑守卫会报「还差多少 px」。
                    r.kind = QuotaKind::Tiered;
                    r.error = QuotaError::Transient;
                    // ⚠ errorMessage 是**供应商原文 detail**，不含 headline：
                    // quota_home_view.cpp 会自己拼 `headline + 「：」 + detail`，
                    // 种子再写一遍 headline 就会在卡上出现
                    // 「网络失败，稍后重试：网络失败，稍后重试：…」（实测过）。
                    r.errorMessage =
                        "套餐过期或无权限，换密钥没用，得续费"
                        "或换账号：example.invalid 接口业务错误（code 2062）："
                        "no active token plan subscription——可能套餐已过期或"
                        "额度已用尽，可到控制台确认后再试；该账号近 7 日已连续"
                        "三次返回同一错误码，若确认仍有效请直接联系供应商工单"
                        "（附 request id 0000-1111-2222-3333），不要重复重试"
                        "以免触发风控；换新密钥无效——配额绑定在账号而非密钥上";
                } else if (i == 7) {
                    r.kind = QuotaKind::Balance;
                    r.balanceRemaining = 8.5 + i;
                    r.currency = "CNY";
                } else {
                    r.kind = QuotaKind::Tiered;
                    QuotaWindow w;
                    w.key = (i % 2 == 0) ? "five_hour" : "weekly";
                    w.usedPercent = 20.0 + static_cast<double>(i) * 8.0;
                    if (i == 3) w.resetsAt = "2030-01-01T00:00:00";
                    r.windows.push_back(w);
                }
                store.applyFetchResult(pid, r, base);
            }
        }
    }


    core::dsl::Ui* ui = nullptr;
    // 单帧标准单元：pollOnce（输入派发 + 交互）→ compose（recompose 树 +
    // layout；spec §4.2 成对要求）
    auto composeFrame = [&]() {
        probe::composeModuleFrame(runtime, window, pageId, w, h,
                                  [&](eui::Ui& u, const eui::Screen& screen) {
                                      ui = &u;
                                      mod->compose(u, screen);
                                  });
    };
    auto settleFrames = [&](int n) {
        for (int i = 0; i < n; ++i) {
            composeFrame();
        }
    };

    settleFrames(2);  // 首帧构建树（结构 diff 计入 paint 预算，组 B 前消化）

    // ---- 「数据还在」的中途落图（--screenshot-seeded <路径>）----
    // 为什么必须多这一个点：组 H(b) 是**破坏性收尾断言**（清空余量表），退出点
    // 那张 --screenshot 拍到的是收尾后的空态。想看「余量卡长什么样」必须在此
    // 处拍。未给该开关 = 立刻返回（零开销），既有行为逐字不变。
    probe::captureSeedIfRequested(args, window, runtime, &passed);

    // ==================== 组 A：类 1(b) sparkline z（P1） ====================
    // home.stat.tokens.spark（lineChart chromeless 根，sparkwrap 顶带 y0..16，
    // home_module.cpp:159-164）必须整带位于 label（.label y16..32，
    // home_module.cpp:109-117）之上：不相交 + gapY ≥ 0（邻接语义；≥2 系
    // spec 实现前估量，见文件头）。回归旧 32px 盒 (0,0) → 相交 → 红。
    {
        const core::dsl::Element* spark =
            ui != nullptr ? ui->find("home.stat.tokens.spark") : nullptr;
        const core::dsl::Element* label =
            ui != nullptr ? ui->find("home.stat.tokens.label") : nullptr;
        if (spark == nullptr || label == nullptr) {
            std::cerr << "home_probes: 组 A 结构缺失 (spark="
                      << (spark != nullptr ? "ok" : "null")
                      << ", label=" << (label != nullptr ? "ok" : "null")
                      << ")——Token 卡/图表未构建\n";
            passed = false;
        } else {
            const float gapY = frameGapY(*spark, *label);
            if (framesIntersect(*spark, *label) || gapY < -0.5f) {
                std::cerr << "home_probes: 类1(b) FAIL——spark frame ("
                          << spark->frame.x << "," << spark->frame.y << " "
                          << spark->frame.width << "x" << spark->frame.height
                          << ") 与 label frame (" << label->frame.x << ","
                          << label->frame.y << " " << label->frame.width << "x"
                          << label->frame.height << ") 相交，gapY=" << gapY
                          << "（修复后 spark 应整带在 label 之上）\n";
                passed = false;
            }
        }
    }

    // ==================== 组 B：类 2(a) 空转重绘（P1） ====================
    // 稳定 15 帧（初始动画/hover 收敛，当前树结构与上帧一致不再 paint）后
    // 连跑 60 帧 pollOnce（不注入、不 recompose），计数 update() 返回 true
    // （= paintRequested_，runtime_lifecycle.h:229-233）。空闲态 tokenLive
    // false → 无 .timer（home_module.cpp:462-477）→ 期望 ≤1（首帧遗留）。
    {
        settleFrames(15);
        int paintFrames = 0;
        for (int i = 0; i < 60; ++i) {
            if (probe::pollOnce(window, runtime)) {
                ++paintFrames;
            }
        }
        if (paintFrames > 1) {
            std::cerr << "home_probes: 类2(a) FAIL——空转 60 帧 update() 返 "
                         "true " << paintFrames
                      << " 帧（期望 ≤1；onTimer 常驻整页重绘回归？）\n";
            passed = false;
        }
    }

    // ==================== 组 C：类 2(b) hover token（P1） ====================
    // hover != surface：深色档（默认 current()）天然不等；浅色档是审计根因
    //（旧 #F5F2EC == surface #F5F2EC，theme.cpp:59-64 修后 #EDE8DF）。双档
    // 各验一次，验完还原深色（后续段几何不受主题影响，保持确定性）。
    {
        bool cOk = true;
        const auto checkTheme = [&](const char* themeName) {
            const auto& t = core::theme::current();
            if (!colorsDiffer(t.colors.hover, t.colors.surface)) {
                std::cerr << "home_probes: 类2(b) FAIL——" << themeName
                          << " 档 hover (" << t.colors.hover.r << ","
                          << t.colors.hover.g << "," << t.colors.hover.b
                          << ") == surface (" << t.colors.surface.r << ","
                          << t.colors.surface.g << "," << t.colors.surface.b
                          << ")（hover 反馈不可见）\n";
                cOk = false;
            }
        };
        checkTheme("深色默认");
        core::theme::setLight();
        checkTheme("浅色");
        core::theme::setDark();
        if (!cOk) {
            passed = false;
        }
    }

    // ==================== 组 D/E 前：API 分布卡当前帧 ====================
    composeFrame();

    // ==================== 组 D：类 4(a) barChart 几何（P1） ====================
    // plot 区总高 = grid.3.y - grid.0.y（barchart.h:114-122；band 不依赖 bar
    // 的 0.16s 帧过渡）。种子的非零用量 → 非空态 → 去头去尾后 9 根柱（9..17）
    // 正常建图（2026-09-29 ③ 改口径：纵轴 = 每小时 output_tokens 合计）；
    // home_module.cpp:567-570 审计修：卡高 260 → wrap 196 → plot 84 ≥ 36。
    // 回归卡高 208 → wrap 128 → plot 16 < 36 → 红。
    {
        const core::dsl::Element* grid0 =
            ui != nullptr ? ui->find("home.api.chart.bars.grid.0") : nullptr;
        const core::dsl::Element* grid3 =
            ui != nullptr ? ui->find("home.api.chart.bars.grid.3") : nullptr;
        if (grid0 == nullptr || grid3 == nullptr) {
            std::cerr << "home_probes: 类4(a) 结构缺失 (grid.0="
                      << (grid0 != nullptr ? "ok" : "null")
                      << ", grid.3=" << (grid3 != nullptr ? "ok" : "null")
                      << ")——barChart 未构建（种子落空或走空态分支）\n";
            passed = false;
        } else {
            const float plotH = grid3->frame.y - grid0->frame.y;
            if (plotH < 35.5f) {
                std::cerr << "home_probes: 类4(a) FAIL——bar 面板 plot 区总高 "
                          << plotH << "px < 36（审计修 260 卡高回退？）\n";
                passed = false;
            }
        }
    }

    // ==================== 组 E：类 5(a) API 卡对称（P3） ====================
    // title/wrap 相对卡 content（inset 16）以 (0,·) 定位 → 左右间隙各 16，
    // |左 - 右| ≤ 2px（任务口径；master 实测差 0）。回归二次
    // position(16,..) → 左 32 右 0。
    {
        const core::dsl::Element* card =
            ui != nullptr ? ui->find("home.api.chart") : nullptr;
        const core::dsl::Element* title =
            ui != nullptr ? ui->find("home.api.chart.title") : nullptr;
        const core::dsl::Element* wrap =
            ui != nullptr ? ui->find("home.api.chart.wrap") : nullptr;
        if (card == nullptr || title == nullptr || wrap == nullptr) {
            std::cerr << "home_probes: 类5(a) 结构缺失 (card="
                      << (card != nullptr ? "ok" : "null")
                      << ", title=" << (title != nullptr ? "ok" : "null")
                      << ", wrap=" << (wrap != nullptr ? "ok" : "null")
                      << ")\n";
            passed = false;
        } else {
            const float cardRight = card->frame.x + card->frame.width;
            const auto checkSymmetric = [&](const char* name,
                                            const core::dsl::Element& el) {
                const float leftGap = el.frame.x - card->frame.x;
                const float rightGap =
                    cardRight - (el.frame.x + el.frame.width);
                if (std::fabs(leftGap - rightGap) > 2.0f) {
                    std::cerr << "home_probes: 类5(a) FAIL——" << name
                              << " 左右间隙差 " << (leftGap - rightGap)
                              << "px（左 " << leftGap << " 右 " << rightGap
                              << "；content 已含 inset，不应二次偏移）\n";
                    passed = false;
                }
            };
            checkSymmetric("title", *title);
            checkSymmetric("wrap", *wrap);
        }
    }

    // ==================== 组 F：类 6(a) stat 溢出（P1） ====================
    // 注入：测试时钟（每次 nowMs() +1000ms）+ 单流累计 20M→40M → 2s 窗口
    // delta 20M → rateTps=10M → 「10000000」（8 位）。慢 48px 档 → 内在宽
    // ≈230 > cw 176 必溢；statNumberFont（home_module.cpp:34-42）收缩到
    // 36px → ≈173 ≤ 176 收纳。badge = 流式档「● 1 路 · 累计 40M」。
    // 判别两段式（见文件头）：clip/wrap true → 过；否则 measureTextSize。
    {
        int64_t fakeT = -1;
        core::proxy_stats::setTestClock([&fakeT]() {
            fakeT += 1000;
            return fakeT;
        });
        const uint64_t streamId = core::proxy_stats::beginStream();
        core::proxy_stats::updateStream(streamId, 20000000);
        core::proxy_stats::updateStream(streamId, 40000000);
        composeFrame();
        const int kMinWindowW = 1200;  // 最小窗宽：statW 208 → cw 176（判别几何）
        for (const char* cardId : kStatCardIds) {
            for (const char* suffix : {"number", "badge"}) {
                const std::string elId =
                    std::string(cardId) + "." + suffix;
                const core::dsl::Element* el =
                    ui != nullptr ? ui->find(elId) : nullptr;
                if (el == nullptr) {
                    std::cerr << "home_probes: 类6(a) 结构缺失 '" << elId
                              << "'\n";
                    passed = false;
                    continue;
                }
                if (textFitsOrGuarded(*el)) {
                    continue;
                }
                core::TextStyle style;
                style.text = el->text;
                style.fontSize = el->fontSize;
                style.fontWeight = el->fontWeight;
                const core::Vec2 intrinsic =
                    core::TextPrimitive::measureTextSize(style);
                std::cerr << "home_probes: 类6(a) FAIL——'" << elId
                          << "' 文本「" << el->text << "」内在宽 "
                          << intrinsic.x << "px > 可用宽 "
                          << el->frame.width << "px（clip="
                          << (el->clip ? 1 : 0) << " wrap="
                          << (el->wrap ? 1 : 0)
                          << "；窗宽 " << args.windowW
                          << (args.windowW >= kMinWindowW
                                  ? ""
                                  : " ——最小窗 1200 判定！")
                          << "，字号收缩或文案长度回退？）\n";
                passed = false;
            }
        }
        core::proxy_stats::endStream(streamId);
        core::proxy_stats::resetForTest();  // 清时钟与状态
    }

    // ==================== 组 G：G7 余量卡几何（P1） ====================
    // 四条，都用**数字**判（不接受「看起来对」）：
    //   G(a) 余量卡横向不溢出**内容列**（不再是「窗口」——整页已挂进
    //       home.scroll，出滚动条时内容列比窗口窄，判窗口是判错对象）
    //   G(b) **上方统计网格的 x/y/h 逐字不变**，且宽 == 内容列宽 − pad×2
    //   G(c) 余量卡在统计网格**之下**；快捷按钮行**没被挤出滚动内容**
    //       （⚠ 2026-10-03 改口径：原判据是「底 ≤ 窗高」，那编码的是
    //        「整页必须塞进视口」——正是矮窗看不到余量卡的根因。现在整页挂在
    //        home.scroll 上，页脚**允许**在视口之外，但**必须仍在内容盒内**
    //        = 用户滚得到。判据从「塞得进视口」改成「没被挤出内容」，不是
    //        删掉断言。）
    //   G(d) **余量卡在任意屏高都构建**（矮窗不再两面落空，见组 I 注释）
    {
        composeFrame();
        const core::dsl::Element* card =
            ui != nullptr ? ui->find("home.quota") : nullptr;
        const core::dsl::Element* stats =
            ui != nullptr ? ui->find("home.stats") : nullptr;
        const core::dsl::Element* acts =
            ui != nullptr ? ui->find("home.actions") : nullptr;
        const core::dsl::Element* root =
            ui != nullptr ? ui->find("home.root") : nullptr;
        const core::dsl::Element* pageScroll =
            ui != nullptr ? ui->find("home.scroll.content") : nullptr;
        if (card == nullptr || stats == nullptr || root == nullptr ||
            pageScroll == nullptr) {
            std::cerr << "home_probes: 组 G 结构缺失 (card="
                      << (card != nullptr ? "ok" : "null")
                      << ", stats=" << (stats != nullptr ? "ok" : "null")
                      << ", root=" << (root != nullptr ? "ok" : "null")
                      << ", scrollContent="
                      << (pageScroll != nullptr ? "ok" : "null") << ")\n";
            passed = false;
        } else {
            const float scale = core::theme::fontScale();
            // 期望值来自 home_module.cpp 的既有常量（pad 40 / header 72 /
            // gap 20 / statH 144×fontScale），不是从当前帧反推的
            // （反推等于「测了它自己」）。**宽度不再写死成「屏宽 − 80」**：
            // 出滚动条时内容列按框架口径收窄（scrollbarWidth + gap），那是
            // 正确行为。改判「== 内容列宽 − pad×2」才是这条断言真正想守的
            // 不变式——统计网格必须**铺满内容列**，不多不少。
            const float expectPad = 40.0f;
            const float expectHeaderH = 72.0f;
            const float expectGap = 20.0f;
            const float expectStatH = 144.0f * scale;
            const float expectStatsY = expectPad + expectHeaderH + expectGap;
            const float expectStatsW = pageScroll->frame.width -
                                       expectPad * 2.0f;
            if (std::fabs(stats->frame.x - expectPad) > 0.5f ||
                std::fabs(stats->frame.width - expectStatsW) > 0.5f ||
                std::fabs(stats->frame.y - expectStatsY) > 0.5f ||
                std::fabs(stats->frame.height - expectStatH) > 0.5f) {
                std::cerr << "home_probes: 组 G(b) FAIL——统计网格几何被顶走："
                          << "x=" << stats->frame.x << "(期望 " << expectPad
                          << ") w=" << stats->frame.width << "(期望 "
                          << expectStatsW << ") y=" << stats->frame.y
                          << "(期望 " << expectStatsY << ") h="
                          << stats->frame.height << "(期望 " << expectStatH
                          << ")\n";
                passed = false;
            }
            // (a) 横向不溢出**内容列**（判窗口是判错对象：出滚动条时内容列比窗口窄）
            if (card->frame.x < -0.5f ||
                card->frame.x + card->frame.width >
                    pageScroll->frame.width + 0.5f) {
                std::cerr << "home_probes: 组 G(a) FAIL——余量卡横向溢出：x="
                          << card->frame.x << " w=" << card->frame.width
                          << "（内容列宽 " << pageScroll->frame.width
                          << "，窗宽 " << args.windowW << "）\n";
                passed = false;
            }
            // (c) 在统计网格之下且不相交
            if (card->frame.y < stats->frame.y + stats->frame.height - 0.5f) {
                std::cerr << "home_probes: 组 G(c) FAIL——余量卡压住统计网格："
                          << "card.y=" << card->frame.y << " stats 底="
                          << (stats->frame.y + stats->frame.height) << "\n";
                passed = false;
            }
            // (c) 快捷按钮行仍在**滚动内容盒内**（用户滚得到）。
            // ⚠ 2026-10-03 改口径，理由见文件头组 G 注释：整页挂上 home.scroll
            // 之后，页脚**允许**在视口之外（那正是滚动存在的意义），但绝不能
            // 溢出内容盒——溢出去就等于滚到底也够不着 = 真丢内容。
            if (acts != nullptr) {
                const float actsBottom = acts->frame.y + acts->frame.height;
                const float contentBottom = root->frame.y + root->frame.height;
                if (actsBottom > contentBottom + 0.5f) {
                    std::cerr << "home_probes: 组 G(c) FAIL——快捷按钮行溢出滚动"
                              << "内容盒（滚到底也够不着）：底=" << actsBottom
                              << " > 内容底=" << contentBottom << "\n";
                    passed = false;
                }
            }
            // (d) 余量卡在**当前屏高**确实构建出来了。
            // 这是 G7 遗留 1 / G8 遗留 1 的判据本体：矮窗下旧实现两面落空
            // （压不出余量卡 + 余量卡整张不画）。⚠ 本条只在探针实际跑的那个
            // 屏高上有判别力 —— 默认 1200×1000 旧实现也画，所以门禁里另有
            // home_probes_min_window（--size 1200x720）专门跑设计最小窗；
            // 预算纯函数那条另有单测（test_home_quota_card 的
            // home_quota_budget_* ），三处合起来才是完整守卫。
            if (card->frame.height <= 0.0f || card->frame.width <= 0.0f) {
                std::cerr << "home_probes: 组 G(d) FAIL——余量卡已构建但零尺寸："
                          << card->frame.width << "x" << card->frame.height
                          << "（屏高 " << args.windowH << "）\n";
                passed = false;
            }
        }
    }

    // ==================== 组 I：G7 错误行只出现一次 + 不溢出 ====================
    // 缺陷（2026-10-03 修）：错误文案被渲染**两遍** —— ① head row 的 `.status`
    // 格（statusTextFor 在 showsErrorLine 时返回 row.lastError）② 专属错误行
    // `.error`。且 `.status` 是既无 clip 又无 wrap 的单行盒（maxWidth 只在
    // wrap=true 时生效，见 EUI-NEO text.cpp），于是第一遍直接溢出卡片右缘被
    // 截断。真机证据：/tmp/af-home-quota-live.png 的 MiniMax 行。
    //
    // I(a) **只出现一次**：错误文案在整棵余量树里**有且仅有一处**携带它 ——
    //     钉「字符串在树里出现几次」而不是「某个格是不是空」，后者回归成
    //     「换个别的文案渲染两遍」就又绿了。
    // I(b) **不溢出内容列**：错误文案所在格的**右缘** ≤ 内容列右缘。
    //     钉的是**宽度比较**（本格右缘 vs 父行内容宽），不是某个字面量宽度 ——
    //     换字号 / 换文案长度 / 换窗口宽都仍成立。
    // 组 I(c) 的实际断言覆盖面：真正**溢出**内容列的错误行数。组尾钉 >=1，
    // 防止「种子文案不够长 → 一格都没溢出 → I(c) 整组空跑成恒真假绿」。
    // 声明在组外：本组尾部（组 I 的 {} 之外）还要读它做空跑守卫。
    int errRowsOverflowing = 0;
    // 空跑守卫的诊断四件套（只服务于「种子文案不够长」这一种失败）。
    float errWidestGap = -1.0e30f;
    std::string errWidestRowId;
    float errWidestRaw = 0.0f;
    float errWidestAvailable = 0.0f;
    std::string errWidestText;
    {
        const auto rows =
            module::claude::quota::listQuotaHomeRows(env.db->db());
        // 错误行必须真的存在，否则本组是空跑（恒真假绿）。门禁那条不带
        // --quota-fixture，靠上面的 i==8 假种子保证有错误行。
        int errorRows = 0;
        for (const auto& r : rows.value()) {
            const bool isErr = r.state == module::claude::quota::QuotaCardState::Incomplete ||
                               r.state == module::claude::quota::QuotaCardState::FailedNever ||
                               r.state == module::claude::quota::QuotaCardState::FailedStale;
            if (isErr && !r.lastError.empty()) ++errorRows;
        }
        if (rows.isFail() || errorRows < 1) {
            std::cerr << "home_probes: 组 I FAIL——没有错误行可断言（错误行 "
                      << errorRows << " 条）：本组会空跑成恒真假绿\n";
            passed = false;
        }
        for (const auto& row : rows.value()) {
            const bool isErr =
                row.state == module::claude::quota::QuotaCardState::Incomplete ||
                row.state == module::claude::quota::QuotaCardState::FailedNever ||
                row.state == module::claude::quota::QuotaCardState::FailedStale;
            if (!isErr || row.lastError.empty()) continue;

            const std::string rid =
                "home.quota.row." + std::to_string(row.profileId);
            const core::dsl::Element* rowEl =
                ui != nullptr ? ui->find(rid) : nullptr;
            const core::dsl::Element* errEl =
                ui != nullptr ? ui->find(rid + ".error") : nullptr;
            const core::dsl::Element* statusEl =
                ui != nullptr ? ui->find(rid + ".status") : nullptr;
            if (rowEl == nullptr || errEl == nullptr || statusEl == nullptr) {
                std::cerr << "home_probes: 组 I FAIL——错误行结构缺失 " << rid
                          << "（row=" << (rowEl != nullptr ? "ok" : "null")
                          << ", error=" << (errEl != nullptr ? "ok" : "null")
                          << ", status=" << (statusEl != nullptr ? "ok" : "null")
                          << "）\n";
                passed = false;
                continue;
            }

            // ---- I(a) 错误文案在整棵余量树里只出现一次 ----
            // 递归扫 `home.quota` 子树，逐个 Text 元素数「文本里含该错误文案」
            // 的格数。专属错误行必须**是**携带者之一（否则就是全树都没有）。
            int carriers = 0;
            std::vector<std::string> carrierIds;
            std::vector<const core::dsl::Element*> stack{rowEl};
            while (!stack.empty()) {
                const core::dsl::Element* el = stack.back();
                stack.pop_back();
                if (el->kind == core::dsl::ElementKind::Text &&
                    el->text.find(row.lastError) != std::string::npos) {
                    ++carriers;
                    carrierIds.push_back(el->id);
                }
                for (const core::dsl::Element* child : el->orderedChildren) {
                    stack.push_back(child);
                }
            }
            if (carriers != 1 || carrierIds.front() != rid + ".error") {
                std::cerr << "home_probes: 组 I(a) FAIL——错误文案在余量树里出现 "
                          << carriers << " 次（应恰好 1 次，且必须在专属错误行 "
                          << rid << ".error）：";
                for (const std::string& id : carrierIds) {
                    std::cerr << " [" << id << "]";
                }
                std::cerr << "；.status 文本=「" << statusEl->text << "」\n";
                passed = false;
            }

            // ---- I(b) 不溢出内容列：本格右缘 ≤ 内容列右缘 ----
            // 宽度比较，不钉字面量：内容列 = 该行 `.head` 的内容右缘（即
            // scrollView 给的 innerW），错误行与之同宽，故直接比父行右缘。
            const float contentRight = rowEl->frame.x + rowEl->frame.width;
            const float errRight = errEl->frame.x + errEl->frame.width;
            if (errRight > contentRight + 0.5f) {
                std::cerr << "home_probes: 组 I(b) FAIL——错误行右缘 " << errRight
                          << " 越出内容列右缘 " << contentRight << "（超出 "
                          << (errRight - contentRight) << "px）\n";
                passed = false;
            }
            // 同一条对 head 的 .status 也钉一遍：它现在恒空（错误文案归错误行），
            // 但**别的状态**下的状态标签也不该越出内容列——那正是本次顺带修的
            // clip 缺失（回归 = 去掉 .clip() 且状态文案够长 →本格越界）。
            const float statusRight = statusEl->frame.x + statusEl->frame.width;
            if (statusRight > contentRight + 0.5f) {
                std::cerr << "home_probes: 组 I(b) FAIL——head .status 右缘 "
                          << statusRight << " 越出内容列右缘 " << contentRight
                          << "\n";
                passed = false;
            }
            // 防溢护栏三选一：clip（scissor 硬剪）/ wrap（换行）/ 单行省略号
            // （overflow==Ellipsis && maxWidth>0 && !wrap）。⚠ 量的必须是
            // **元素真实声明的样式**（含 maxWidth/overflow），否则量的是「没接
            // 线时的文本宽」——省略号那一格会被误判成「溢出」（假红），而
            // 没接 maxWidth 的格子又会因为漏量而被放过（假绿）。
            for (const core::dsl::Element* el : {errEl, statusEl}) {
                if (el->clip || el->wrap) continue;
                core::TextStyle st;
                st.text = el->text;
                st.fontFamily = el->fontFamily;
                st.fontSize = el->fontSize;
                st.fontWeight = el->fontWeight;
                st.maxWidth = el->maxWidth;
                st.overflow = el->overflow;
                st.wrap = el->wrap;
                st.lineHeight = el->lineHeight;
                const float w = core::TextPrimitive::measureTextSize(st).x;
                if (w > el->frame.width + 1.0f) {
                    std::cerr << "home_probes: 组 I(b) FAIL——" << el->id
                              << " 无 clip/wrap 且文案宽 " << w << " > 盒宽 "
                              << el->frame.width << "（会画出内容列）\n";
                    passed = false;
                }
            }

            // ---- I(c) 超长错误文案以「…」收尾（不是被硬剪）----
            // 只在**这一格真的溢出**时才断言：(1) 前提（原文按 Clip 量 > 内容列
            // 宽）；不溢出时本组对它无话可说，跳过——但跳过次数被 errorRowsKnown
            // 记着，组尾统一钉「至少有一格真溢出」，否则整组空跑成恒真假绿。
            const float rawWidth = rawWidthOfElement(*errEl);
            const float errAvailable = contentRight - errEl->frame.x;
            // 记下最接近溢出的那一格，供组尾空跑守卫把数字打出来——「种子太短」
            // 这种空跑必须一眼看出差多少，而不是只报一个 0。
            if (rawWidth - errAvailable > errWidestGap) {
                errWidestGap = rawWidth - errAvailable;
                errWidestRowId = errEl->id;
                errWidestRaw = rawWidth;
                errWidestAvailable = errAvailable;
                errWidestText = errEl->text;
            }
            if (rawWidth > errAvailable + 0.5f) {
                ++errRowsOverflowing;
                const float errMaxWidth = errEl->maxWidth;
                if (errEl->overflow != core::TextOverflow::Ellipsis) {
                    std::cerr << "home_probes: 组 I(c) FAIL——" << errEl->id
                              << " 溢出（原文宽 " << rawWidth << " > 可用 "
                              << (contentRight - errEl->frame.x)
                              << "）但 overflow != Ellipsis：文案被静默硬剪\n";
                    passed = false;
                } else if (errEl->wrap) {
                    std::cerr << "home_probes: 组 I(c) FAIL——" << errEl->id
                              << " wrap==true：省略号是单行语义，wrap 下是 no-op\n";
                    passed = false;
                } else if (errMaxWidth <= 0.0f) {
                    std::cerr << "home_probes: 组 I(c) FAIL——" << errEl->id
                              << " maxWidth==0：ellipsis 静默不生效（runtime 会回退到 "
                              "frame.width，DSL 布局却按 0 走，两条路会分叉）\n";
                    passed = false;
                } else {
                    const EllipsisExpectation expect = expectedEllipsis(*errEl);
                    const float rendered = renderedLineWidth(*errEl);
                    // 一行摘要：本组**实际断言了什么**（长度、保留了多少字形、
                    // 截到多宽）。没有它，「组 I(c) 绿」只说明「没红」，看不出
                    // 它是在多长的文案上真跑过的——而这正是本组最容易被悄悄
                    // 退化成空跑的地方。
                    std::cout << "home_probes: 组 I(c) " << errEl->id
                              << " 原文宽 " << rawWidth << " > 可用 " << errAvailable
                              << " → 保留 " << expect.keep << "/" << expect.glyphCount
                              << " 字形，行宽 " << rendered << " ≤ maxWidth "
                              << errMaxWidth << "，末尾为 U+2026\n";
                    // (3a) 末尾那个字形**就是** U+2026：行宽逐位等于「保留前缀 + …」。
                    //      若框架改成多留一个字 / 不加省略号 / 硬剪，行宽就落在别的
                    //      取值上，这里立刻红。
                    if (std::fabs(rendered - expect.expectedWidth) > 0.01f) {
                        std::cerr << "home_probes: 组 I(c) FAIL——" << errEl->id
                                  << " 渲染行宽 " << rendered
                                  << " != 「前缀(保留 " << expect.keep << "/"
                                  << expect.glyphCount << " 字形) + …」的宽 "
                                  << expect.expectedWidth
                                  << "：末尾不是省略号\n";
                        passed = false;
                    }
                    // (3b) 行总宽 ≤ 内容列（不越出右缘）。
                    if (rendered > errMaxWidth + 0.01f) {
                        std::cerr << "home_probes: 组 I(c) FAIL——" << errEl->id
                                  << " 行宽 " << rendered << " > maxWidth "
                                  << errMaxWidth << "：仍会画出内容列\n";
                        passed = false;
                    }
                    // (3c) 截断确实发生了（不是「本来就放得下」的空转）。
                    if (expect.keep >= expect.glyphCount) {
                        std::cerr << "home_probes: 组 I(c) FAIL——" << errEl->id
                                  << " 一字未截（保留 " << expect.keep << "/"
                                  << expect.glyphCount << "）却判溢出："
                                  << "前提(1) 与实际不符\n";
                        passed = false;
                    }
                    // (3d) 渲染路径与 DSL 布局路径（measureTextSize）同值——
                    //      两者分叉 = 盒宽按一套排、字按另一套画。
                    const float measured =
                        core::TextPrimitive::measureTextSize(styleOfElement(*errEl)).x;
                    if (std::fabs(measured - rendered) > 0.01f) {
                        std::cerr << "home_probes: 组 I(c) FAIL——" << errEl->id
                                  << " 渲染行宽 " << rendered
                                  << " != DSL 测量宽 " << measured
                                  << "：两条路径分叉\n";
                        passed = false;
                    }
                }
            }
        }
        // I(c) **`.status` 的防溢护栏必须在场**（静态审计 m6）。
        //
        // 原实现只在**错误行**上查 clip/wrap，而修复后错误行的 .status 文本
        // **恒为空串** → measureTextSize("") = 0 → 那条宽度检查对 .status 是
        // 恒真的空转（审计实测：删掉 .status 的 .clip()，14/14 probe 全绿）。
        //
        // ⚠ 判据为什么是「护栏在不在」而不是「文本有没有超出盒宽」：
        //   当前三种非错误状态的标签（未配置余量查询 / 配置不完整，去补全 /
        //   等待首次查询）在任何窗口宽下都装得进 statusW → 宽度判据恒不触发，
        //   那才是**又一条恒真假绿**（本条第一版就是这么写的，实测删掉
        //   .clip() 仍然全绿，当场拆穿）。
        //   而「.status 是固定高单行盒 + 上游文案长度不可控 → 必须有 clip 或
        //   wrap」是代码里**明写的不变式**（home_quota_card.cpp 的 .status
        //   段：「maxWidth 只在 wrap=true 时生效……故防溢只能靠 clip 剪裁」）。
        //   断言这条不变式本身，才是对「护栏在不在」的真正钉死。
        int statusCheckedRows = 0;
        for (const auto& row : rows.value()) {
            const std::string rid =
                "home.quota.row." + std::to_string(row.profileId);
            const core::dsl::Element* statusEl =
                ui != nullptr ? ui->find(rid + ".status") : nullptr;
            if (statusEl == nullptr) continue;
            ++statusCheckedRows;
            if (statusEl->clip || statusEl->wrap) continue;
            std::cerr << "home_probes: 组 I(c) FAIL——" << statusEl->id
                      << " 既无 clip 也无 wrap（文本=\"" << statusEl->text
                      << "\"）：.status 是固定高单行盒且 maxWidth 只在 wrap=true "
                         "时生效，无护栏 = 上游长文案直接画出内容列\n";
            passed = false;
        }
        // 非恒真守卫的第一道闸：这条检查一行都没跑到 = 空转，必须报红。
        // 「过滤器匹配 0 条时静默绿」是本项目踩过多次的坑。
        if (statusCheckedRows < 1) {
            std::cerr << "home_probes: 组 I(c) FAIL——一行 .status 都没找到（读到 "
                      << statusCheckedRows
                      << " 行）：本轮空转 = 没有守卫\n";
            passed = false;
        }
    }
    // 组 I(c) 空跑守卫：必须真的有一格溢出可断言。
    if (errRowsOverflowing < 1) {
        std::cerr << "home_probes: 组 I(c) FAIL——没有任何一条错误文案超出内容列"
                  << "（errRowsOverflowing=" << errRowsOverflowing
                  << "）：本组空跑成恒真假绿，种子文案需要更长";
        if (errWidestRowId.empty()) {
            std::cerr << "（一行错误文案都没有）\n";
        } else {
            std::cerr << "；最接近溢出的 " << errWidestRowId << "：原文宽 "
                      << errWidestRaw << " vs 可用 " << errWidestAvailable
                      << "（还差 " << -errWidestGap << "px）；实际文案前 60 字「"
                      << errWidestText.substr(0, 60) << "」（共 "
                      << errWidestText.size() << " 字节）\n";
        }
        passed = false;
    }

    // ==================== 组 H：G7 余量卡滚动（P1） ====================
    // H(a) 滚动条**由内容溢出驱动**：出滚动条 ⟺ 内容高 > 视口高。
    //     「出现」这一半是原有断言；「不溢出时不出现」这一半是 2026-10-03 补的
    //     ——它让本断言在 --quota-fixture 模式（真机数据只有 4~5 行、内容未必
    //     溢出）与「窗口更高 → 视口更大 → 内容装得下」两种情形下依然有意义，
    //     而不是因为行数变了就整条失去判别力。
    //     判据用 computeQuotaCardMetrics（home_quota_card.h 的公开契约函数），
    //     cap 取**实测视口高**：于是 overflows 恰好等于「内容超出这个视口」。
    //     ⚠ bar 为 null 不直接判红——那可能正是「不溢出」的正确答案；红不红由
    //       overflows 与 bar 存在性是否一致决定。
    // H(b) 删掉全部余量数据后重排 → **滚动条消失**（证明它是内容驱动的，
    //      不是「无条件画一根」的装饰）
    {
        const core::dsl::Element* bar =
            ui != nullptr ? ui->find("home.quota.scroll.scroll") : nullptr;
        const core::dsl::Element* view =
            ui != nullptr ? ui->find("home.quota.scroll") : nullptr;
        // 溢出判据：内容（行高之和 + 行间距）是否超过视口。用桥读回真实行，
        // 与卡片渲染的是同一份数据（否则就是拿种子猜渲染结果）。
        const auto rows =
            module::claude::quota::listQuotaHomeRows(env.db->db());
        if (view == nullptr) {
            std::cerr << "home_probes: 组 H(a) FAIL——滚动视口未构建\n";
            passed = false;
        } else if (rows.isFail() || rows.value().empty()) {
            std::cerr << "home_probes: 组 H(a) FAIL——listQuotaHomeRows "
                      << (rows.isFail() ? "失败: " + rows.errorMessage()
                                        : std::string("返回空"))
                      << "，拿不到内容高度\n";
            passed = false;
        } else {
            const auto m = module::home::quota_card::computeQuotaCardMetrics(
                rows.value(), 0.0f, view->frame.height);
            if (m.overflows != (bar != nullptr)) {
                std::cerr << "home_probes: 组 H(a) FAIL——内容 "
                          << (m.overflows ? "溢出" : "不溢出") << "视口 "
                          << view->frame.height << "px，但滚动条"
                          << (bar != nullptr ? "在" : "不在")
                          << "（滚动条必须由内容溢出驱动）\n";
                passed = false;
            }
            if (bar != nullptr &&
                (bar->frame.height > view->frame.height + 0.5f ||
                 bar->frame.height <= 0.0f)) {
                std::cerr << "home_probes: 组 H(a) FAIL——滚动条高 "
                          << bar->frame.height << " 越界（视口高 "
                          << view->frame.height << "）\n";
                passed = false;
            }
        }
    }
    {
        // H(b)：清空余量数据**并删掉这批种子 profile** 后重排，滚动条必须消失。
        // ⚠ 只删余量两表是不够的：剩下的 profile 全是「未配置」行，9 行 ×
        // (24 头 + 10 间距) = 296 > 250 视口，**本来就该**出滚动条——那是
        // 正确行为，断言它「不该出」才是错的。
        // ⚠ 假种子按名字前缀删（quota-p%），**真机种子按 id 删**（--quota-fixture
        // 的 profile 名就是供应商名，前缀删不掉——漏删会让本断言在真机模式下
        // 测的根本不是「只剩一行」）。
        auto& sq = env.db->db();
        std::string cleanup = "DELETE FROM claude_profiles WHERE name LIKE 'quota-p%'";
        for (const int64_t fid : fixtureProfileIds) {
            cleanup += " OR id = " + std::to_string(fid);
        }
        if (!sq.exec("DELETE FROM profile_quota_config").isOk() ||
            !sq.exec("DELETE FROM profile_quota_result").isOk() ||
            !sq.exec(cleanup).isOk()) {
            std::cerr << "home_probes: 组 H(b) 清库失败\n";
            passed = false;
        }
        composeFrame();
        composeFrame();
        const core::dsl::Element* bar =
            ui != nullptr ? ui->find("home.quota.scroll.scroll") : nullptr;
        if (bar != nullptr) {
            std::cerr << "home_probes: 组 H(b) FAIL——只剩 1 行（未配置）时"
                      << "仍画了滚动条（内容不满视口不该出）\n";
            passed = false;
        }
        if (ui != nullptr && ui->find("home.quota.row.1.config.bg") == nullptr) {
            std::cerr << "home_probes: 组 H(b) FAIL——未配置行的「去配置」按钮"
                      << "在清库后消失了\n";
            passed = false;
        }
    }

    // ---- 退出前落一张「当前状态图」（--screenshot <路径>，2026-10-03 帧捕获批次）----
    // 通用看图口：跑一次 home_probes 就能拿到此刻首页长什么样，不必再靠
    // screencapture（agent 会话拿不到屏幕录制 TCC 授权）。不传该开关时
    // captureIfRequested 立刻返回，行为与从前逐字一致（零开销 / 不写文件）。
    probe::captureIfRequested(args, window, runtime, &passed);

    mod->onStop();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!passed) std::cerr << "FAIL\n";
    return passed ? 0 : 1;
}