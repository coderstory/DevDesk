// home_probes.cpp — home 模块几何探针。组 A..M，每组一段，各自带判据注释。
//
// 运行：home_probes.exe [--frames N] [--size 1200x600]
// 不带 --quota-fixture 时全程离线假种子，可复现、不打任何供应商接口。
// statW 只随窗**宽**变，所以各组几何在默认 1200x1000 与守护短窗 1200x600 下同型
//（600 < 设计最小窗高 640，UAT3 D-R3-2 起刻意低于一切真实下限作守护性深档）。

#include "probes/probe_harness.h"
#include "test_support.h"
#include "app_registry.h"
#include "core/module_interface.h"
#include "core/proxy_stats.h"
#include "module_home/home_quota_card.h"   // computeQuotaCardMetrics（组 H(a) 判据）
#include "module_home/home_service.h"  // startOfDayEpoch（逐时 token 种子的本地零点锚）
#include "quota_home_bridge.h"  // devdesk::quota_bridge::setQuotaFeatureEnabled
#include "quota/quota_store.h"  // 余量卡种子走真落库口（编解码单点）
#include "platform/json.h"
#include <fstream>
#include <sstream>
#include <vector>
#include "core/theme.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
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

// 统计卡 id 表（与 THS-08 元数据表 statCardElementId 同值的五个）
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

// --quota-fixture <path>：用 quota_live_probe --dump 落的真机数据种子。
// 默认走离线假种子：门禁 ctest -L probe 跑的是那一条，纯离线、可复现、不打供应商接口。
// 给了开关就把「真网络 → 真 QuotaStore 落库 → 真中立桥 → 真卡片绘制 → 真落图」
// 这条链一次跑通。
//
// 落库仍走真 QuotaStore::applyFetchResult：fixture 只提供「这一次 fetch 返回了什么」，
// 之后 encode/decode、失败韧性、五态判据、标签/裁剪全部是产品代码。若落库/解码
// 绕过了产品代码，这张图证明不了任何事。
//
// fixture 里没有凭据（探针侧就不导出 apiKey / AK / SK），这里也不需要——种子只落
// config + result 两行，凭据对渲染是无关字段。
//
// 返回值 = 失败原因（空串 = 成功）。失败必须让整个探针红，不许退化成「悄悄改用
// 假种子」——那会让这张图的身份变得不可知。

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
        // 而不是 displayName：后者是供应商中文名，拿它当 profile 名会让卡片
        // 两列显示同一个词，看着像渲染重复。
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

// 组 I(c)：单行省略号（core::TextOverflow::Ellipsis）
//
// 取证路径与 runtime 同源：runtime_render.h 的 prepareText 就是把 Element 的
// 字段逐条喂给 TextPrimitive。所以本组不重画整张卡，而是把 `.error` 那一格的
// 元素字段喂进一个裸 TextPrimitive，量 prepare() 之后的行宽——断的是
// 「产品那一格有没有正确接线 maxWidth / overflow」，而「框架能不能画省略号」是
// EUI-NEO 自己 tests/unit/text_ellipsis.cpp 的职责。
//
// 三条断言，逐条可独立致红：
//   (1) 前提：原文按 Clip 量出来的宽 > 内容列宽。不成立 = 这一格压根没溢出，
//       整组退化成恒真假绿（种子不够长的经典坑）。
//   (2) 接线：产品那格声明 overflow==Ellipsis 且 maxWidth>0 且 !wrap。
//       漏任何一条，EUI-NEO 侧 ellipsis 是静默 no-op（不报错）。
//   (3) 结果：渲染行宽 == measureTextWidth(保留前缀 + "…")，即末尾那个字形就是
//       U+2026（不是「多留一个字」也不是「硬剪」）；且行宽 ≤ maxWidth。
//
// 变异口径（两条都真能改变行为）：摘 `.overflow(Ellipsis)` 或摘
// `.maxWidth(innerW)`，(3) 都红（ellipsis 静默不生效，行宽回到整句宽）。

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
// 接 maxWidth / overflow 完全无关。专用于组 I(c) 前提 (1) 与「有没有被截断」的
// 判断。勿用 styleOfElement 代替：它会把元素上的 maxWidth / overflow 一起带进去，
// 量回来的是已经被省略号截断过的行宽，前提 (1) 会恒假。
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

    // 数据底座（组 D/E/F 前提）：已 migrate 的临时库注册为进程级 appDatabase。
    // null 则 statsError、API 分布全零 → 分布图走空态分支不建图，组 D 结构前提失效。
    // 种子 = 1 条 profile（claude_call_logs FK）+ 今日 2 条**带用量**的调用：
    // 纵轴 = 每小时 output_tokens 合计，output_tokens 为 0 的行合计 0（= 没数据）
    // → 依然走空态、组 D 结构前提失效。两条落在 9 时与 17 时 → 去头去尾后恰
    // 9 根柱（9..17 闭区间）。
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
    // 余量功能默认关，probe 侧显式打开以验证卡片渲染
    devdesk::quota_bridge::setQuotaFeatureEnabled(true);

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    const std::string pageId = "home";
    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    bool passed = true;

    // 余量卡种子（组 G/H 前提）
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
                // i==0 的名字刻意给长（52 字符 ≈ 内在宽 400px > kNameW 168px）：
                // head 行的 .name 是固定宽单行盒且不能 wrap（换行会撑破
                // quotaRowHeight 的高度预算），组 J(c) 用它钉「防溢护栏必须在场」。
                // 前缀 quota-p 不变——组 H(b) 按 LIKE 'quota-p%' 清库。
                const std::string name =
                    (i == 0)
                        ? "quota-p0-claude-pro-max-with-very-long-profile-name"
                        : ("quota-p" + std::to_string(i));
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
                    // i==8 是唯一一条错误行（Transient + 从未成功过 → FailedNever →
                    // showsErrorLine 真）。门禁那条不带 --quota-fixture、只走这批
                    // 假种子，没有它组 I 就是空跑。文案刻意给长：上游 detail 跟在
                    // headline 后面，真机上就是这样，断言要钉「长文案也不越出内容列」。
                    //
                    // 长度必须钉死，不是拍脑袋：整句需达可用宽的 2 倍量级——1.25 倍时
                    // 省略号只吃掉尾巴 ~25%，落图上肉眼分不出「省略号」与「硬剪」；
                    // 2.2 倍时保留 114/232 字形、肉眼一眼可辨，且不因换字号/换窗宽
                    // 就翻回不溢出。真机原文只有 224 字节 ≈ 1049px（不溢出），所以
                    // 「能看见 …」的落图必须另造场景，不能拿真机数据当证据。
                    // 组 I(c) 每次运行都打印实时数字，空跑守卫会报「还差多少 px」。
                    r.kind = QuotaKind::Tiered;
                    r.error = QuotaError::Transient;
                    // errorMessage 是**供应商原文 detail**，不含 headline：
                    // quota_home_view.cpp 会自己拼 `headline + 「：」 + detail`，
                    // 种子再写一遍 headline 就会在卡上出现
                    // 「网络失败，稍后重试：网络失败，稍后重试：…」。
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
                    if (i == 0) {
                        // 组 J(a) 前提：同一 profile 两条窗口、reset 一有一无——pct 列是否固定，
                        // 只有混合形态才有判别力（单一形态的行 pct 天然同位，断它
                        // 恒真假绿）。
                        QuotaWindow w2;
                        w2.key = "weekly";
                        w2.usedPercent = 25.0;
                        w2.resetsAt = "2030-01-01T00:00:00";
                        r.windows.push_back(w2);
                    }
                }
                store.applyFetchResult(pid, r, base);
            }
        }
    }


    core::dsl::Ui* ui = nullptr;
    // 单帧标准单元：pollOnce（输入派发 + 交互）→ compose（recompose 树 + layout），
    // 两者成对，缺一个会让帧序与真机不一致
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

    // 「数据还在」的中途落图（--screenshot-seeded <路径>）
    // 必须早于组 H(b)：那是破坏性收尾断言（清空余量表），退出点那张
    // --screenshot 拍到的是收尾后的空态。想看「余量卡长什么样」只能在此处拍。
    // 未给该开关 = 立刻返回（零开销）。
    probe::captureSeedIfRequested(args, window, runtime, &passed);

    // 组 A：类 1(b) sparkline z（P1）
    // home.stat.tokens.spark（sparkwrap 顶带 y0..16）必须整带位于 label
    // （.label y16..32）之上：不相交 + gapY ≥ 0（邻接语义）。回归成 32px 盒
    // (0,0) → 与 label 相交 → 红。
    // 不断言曲线段：linechart.h 空闲态 {0.0f} 零长度段被跳过，且平线贴 bottomY
    // 时胶囊半宽会把段 frame 探进 label 带。
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

    // 组 B：类 2(a) 空转重绘（P1）
    // 稳定 15 帧（初始动画/hover 收敛）后连跑 60 帧 pollOnce（不注入、不
    // recompose），计数 update() 返回 true（= paintRequested_）的帧数。
    // 空闲态 tokenLive false → 无 .timer，期望 ≤1（首帧遗留）。回归成常驻
    // onTimer → 60 帧内 ≥2 → 红。
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

    // 组 C：类 2(b) hover token（P1）
    // hover != surface：深色档（默认 current()）天然不等，浅色档需显式验。
    // 双档各验一次，验完还原深色（后续段几何不受主题影响，保持确定性）。
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

    // 组 D/E 前：API 分布卡当前帧
    composeFrame();

    // 组 D：类 4(a) barChart 几何（P1）
    // plot 区总高 = grid.3.y - grid.0.y（barchart.h 四条横线 y = plotY + k·H/3
    // → grid.0 在 plotY、grid.3 在 bottomY）；不量 bar 高度，因为 bar 有 0.16s
    // 帧过渡。种子的非零用量 → 非空态 → 去头去尾后 9 根柱（9..17）正常建图。
    // 卡高 260 → wrap 196 → plot 84 ≥ 36；回归成卡高 208 → plot 16 < 36 → 红。
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

    // 组 E：类 5(a) API 卡对称（P3）
    // title/wrap 相对卡 content（inset 16）以 (0,·) 定位 → 左右间隙各 16，
    // |左 - 右| ≤ 2px。content 已含 inset，再叠一层 position(16,..) → 左 32 右 0。
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

    // 组 F：类 6(a) stat 溢出（P1）
    // 注入：测试时钟（每次 nowMs() +1000ms）+ 单流累计 20M→40M → 2s 窗口
    // delta 20M → rateTps=10M → 「10000000」（8 位）。数字自 UAT2 起为 22px
    // 固定字号（fontSizeLarge+4，见 drawStatCard；旧的 48px+动态收缩已删）：
    // 8 位内在宽 ≈106 < cw 176 天然收纳，无溢出。本组防的是回归——字号再涨
    // 或盒再窄 → 下面两段式判别兜底变红。
    // badge = 流式档「● 1 路 · 累计 40M」，相对当前元素 frame 度量，是弱判别；
    // 数字（8 位）才是强判别。
    // 判别两段式：clip/wrap true → 过；否则 measureTextSize 量内在宽比盒宽。
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
        const int kMinWindowW = 1000;  // 设计最小窗宽（UAT3 D-R3-2，main.cpp kMinWindowLogical）：低于此宽的文本溢出归因窗口过小
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

    // 组 G：余量卡几何（P1）
    // 四条，都用**数字**判（不接受「看起来对」）：
    //   G(a) 余量卡横向不溢出**内容列**。判窗口是判错对象：整页挂进 home.scroll，
    //       出滚动条时内容列比窗口窄。
    //   G(b) 上方统计网格的 x/y/h 逐字不变，且宽 == 内容列宽 − pad×2
    //   G(c) 余量卡在统计网格**之下**；快捷按钮行**没被挤出滚动内容盒**。
    //       整页挂在 home.scroll 上后，页脚允许在视口之外（那正是滚动存在的
    //       意义），但必须仍在内容盒内 = 用户滚得到。判「塞得进视口」会把
    //       「矮窗看不到余量卡」编码成正确行为。
    //   G(d) 余量卡在任意屏高都构建（矮窗不再两面落空）
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
            // 期望值来自 home_module.cpp 的既有常量（pad 40 / header 72 / gap 20 /
            // statH 144×fontScale），不从当前帧反推（反推等于「测了它自己」）。
            // 宽度不写死成「屏宽 − 80」：出滚动条时内容列按框架口径收窄
            // （scrollbarWidth + gap），那是正确行为。判「== 内容列宽 − pad×2」
            // 才是这条断言想守的不变式——统计网格必须铺满内容列，不多不少。
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
            // (c) 快捷按钮行仍在**滚动内容盒内**（用户滚得到）。整页挂上
            // home.scroll 之后页脚允许在视口之外，但绝不能溢出内容盒——
            // 溢出去等于滚到底也够不着 = 真丢内容。
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
            // 本条只在探针实际跑的那个屏高上有判别力——默认 1200×1000 下卡
            // 本来就画。门禁里另有 home_probes_min_window（--size 1200x600）
            // 跑更矮的窗，预算纯函数那条另有单测（test_home_quota_card 的
            // home_quota_budget_*），三处合起来才是完整守卫。
            if (card->frame.height <= 0.0f || card->frame.width <= 0.0f) {
                std::cerr << "home_probes: 组 G(d) FAIL——余量卡已构建但零尺寸："
                          << card->frame.width << "x" << card->frame.height
                          << "（屏高 " << args.windowH << "）\n";
                passed = false;
            }
        }
    }

    // 组 J：余量卡三症状几何
    // 三症状各自钉一条几何判据（全部数字判，不接受「看起来对」）：
    //
    // J(a) 布局错位：窗口行的 pct 列 x 必须固定。drawWindowRow 若按有没有 reset
    //     各算一套 gaps/barW（`hasReset ? 3·gap : 2·gap`），无 reset 的行 pct
    //     会右移 116px（five_hour.pct x=1068 vs weekly/monthly.pct x=952）。
    //     判据：同一 profile 的全部 .bar.*.pct 元素 .frame.x 逐个相等（0.5px 容差）。
    //     种子前提：quota-p0 两条窗口、reset 一有一无（单一 reset 形态的行 pct
    //     天然同位，断它 = 恒真）；空跑守卫：混合形态行数 < 1 即红。
    // J(b) 按钮位置没对齐（两处，各自独立致红）：
    //     (1)「去配置」按钮高 26 装在 24 的 head 行里（kHeadH=24、按钮
    //         .size(cfgW, 26)）→ alignItems CENTER 后上下各溢出 head frame 1px。
    //         判据：按钮 frame 必须完全落在 head frame 之内（0.5px 容差）。
    //     (2) 卡头行（home.quota.head）若没有 .gap()，而 title 宽按 cw−104
    //         （= 按钮 96 + 间距 8）预留 → title 与按钮之间 0 间距、[刷新] 右缘
    //         恒比 head 右缘短 8px（实测 1136 vs 1144，按钮不贴右）。
    //         判据：refresh 右缘 == head 右缘（0.5px 容差）。
    // J(c) 文本重叠：head 行 .name/.provider 是固定宽单行盒——长 profile 名的
    //     字形会直接画出 168px 盒、压进 provider 列（上游条目名长度不受控）。
    //     判据（组 F 同款两段式）：clip/wrap 任一在场 → 过；否则按元素真实样式
    //     量内在宽 ≤ 盒宽 + 1px。种子前提：quota-p0 名字刻意给长（52 字符，
    //     无护栏时内在宽 ≈ 400px > 168px 必红）；空跑守卫：没有一格真溢出 = 红。
    {
        composeFrame();
        const auto rows = module::claude::quota::listQuotaHomeRows(env.db->db());
        // 查询失败时不得直接 `.value()`——result.h 契约下那是 LOG_FATAL+abort，
        // 探针会以崩溃收场而不是干净 FAIL（后面的组全不跑、诊断全丢）。
        // 先查 isFail，败 → FAIL + 空列表（本组判据全跳过，但不恒绿：
        // passed 已置 false）。
        if (rows.isFail()) {
            std::cerr << "home_probes: 组 J FAIL——listQuotaHomeRows 失败: "
                      << rows.errorMessage() << "（组 J 判据本轮全部跳过）\n";
            passed = false;
        }
        const std::vector<module::claude::quota::QuotaHomeRow> rowList =
            rows.isOk() ? rows.value()
                        : std::vector<module::claude::quota::QuotaHomeRow>{};

        // J(a) 同一 profile 的 pct 列 x 固定
        int mixedResetRows = 0;
        for (const auto& row : rowList) {
            const std::string rid =
                "home.quota.row." + std::to_string(row.profileId);
            // 收集本行全部 pct 元素；同时判定 reset 是否一有一无（判别力前提）
            std::vector<const core::dsl::Element*> pcts;
            bool anyReset = false;
            bool anyNoReset = false;
            for (const auto& w : row.windows) {
                const core::dsl::Element* pct =
                    ui != nullptr
                        ? ui->find(rid + ".bar." + w.key + ".pct")
                        : nullptr;
                if (pct == nullptr) continue;   // 无条状态（不画窗口行）不算
                pcts.push_back(pct);
                const core::dsl::Element* reset =
                    ui->find(rid + ".bar." + w.key + ".reset");
                if (reset != nullptr) {
                    anyReset = true;
                } else {
                    anyNoReset = true;
                }
            }
            if (pcts.size() < 2 || !(anyReset && anyNoReset)) continue;
            ++mixedResetRows;
            for (size_t k = 1; k < pcts.size(); ++k) {
                if (std::fabs(pcts[k]->frame.x - pcts[0]->frame.x) > 0.5f) {
                    std::cerr << "home_probes: 组 J(a) FAIL——" << rid
                              << " 的 pct 列不齐：pct[0].x=" << pcts[0]->frame.x
                              << " vs pct[" << k << "].x=" << pcts[k]->frame.x
                              << "（差 " << (pcts[k]->frame.x - pcts[0]->frame.x)
                              << "px；无 reset 的行把 pct 顶进 reset 列？）\n";
                    passed = false;
                }
            }
        }
        if (mixedResetRows < 1) {
            std::cerr << "home_probes: 组 J(a) FAIL——没有任何一行窗口 reset "
                         "一有一无（mixedResetRows=" << mixedResetRows
                      << "）：本组空跑成恒真假绿，种子需要混合 reset 形态\n";
            passed = false;
        }

        // J(b)(1)「去配置」按钮 frame 必须落在 head frame 之内
        int configBtnRows = 0;
        for (const auto& row : rowList) {
            const std::string rid =
                "home.quota.row." + std::to_string(row.profileId);
            const core::dsl::Element* head =
                ui != nullptr ? ui->find(rid + ".head") : nullptr;
            const core::dsl::Element* btn =
                ui != nullptr ? ui->find(rid + ".config") : nullptr;
            if (btn == nullptr) continue;   // 非 Unconfigured/Incomplete 行无按钮
            ++configBtnRows;
            if (head == nullptr) {
                std::cerr << "home_probes: 组 J(b) FAIL——" << rid
                          << " 有 .config 但 .head 缺失\n";
                passed = false;
                continue;
            }
            if (btn->frame.y < head->frame.y - 0.5f ||
                btn->frame.y + btn->frame.height >
                    head->frame.y + head->frame.height + 0.5f ||
                btn->frame.x < head->frame.x - 0.5f ||
                btn->frame.x + btn->frame.width >
                    head->frame.x + head->frame.width + 0.5f) {
                std::cerr << "home_probes: 组 J(b) FAIL——" << rid
                          << ".config frame (" << btn->frame.x << ","
                          << btn->frame.y << " " << btn->frame.width << "x"
                          << btn->frame.height << ") 溢出 head frame ("
                          << head->frame.x << "," << head->frame.y << " "
                          << head->frame.width << "x" << head->frame.height
                          << ")（按钮比 head 行高？）\n";
                passed = false;
            }
        }
        if (configBtnRows < 1) {
            std::cerr << "home_probes: 组 J(b) FAIL——没有任何一行有「去配置」"
                         "按钮（configBtnRows=" << configBtnRows
                      << "）：本组空跑成恒真假绿\n";
            passed = false;
        }

        // J(b)(2) 卡头 [刷新] 按钮右缘必须贴内容列右缘
        {
            const core::dsl::Element* head =
                ui != nullptr ? ui->find("home.quota.head") : nullptr;
            const core::dsl::Element* refresh =
                ui != nullptr ? ui->find("home.quota.refresh") : nullptr;
            if (head == nullptr || refresh == nullptr) {
                std::cerr << "home_probes: 组 J(b) FAIL——卡头结构缺失 (head="
                          << (head != nullptr ? "ok" : "null") << ", refresh="
                          << (refresh != nullptr ? "ok" : "null") << ")\n";
                passed = false;
            } else {
                const float headRight = head->frame.x + head->frame.width;
                const float refreshRight =
                    refresh->frame.x + refresh->frame.width;
                if (std::fabs(headRight - refreshRight) > 0.5f) {
                    std::cerr << "home_probes: 组 J(b) FAIL——[刷新] 右缘 "
                              << refreshRight << " != head 右缘 " << headRight
                              << "（差 " << (headRight - refreshRight)
                              << "px：head 行缺 .gap()，按钮不贴右）\n";
                    passed = false;
                }
            }
        }

        // J(c) head 行 name/provider 防溢护栏（clip/wrap）在场
        int nameCellsChecked = 0;
        for (const auto& row : rowList) {
            const std::string rid =
                "home.quota.row." + std::to_string(row.profileId);
            for (const char* suffix : {".name", ".provider"}) {
                const core::dsl::Element* el =
                    ui != nullptr ? ui->find(rid + suffix) : nullptr;
                if (el == nullptr) continue;
                ++nameCellsChecked;
                if (textFitsOrGuarded(*el)) continue;
                std::cerr << "home_probes: 组 J(c) FAIL——" << el->id
                          << " 无 clip/wrap 且文案宽超出盒宽（文案「"
                          << el->text << "」盒宽 " << el->frame.width
                          << "px：长名字的字形会压进相邻列 = 文本重叠）\n";
                passed = false;
            }
        }
        if (nameCellsChecked < 1) {
            std::cerr << "home_probes: 组 J(c) FAIL——一格 .name/.provider 都没"
                         "读到（" << nameCellsChecked
                      << "）：本轮空转 = 没有守卫\n";
            passed = false;
        }
        // 空跑守卫：种子必须真有一格长名字（无护栏时内在宽 > 盒宽），否则上面的
        // 宽度比较对每一格都恒过 = 恒真假绿。rawWidthOfElement 强制
        // Clip+maxWidth=0，量的就是「没护栏时会不会溢出」，与本格是否已接 clip
        // 无关。--quota-fixture（真机）模式下本守卫会红：真 profile 名恰好都短
        // （没有一格真溢出），这与组 I(c) 在 fixture 模式红是同一口径（真机数据
        // 不含该场景 → 诚实呈现，不静默绿）；门禁那条走离线假种子，长名字由种子保证。
        int longNameCells = 0;
        for (const auto& row : rowList) {
            const std::string rid =
                "home.quota.row." + std::to_string(row.profileId);
            const core::dsl::Element* nameEl =
                ui != nullptr ? ui->find(rid + ".name") : nullptr;
            if (nameEl == nullptr) continue;
            if (rawWidthOfElement(*nameEl) > nameEl->frame.width + 1.0f) {
                ++longNameCells;
            }
        }
        if (longNameCells < 1) {
            std::cerr << "home_probes: 组 J(c) FAIL——没有任何一格名字超出盒宽"
                         "（longNameCells=" << longNameCells
                      << "）：种子名字不够长，本组空跑成恒真假绿\n";
            passed = false;
        }

        // J(d) 长名/长供应商名以「…」收尾（Ellipsis 接线）
        // J(c) 钉「护栏在场」，J(d) 更进一步：真溢出的那格必须是**单行省略号**
        // 而不是硬剪——clip 只防重叠，长名被静默剪掉、用户看不出「后面还有字」。
        // 判据与组 I(c) 同款三段式，只对**真溢出**的格子生效（rawWidth > 盒宽，
        // rawWidthOfElement 强制 Clip+maxWidth=0 量「没护栏时的宽」）：
        //   (1) 接线：overflow==Ellipsis && maxWidth>0 && !wrap——漏任何一条
        //       EUI-NEO 侧是静默 no-op；
        //   (2) 结果：渲染行宽逐位 == 「保留前缀 + …」的宽（末尾字形就是
        //       U+2026），且 ≤ maxWidth；
        //   (3) 同源：渲染路径与 DSL 测量路径（measureTextSize）同值。
        // 空跑守卫：ellipsisCells >= 1（离线种子 quota-p0 的 52 字符名保证）。
        int ellipsisCells = 0;
        for (const auto& row : rowList) {
            const std::string rid =
                "home.quota.row." + std::to_string(row.profileId);
            for (const char* suffix : {".name", ".provider"}) {
                const core::dsl::Element* el =
                    ui != nullptr ? ui->find(rid + suffix) : nullptr;
                if (el == nullptr) continue;
                if (rawWidthOfElement(*el) <= el->frame.width + 0.5f) {
                    continue;   // 没溢出的格子本组无话可说（J(c) 已管护栏）
                }
                ++ellipsisCells;
                if (el->wrap) {
                    std::cerr << "home_probes: 组 J(d) FAIL——" << el->id
                              << " wrap==true：省略号是单行语义，wrap 下是 no-op\n";
                    passed = false;
                } else if (el->overflow != core::TextOverflow::Ellipsis) {
                    std::cerr << "home_probes: 组 J(d) FAIL——" << el->id
                              << " 溢出但 overflow != Ellipsis（当前 "
                              << (el->overflow == core::TextOverflow::Clip
                                      ? "Clip"
                                      : "?")
                              << "）：长名被硬剪，用户看不出还有内容\n";
                    passed = false;
                } else if (el->maxWidth <= 0.0f) {
                    std::cerr << "home_probes: 组 J(d) FAIL——" << el->id
                              << " maxWidth==0：ellipsis 静默不生效（runtime 回退"
                                 " frame.width，DSL 布局按 0 走，两条路分叉）\n";
                    passed = false;
                } else {
                    const EllipsisExpectation expect = expectedEllipsis(*el);
                    const float rendered = renderedLineWidth(*el);
                    std::cout << "home_probes: 组 J(d) " << el->id << " 原文宽 "
                              << rawWidthOfElement(*el) << " > 盒宽 "
                              << el->frame.width << " → 保留 " << expect.keep
                              << "/" << expect.glyphCount << " 字形，行宽 "
                              << rendered << " ≤ maxWidth " << el->maxWidth
                              << "，末尾为 U+2026\n";
                    if (std::fabs(rendered - expect.expectedWidth) > 0.01f) {
                        std::cerr << "home_probes: 组 J(d) FAIL——" << el->id
                                  << " 渲染行宽 " << rendered << " != 「前缀 + …」"
                                  << "的宽 " << expect.expectedWidth
                                  << "：末尾不是省略号\n";
                        passed = false;
                    }
                    if (rendered > el->maxWidth + 0.01f) {
                        std::cerr << "home_probes: 组 J(d) FAIL——" << el->id
                                  << " 行宽 " << rendered << " > maxWidth "
                                  << el->maxWidth << "：仍会画出本格\n";
                        passed = false;
                    }
                    if (expect.keep >= expect.glyphCount) {
                        std::cerr << "home_probes: 组 J(d) FAIL——" << el->id
                                  << " 一字未截（保留 " << expect.keep << "/"
                                  << expect.glyphCount << "）却判溢出：前提不符\n";
                        passed = false;
                    }
                    const float measured =
                        core::TextPrimitive::measureTextSize(styleOfElement(*el)).x;
                    if (std::fabs(measured - rendered) > 0.01f) {
                        std::cerr << "home_probes: 组 J(d) FAIL——" << el->id
                                  << " 渲染行宽 " << rendered
                                  << " != DSL 测量宽 " << measured
                                  << "：两条路径分叉\n";
                        passed = false;
                    }
                }
            }
        }
        if (ellipsisCells < 1) {
            std::cerr << "home_probes: 组 J(d) FAIL——没有任何一格 .name/.provider"
                         " 真溢出（ellipsisCells=" << ellipsisCells
                      << "）：本组空跑成恒真假绿，种子需要长名/长供应商名\n";
            passed = false;
        }
    }

    // 组 I：错误行只出现一次 + 不溢出
    // 错误文案会被渲染两遍：① head row 的 `.status` 格（statusTextFor 在
    // showsErrorLine 时返回 row.lastError）② 专属错误行 `.error`。且 `.status`
    // 是既无 clip 又无 wrap 的单行盒（maxWidth 只在 wrap=true 时生效，见
    // EUI-NEO text.cpp），第一遍直接溢出卡片右缘被截断。
    //
    // I(a) **只出现一次**：错误文案在整棵余量树里有且仅有一处携带它——钉
    //     「字符串在树里出现几次」而不是「某个格是不是空」，后者换个别的文案
    //     渲染两遍就又绿了。
    // I(b) **不溢出内容列**：错误文案所在格的右缘 ≤ 内容列右缘。钉的是宽度
    //     比较（本格右缘 vs 父行内容宽）而不是某个字面量宽度，换字号 / 换文案
    //     长度 / 换窗口宽都仍成立。
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
        // 查询失败时不得直接 `.value()`（同组 J）：否则 LOG_FATAL+abort，探针以崩溃
        // 收场，后面的组全不跑、诊断全丢。先查 isFail，败 → FAIL + 空列表。
        if (rows.isFail()) {
            std::cerr << "home_probes: 组 I FAIL——listQuotaHomeRows 失败: "
                      << rows.errorMessage() << "（组 I 判据本轮全部跳过）\n";
            passed = false;
        }
        const std::vector<module::claude::quota::QuotaHomeRow> rowList =
            rows.isOk() ? rows.value()
                        : std::vector<module::claude::quota::QuotaHomeRow>{};
        // 错误行必须真的存在，否则本组是空跑（恒真假绿）。门禁那条不带
        // --quota-fixture，靠上面的 i==8 假种子保证有错误行。
        int errorRows = 0;
        for (const auto& r : rowList) {
            const bool isErr = r.state == module::claude::quota::QuotaCardState::Incomplete ||
                               r.state == module::claude::quota::QuotaCardState::FailedNever ||
                               r.state == module::claude::quota::QuotaCardState::FailedStale;
            if (isErr && !r.lastError.empty()) ++errorRows;
        }
        if (rows.isOk() && errorRows < 1) {
            std::cerr << "home_probes: 组 I FAIL——没有错误行可断言（错误行 "
                      << errorRows << " 条）：本组会空跑成恒真假绿\n";
            passed = false;
        }
        for (const auto& row : rowList) {
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

            // I(a) 错误文案在整棵余量树里只出现一次
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

            // I(b) 不溢出内容列：本格右缘 ≤ 内容列右缘
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
            // （overflow==Ellipsis && maxWidth>0 && !wrap）。量的必须是**元素
            // 真实声明的样式**（含 maxWidth/overflow）：量「没接线时的文本宽」
            // 会让省略号那一格误判成溢出（假红），而没接 maxWidth 的格子又会
            // 因为漏量而被放过（假绿）。
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

            // I(c) 超长错误文案以「…」收尾（不是被硬剪）
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
        // I(c) **`.status` 的防溢护栏必须在场**。
        //
        // 错误行的 .status 文本恒为空串 → measureTextSize("") = 0 → 在错误行上
        // 查 clip/wrap 对 .status 是恒真的空转（删掉 .status 的 .clip() 全绿）。
        // 所以判据是「护栏在不在」而不是「文本有没有超出盒宽」：当前三种非错误
        // 状态的标签（未配置余量查询 / 配置不完整，去补全 / 等待首次查询）在任何
        // 窗口宽下都装得进 statusW，宽度判据恒不触发。
        // 「.status 是固定高单行盒 + 上游文案长度不可控 → 必须有 clip 或 wrap」
        // 是 home_quota_card.cpp .status 段里明写的不变式（「maxWidth 只在
        // wrap=true 时生效……故防溢只能靠 clip 剪裁」），断言这条不变式本身。
        int statusCheckedRows = 0;
        for (const auto& row : rowList) {
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
        // 过滤器匹配 0 条时静默绿是本项目反复踩的坑。
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

    // 中途落图（--screenshot-seeded <路径>）
    // 此刻余量种子全在场（quota-p0 的 52 字符长名 + i==8 错误行都画在卡上），
    // 下一组 H(b) 马上清库——「长名省略号」的取证图只能在这个状态点拍。
    // 不传开关时 captureSeedIfRequested 立即返回（零开销，probe_harness.h 契约）。
    probe::captureSeedIfRequested(args, window, runtime, &passed);

    // 组 H：余量卡滚动（P1）
    // H(a) 滚动条**由内容溢出驱动**：出滚动条 ⟺ 内容高 > 视口高。「出现」与
    //     「不溢出时不出现」两半都要断，否则 --quota-fixture 模式（真机数据
    //     只有 4~5 行、内容未必溢出）与「窗口更高 → 视口更大 → 内容装得下」
    //     两种情形下整条会失去判别力。
    //     判据用 computeQuotaCardMetrics（home_quota_card.h 的公开契约函数），
    //     cap 取实测视口高：overflows 恰好等于「内容超出这个视口」。
    //     bar 为 null 不直接判红——那可能正是「不溢出」的正确答案；红不红由
    //     overflows 与 bar 存在性是否一致决定。
    // H(b) 删掉全部余量数据后重排 → 滚动条消失（证明它是内容驱动的，不是
    //     「无条件画一根」的装饰）
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
        // 只删余量两表是不够的：剩下的 profile 全是「未配置」行，9 行 ×
        // (24 头 + 10 间距) = 296 > 250 视口，本来就该出滚动条——那是正确行为。
        // 假种子按名字前缀删（quota-p%），真机种子按 id 删（--quota-fixture 的
        // profile 名就是供应商名，前缀删不掉——漏删会让本断言在真机模式下测的
        // 根本不是「只剩一行」）。
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

    // 组 K：矮窗多档——余量卡在真实最小窗各档都构建
    // 补两处判别力空缺：组 G(d) 只在探针**实际运行**的那档屏高有判别力；
    // home_probes_min_window 只跑 1200×600，而真实设计最小窗是 1000×640 逻辑
    // （main.cpp:492-493 kMinWindowLogical*，UAT3 D-R3-2 从 1280×800 收窄）。
    // 收窄后两平台**均不再钳**：macOS 640 ≤ 805 可用、Windows@125% 640×1.25=800
    // 物理 ≤ 973 可用——有效下限 = 设计下限（640；旧 Windows 钳制档 778 降为普通
    // 深压缩尺寸）。
    //
    // 本组用**无头 compose**一跑覆盖四档（同一模块实例在不同 Screen 下各排
    // 一帧）：compose 的 screen 是参数，不必为每档开一条 ctest 条目重跑全组。
    // 四档 = 预算的工作档：
    //   · 845 压缩刀口档（合成档，非最小窗——旧设计值恰在刀口，保留专测 1px
    //     压缩路径）：quotaBody = 845−fixed−kHomeChartH = 95 < 96，靠压缩
    //     图表 1px（260→259）恰好画出卡；
    //   · 778 深压缩档（旧 Windows@125% 有效下限；778 > 750 走压缩路径，
    //     图表压到 ~192。现高于新设计最小窗 640，作为普通深压缩尺寸保留覆盖）；
    //   · 640 设计最小窗 = 两平台有效下限（滚动模式档：< fixed+kHomeChartH）：
    //     整页出滚动条、图表一分不压、卡照画；
    //   · 600 守护性深档（< 640，低于一切真实下限）：滚动模式，钉「低于下限
    //     的窗口也不丢内容不崩」。
    // 判据（每档一致）：
    //   K(a) 余量卡节点存在、非零尺寸、且不压统计网格（矮窗整张不画 = 欠账复发）
    //   K(b) 整页滚动容器在场（矮窗可见性依赖它吸收溢出；G 同款 content 节点）
    //   K(c) 既有内容一条不少（stats / chart / actions / root）
    //   K(d) 快捷按钮行仍在滚动内容盒内（滚到底够得着 = 没丢内容）
    //   K(e) 图表卡高度 == computeHomeQuotaBudget 的结论，且预算说这档画卡
    //        （home_module 把 budget.chartHeight 传进图表卡；预算与渲染分叉
    //        时这里红，常量漂移时 home_quota_budget_* 单测先红）
    // 数据前提：本组在组 H(b)（清库）之后跑，库里只剩未配置行——卡画不画只由
    // 屏高预算决定（空态/未配置也画卡），与行数无关，无需补种子。
    {
        struct Tier {
            float height;
            const char* label;
        };
        const Tier tiers[] = {
            {845.0f, "845 压缩刀口档（合成：quotaBody=95<96 压 1px 恰好画卡；现行设计最小窗 1000×640）"},
            {778.0f, "1000×778 深压缩档（旧 Windows@125% 有效下限；现为普通深压缩尺寸）"},
            {640.0f, "1000×640 设计最小窗（main.cpp:492-493；两平台均不钳 = 有效下限，滚动模式档）"},
            {600.0f, "1000×600 守护性深档（< 640 设计下限，滚动模式不丢内容）"},
        };
        // 预算输入与 home_module compose 同源（不另写一套魔法数）：statH =
        // 144×fontScale，pad/gap 取主题 token（组 G(b) 同款口径）。
        const auto& kTheme = core::theme::current();
        const float kStatH = 144.0f * core::theme::fontScale();
        const float kPad = kTheme.sizes.spacingXXLarge;
        const float kGap = kTheme.sizes.spacingLarge * 1.25f;
        for (const Tier& tier : tiers) {
            eui::Screen tierScreen;
            tierScreen.width = 1000.0f;
            tierScreen.height = tier.height;
            eui::Ui tierUi;
            tierUi.begin("home_probe_k_" +
                         std::to_string(static_cast<int>(tier.height)));
            mod->compose(tierUi, tierScreen);
            tierUi.end();
            // 必须 layout：compose 只建树不排版，不 layout 所有 frame 都是 0，
            // 后面那些「>= 36 / 零尺寸」判据会全落进「0 <= 0.5」的恒真假绿。
            tierUi.layout(tierScreen.width, tierScreen.height);

            const core::dsl::Element* card = tierUi.find("home.quota");
            const core::dsl::Element* pageScroll =
                tierUi.find("home.scroll.content");
            const core::dsl::Element* stats = tierUi.find("home.stats");
            const core::dsl::Element* chart = tierUi.find("home.api.chart");
            const core::dsl::Element* acts = tierUi.find("home.actions");
            const core::dsl::Element* root = tierUi.find("home.root");
            const char* ok = "ok";
            const char* no = "null";
            if (pageScroll == nullptr || stats == nullptr || chart == nullptr ||
                acts == nullptr || root == nullptr) {
                std::cerr << "home_probes: 组 K(b/c) FAIL（" << tier.label
                          << "）——结构缺失 (scrollContent="
                          << (pageScroll != nullptr ? ok : no)
                          << ", stats=" << (stats != nullptr ? ok : no)
                          << ", chart=" << (chart != nullptr ? ok : no)
                          << ", acts=" << (acts != nullptr ? ok : no)
                          << ", root=" << (root != nullptr ? ok : no) << ")\n";
                passed = false;
                continue;
            }
            // K(a) 判据本体：卡必须画出来（矮窗整张不画 = 欠账复发）
            if (card == nullptr) {
                std::cerr << "home_probes: 组 K(a) FAIL（" << tier.label
                          << "）——余量卡整张未构建：矮窗不可见"
                          << "\n";
                passed = false;
            } else {
                if (card->frame.width <= 0.0f || card->frame.height <= 0.0f) {
                    std::cerr << "home_probes: 组 K(a) FAIL（" << tier.label
                              << "）——余量卡已构建但零尺寸 "
                              << card->frame.width << "x" << card->frame.height
                              << "\n";
                    passed = false;
                }
                if (card->frame.y <
                    stats->frame.y + stats->frame.height - 0.5f) {
                    std::cerr << "home_probes: 组 K(a) FAIL（" << tier.label
                              << "）——余量卡压住统计网格：card.y="
                              << card->frame.y << " stats 底="
                              << (stats->frame.y + stats->frame.height) << "\n";
                    passed = false;
                }
            }
            // K(d) 快捷按钮行仍在滚动内容盒内（滚到底够得着 = 没丢内容）
            if (acts->frame.y + acts->frame.height >
                root->frame.y + root->frame.height + 0.5f) {
                std::cerr << "home_probes: 组 K(d) FAIL（" << tier.label
                          << "）——快捷按钮行溢出滚动内容盒：底="
                          << (acts->frame.y + acts->frame.height) << " > 内容底="
                          << (root->frame.y + root->frame.height) << "\n";
                passed = false;
            }
            // K(e) 图表卡高度 == 预算结论（渲染与预算分叉 = 两条口径漂移）
            const module::home::quota_card::HomeQuotaBudget budget =
                module::home::quota_card::computeHomeQuotaBudget(
                    tier.height, kStatH, kPad, kGap);
            if (!budget.drawsQuotaCard) {
                std::cerr << "home_probes: 组 K(e) FAIL（" << tier.label
                          << "）——预算函数说这档不画卡（quotaBody="
                          << budget.quotaBodyHeight
                          << "）：预算回退或常量漂移\n";
                passed = false;
            }
            if (std::fabs(chart->frame.height - budget.chartHeight) > 0.5f) {
                std::cerr << "home_probes: 组 K(e) FAIL（" << tier.label
                          << "）——图表卡高 " << chart->frame.height
                          << " != 预算结论 " << budget.chartHeight
                          << "（渲染与预算分叉）\n";
                passed = false;
            }
        }
    }

    // 组 L：[刷新] 与 [去配置] 右缘同线
    // [去配置] 在 home.quota.scroll（scrollView）里——内容溢出时 scrollView 把
    // 内容列缩成 width−(scrollbar+gap)（EUI-NEO scrollview.h 默认 metrics
    // 8+16=24px），行 head 连同 [去配置] 的右缘跟着缩；[刷新] 在 scrollView
    // **外**的卡头行里、不跟缩 → 滚动条一出现 [刷新] 恒比 [去配置] 突出 24px。
    // 卡头行宽度必须 == 滚动内容列宽（inset 取自与 scrollView 同一份
    // componentTokens metrics，两侧同源不漂移）。
    // 注意 J(b)(2) 的判据是「[刷新] 右缘 == **卡头行**右缘」，它不含行 head：
    // 卡头行自身对齐了不等于行 head 也对齐，两条要各断各的。
    // 断言（两档同码，各自独立致红；空跑守卫钉前提）：
    //   L(a) 每个 [去配置] 右缘 == [刷新] 右缘（0.5px 容差）——按钮面对齐。
    //   L(b) 每个行 head 右缘 == 卡头行右缘（0.5px）——结构面（name/status
    //        列同线，不止按钮）。
    //   L(c) 两按钮各自与所在行垂直居中（center.y 相等）且同高（height 相等）
    //        ——两按钮分属 kTitleH=28 / kHeadH=24 两档行高，**跨行** y/高度
    //        全等不是本卡的布局语义（J(b)(1) 已钉「按钮高==行高」）；
    //        「对齐」的语义 = 右缘同线 + 行内各自居中。
    // 档位：非滚动档 = 组 H(b) 收尾态（仅 1 行未配置、无滚动条，两缘本来就
    //       同在 cw——控制组）；滚动档 = 本组补种 5 个双窗 profile
    //       （内容 24+5×72+5×10=434 > 视口 ~250）后断，此档才是错位暴露的那档。
    //       断完清掉补种行再排一帧，退出状态与 H(b) 收尾态一致。
    {
        auto assertTier = [&](const char* tier, bool expectBar) {
            const core::dsl::Element* refresh =
                ui != nullptr ? ui->find("home.quota.refresh") : nullptr;
            const core::dsl::Element* cardHead =
                ui != nullptr ? ui->find("home.quota.head") : nullptr;
            const core::dsl::Element* bar =
                ui != nullptr ? ui->find("home.quota.scroll.scroll") : nullptr;
            if (refresh == nullptr || cardHead == nullptr) {
                std::cerr << "home_probes: 组 L FAIL（" << tier
                          << "）——卡头结构缺失 (refresh="
                          << (refresh != nullptr ? "ok" : "null")
                          << ", head=" << (cardHead != nullptr ? "ok" : "null")
                          << ")\n";
                passed = false;
                return;
            }
            // 空跑守卫：滚动档必须有滚动条（没有 = 补种没溢出，右缘断言会
            // 假绿）；非滚动档必须没有（有 = 档位标签说谎）。
            if ((bar != nullptr) != expectBar) {
                std::cerr << "home_probes: 组 L FAIL（" << tier << "）——滚动条"
                          << (bar != nullptr ? "在" : "不在") << "，与该档预期（"
                          << (expectBar ? "滚动" : "非滚动")
                          << "）不符：前提不成立，右缘断言失真\n";
                passed = false;
                return;
            }
            const float refreshRight = refresh->frame.x + refresh->frame.width;
            const float cardHeadRight =
                cardHead->frame.x + cardHead->frame.width;
            // L(c) 卡头侧：[刷新] 与卡头行垂直居中且同高
            const float refreshCy =
                refresh->frame.y + refresh->frame.height * 0.5f;
            const float cardHeadCy =
                cardHead->frame.y + cardHead->frame.height * 0.5f;
            if (std::fabs(refreshCy - cardHeadCy) > 0.5f ||
                std::fabs(refresh->frame.height - cardHead->frame.height) >
                    0.5f) {
                std::cerr << "home_probes: 组 L(c) FAIL（" << tier
                          << "）——[刷新] (" << refresh->frame.y << "+"
                          << refresh->frame.height << ") 与卡头行 ("
                          << cardHead->frame.y << "+" << cardHead->frame.height
                          << ") 不同心/不同高\n";
                passed = false;
            }
            const auto rowsL =
                module::claude::quota::listQuotaHomeRows(env.db->db());
            if (rowsL.isFail()) {
                std::cerr << "home_probes: 组 L FAIL（" << tier
                          << "）——listQuotaHomeRows 失败: "
                          << rowsL.errorMessage() << "（本档判据全部跳过）\n";
                passed = false;
                return;
            }
            int configBtns = 0;
            int headRows = 0;
            for (const auto& row : rowsL.value()) {
                const std::string rid =
                    "home.quota.row." + std::to_string(row.profileId);
                const core::dsl::Element* rowHead =
                    ui != nullptr ? ui->find(rid + ".head") : nullptr;
                const core::dsl::Element* btn =
                    ui != nullptr ? ui->find(rid + ".config") : nullptr;
                if (rowHead == nullptr) continue;  // 行 head 缺失 = 结构问题，
                                                   // 滚动视口不裁树（非虚拟化）
                ++headRows;
                // L(b) 结构面：行 head 右缘 == 卡头行右缘
                const float rowHeadRight =
                    rowHead->frame.x + rowHead->frame.width;
                if (std::fabs(rowHeadRight - cardHeadRight) > 0.5f) {
                    std::cerr << "home_probes: 组 L(b) FAIL（" << tier
                              << "）——" << rid << ".head 右缘 " << rowHeadRight
                              << " != 卡头行右缘 " << cardHeadRight << "（差 "
                              << (cardHeadRight - rowHeadRight)
                              << "px：滚动缩进没同步到卡头行？）\n";
                    passed = false;
                }
                if (btn == nullptr) continue;
                ++configBtns;
                // L(a) 按钮面：[去配置] 右缘 == [刷新] 右缘
                const float btnRight = btn->frame.x + btn->frame.width;
                if (std::fabs(btnRight - refreshRight) > 0.5f) {
                    std::cerr << "home_probes: 组 L(a) FAIL（" << tier
                              << "）——" << rid << ".config 右缘 " << btnRight
                              << " != [刷新] 右缘 " << refreshRight << "（差 "
                              << (refreshRight - btnRight)
                              << "px：一个前一个后）\n";
                    passed = false;
                }
                // L(c) 行侧：[去配置] 与所在 head 行垂直居中且同高
                const float btnCy = btn->frame.y + btn->frame.height * 0.5f;
                const float headCy =
                    rowHead->frame.y + rowHead->frame.height * 0.5f;
                if (std::fabs(btnCy - headCy) > 0.5f ||
                    std::fabs(btn->frame.height - rowHead->frame.height) >
                        0.5f) {
                    std::cerr << "home_probes: 组 L(c) FAIL（" << tier
                              << "）——" << rid << ".config (" << btn->frame.y
                              << "+" << btn->frame.height << ") 与所在 head 行 ("
                              << rowHead->frame.y << "+" << rowHead->frame.height
                              << ") 不同心/不同高\n";
                    passed = false;
                }
            }
            // 空跑守卫：一档里一个 [去配置] 都没有 = L(a) 没断言面，恒真假绿
            if (configBtns < 1) {
                std::cerr << "home_probes: 组 L(a) FAIL（" << tier
                          << "）——没有任何一行带 [去配置] 按钮（configBtns=0）"
                          << "：本档空转\n";
                passed = false;
            }
            if (headRows < 1) {
                std::cerr << "home_probes: 组 L(b) FAIL（" << tier
                          << "）——一行 head 都没读到（headRows=0）：本档空转\n";
                passed = false;
            }
            std::cout << "home_probes: 组 L（" << tier << "）：[刷新] 右缘 "
                      << refreshRight << "，行 head " << headRows << " 行，"
                      << "[去配置] " << configBtns << " 个\n";
        };

        // 非滚动档：组 H(b) 收尾态（1 行未配置、无滚动条）
        composeFrame();
        composeFrame();
        assertTier("非滚动档", false);

        // 滚动档：补种 5 个双窗 profile → 内容必超视口
        {
            using namespace module::claude::quota;
            QuotaStore store(env.db->db());
            const int64_t now = static_cast<int64_t>(std::time(nullptr));
            for (int i = 0; i < 5; ++i) {
                const std::string name = "quota-L" + std::to_string(i);
                if (!env.db->db()
                         .exec("INSERT INTO claude_profiles (name, base_url, "
                               "api_key_hint, api_key_check, model, created_at, "
                               "updated_at) VALUES ('" +
                                   name +
                                   "', 'https://example.invalid', 'sk-***', "
                                   "'chk', 'm', " +
                                   std::to_string(now) + ", " +
                                   std::to_string(now) + ")")
                         .isOk()) {
                    std::cerr << "home_probes: 组 L 补种 profile 插入失败\n";
                    passed = false;
                    break;
                }
                auto st = env.db->db().prepare("SELECT last_insert_rowid() AS id");
                auto rows = st.isOk()
                                ? st.value().fetchAll()
                                : core::Result<
                                      std::vector<core::data::SqlRow>>::fail(
                                      st.errorCode(), st.errorMessage());
                if (rows.isFail() || rows.value().empty()) {
                    std::cerr << "home_probes: 组 L 取补种 profileId 失败\n";
                    passed = false;
                    break;
                }
                const int64_t pid = rows.value()[0].getInt64("id").value();
                QuotaConfigRow cfg;
                cfg.profileId = pid;
                cfg.providerId = "opencode_go";
                cfg.enabled = true;
                cfg.autoRefresh = true;
                cfg.intervalMinutes = 15;
                store.saveConfig(cfg);
                QuotaResult r;
                r.kind = QuotaKind::Tiered;
                QuotaWindow w1;
                w1.key = "five_hour";
                w1.usedPercent = 30.0;
                w1.resetsAt = "2030-01-01T00:00:00";
                r.windows.push_back(w1);
                QuotaWindow w2;
                w2.key = "weekly";
                w2.usedPercent = 60.0;
                w2.resetsAt = "2030-01-01T00:00:00";
                r.windows.push_back(w2);
                store.applyFetchResult(pid, r, now);
            }
        }
        composeFrame();
        composeFrame();
        assertTier("滚动档", true);

        // 清掉补种行，退出状态回到 H(b) 收尾态（不污染收尾图）
        if (!env.db->db()
                 .exec("DELETE FROM claude_profiles WHERE name LIKE 'quota-L%'")
                 .isOk() ||
            !env.db->db().exec("DELETE FROM profile_quota_config").isOk() ||
            !env.db->db().exec("DELETE FROM profile_quota_result").isOk()) {
            std::cerr << "home_probes: 组 L 清理补种行失败\n";
            passed = false;
        }
        composeFrame();
        composeFrame();
    }

    // 组 M：[刷新] 与 [去配置] 等宽
    // EUI-NEO button 的 .size 是**定宽帧**（无 minWidth / 无内容自适应，
    // button.h 的 width_ 直落 frame），所以两按钮的宽必须显式给成同一个值：
    // 内容文字长度不同（「刷新」带 icon vs 「去配置」三字）不会被自适应拉齐。
    // 两处必须读同一个宽度常量 kQuotaBtnW=96（= 含 icon 的「刷新」自然宽，
    // 「去配置」三字仅 ~39px 也装得下）。home_quota_card.cpp 内分成两处字面量
    // 就是漂移入口——本组就是为钉住它而生。
    // 断言：M(a) [刷新] frame.width == 每个 [去配置] frame.width（0.5px 容差）。
    //        等宽是跨档不变式（按钮宽是常量，不随滚动/行数变），组 L 收尾态
    //        （1 行未配置，必带 [去配置]）单档断言即足。
    // 空跑守卫：[刷新] 缺失 / 一个 [去配置] 都没有 = 本组空转恒真假绿，红。
    {
        const core::dsl::Element* refresh =
            ui != nullptr ? ui->find("home.quota.refresh") : nullptr;
        if (refresh == nullptr) {
            std::cerr << "home_probes: 组 M FAIL——卡头 [刷新] 按钮缺失"
                         "（home.quota.refresh 未找到）\n";
            passed = false;
        }
        const auto rowsM =
            module::claude::quota::listQuotaHomeRows(env.db->db());
        if (rowsM.isFail()) {
            std::cerr << "home_probes: 组 M FAIL——listQuotaHomeRows 失败: "
                      << rowsM.errorMessage() << "（本组判据全部跳过）\n";
            passed = false;
        } else {
            int configBtns = 0;
            for (const auto& row : rowsM.value()) {
                const std::string rid =
                    "home.quota.row." + std::to_string(row.profileId);
                const core::dsl::Element* btn =
                    ui != nullptr ? ui->find(rid + ".config") : nullptr;
                if (btn == nullptr) continue;  // 本行无 [去配置]（状态语义，
                                               // showsConfigButton 决定），非结构缺失
                ++configBtns;
                // M(a) 两按钮等宽
                if (refresh != nullptr &&
                    std::fabs(btn->frame.width - refresh->frame.width) >
                        0.5f) {
                    std::cerr << "home_probes: 组 M(a) FAIL——" << rid
                              << ".config 宽 " << btn->frame.width
                              << " != [刷新] 宽 " << refresh->frame.width
                              << "（差 "
                              << (refresh->frame.width - btn->frame.width)
                              << "px：两按钮宽窄不一）\n";
                    passed = false;
                }
            }
            // 空跑守卫：一个 [去配置] 都没有 = 等宽断言没有断言面
            if (configBtns < 1) {
                std::cerr << "home_probes: 组 M(a) FAIL——没有任何一行带 "
                             "[去配置] 按钮（configBtns=0）：本组空转\n";
                passed = false;
            }
            std::cout << "home_probes: 组 M：[刷新] 宽 "
                      << (refresh != nullptr ? refresh->frame.width : -1.0f)
                      << "，[去配置] " << configBtns << " 个\n";
        }
    }

    // 退出前落一张「当前状态图」（--screenshot <路径>）
    // 通用看图口：跑一次 home_probes 就能拿到此刻首页长什么样。不传该开关时
    // captureIfRequested 立刻返回（零开销 / 不写文件）。
    probe::captureIfRequested(args, window, runtime, &passed);

    mod->onStop();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!passed) std::cerr << "FAIL\n";
    return passed ? 0 : 1;
}