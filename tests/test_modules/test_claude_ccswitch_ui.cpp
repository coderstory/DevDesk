// tests/test_modules/test_claude_ccswitch_ui.cpp
// cc-switch 导入弹窗 UI 呈现层无头单测（UI 测试缺口地图 G3）：
//   ① 未开不渲染（panel opacity 0）+ 点击导入按钮打开
//   ② 打开后骨架节点 id 契约（title/desc/selall/list/row.N/confirm/cancel）
//      + 行勾选翻转（选择交互）+ app_type 过滤（codex 不进列表）+ 映射失败行红字
//   ③ 无内容空态（库里没有 Claude / Claude Desktop 供应商）
//   ④ 读库失败错误态（NotFound 分档）
//   ⑤ 导入链：选择 → 确认 → 落库（listProfiles 可见）+ 结果行 + toast
// 跨文件前提：
//   - 夹具自建临时 sqlite 库经 CCSWITCH_DB_PATH 指路，绝不碰真实 ~/.cc-switch；
//     密钥一律假串；断言不含 "sk-"（弹窗不画密钥是契约）。
//   - dialog 子节点常驻构建树，开合用 panel opacity 判定（test_claude_ui 同口径）。
//   - 导入链走真模块 compose 的确认按钮回调（内部读 ClaudeService::global()），
//     断言到 store 可观测面：listProfiles 行、resultLines 节点、toast 节点。
#include "doctest.h"
#include "test_support.h"

#include "claude_service.h"
#include "core/database.h"
#include "core/module_interface.h"
#include "core/theme.h"

#include <eui/dsl.h>

#include <sqlite3.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

extern "C" ::core::Module* create_claude_module();

namespace {

constexpr const char* kTestKeychainService = "DevDeskTestClaudeModule";
constexpr const char* kDbPrefix = "devdesk_test_claude_ccswitch_ui_";

// RAII：临时库 + 注册全局 ClaudeService（析构解除注册，防悬垂污染后续用例）
struct TestEnv : test_support::TestDb {
    std::unique_ptr<module::claude::ClaudeService> svc;

    TestEnv() : test_support::TestDb(kDbPrefix, "") {
        svc = std::make_unique<module::claude::ClaudeService>(db->db(),
                                                              kTestKeychainService);
        module::claude::ClaudeService::setGlobal(svc.get());
    }

    ~TestEnv() { module::claude::ClaudeService::setGlobal(nullptr); }
};

// 合成一帧（显式 layout，元素 frame 需 layout 后才有值）
void composeFrame(::core::Module& mod, eui::Ui& ui, const char* tag) {
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    ui.begin(tag);
    mod.compose(ui, screen);
    ui.end();
    ui.layout(1600.0f, 1000.0f);
}

// 直调元素 onClick（无头无命中测试；点击路径由 probe / 组件用例覆盖）
bool clickById(eui::Ui& ui, const std::string& id) {
    const auto* node = ui.find(id);
    if (node == nullptr || node->onClick == nullptr) {
        std::cerr << "missing clickable: " << id << std::endl;
        return false;
    }
    node->onClick();
    return true;
}

// cc-switch 真实建表语句（列名逐字对齐，与 test_ccswitch_import.cpp 同源）
const char* const kRealSchema = "CREATE TABLE providers ("
    "  id TEXT NOT NULL,"
    "  app_type TEXT NOT NULL,"
    "  name TEXT NOT NULL,"
    "  settings_config TEXT NOT NULL,"
    "  website_url TEXT,"
    "  category TEXT,"
    "  created_at INTEGER,"
    "  sort_index INTEGER,"
    "  notes TEXT,"
    "  icon TEXT,"
    "  icon_color TEXT,"
    "  meta TEXT NOT NULL DEFAULT '{}',"
    "  is_current BOOLEAN NOT NULL DEFAULT 0,"
    "  in_failover_queue BOOLEAN NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (id, app_type)"
    ");";

// 建夹具库并按序插行（插入序 = 扫描序 = 弹窗行序）
bool createCcSwitchDb(const std::filesystem::path& path,
                      const std::vector<std::string>& inserts) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.string().c_str(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    const auto exec = [&](const std::string& sql) {
        char* err = nullptr;
        const bool ok = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK;
        if (err) sqlite3_free(err);
        return ok;
    };
    bool ok = exec(kRealSchema);
    for (const std::string& insert : inserts) {
        if (ok) ok = exec(insert);
    }
    sqlite3_close(db);
    return ok;
}

// 行插入 SQL（app_type / name / settings_config 三元组）
std::string providerRow(const char* id, const char* appType, const char* name,
                        const std::string& settingsConfig) {
    std::string cfg = settingsConfig;
    std::string escaped;
    escaped.reserve(cfg.size());
    for (const char c : cfg) {
        if (c == '\'') escaped += "''";
        else escaped += c;
    }
    return std::string("INSERT INTO providers (id, app_type, name, settings_config) "
                       "VALUES ('") +
           id + "', '" + appType + "', '" + name + "', '" + escaped + "')";
}

// 夹具 settings_config：好配置（只有 AUTH_TOKEN → bearer；带一个额外 env 键）
const char* const kGoodConfig =
    "{\"env\":{\"ANTHROPIC_BASE_URL\":\"https://bridge.example.com\","
    "\"ANTHROPIC_AUTH_TOKEN\":\"sk-fixture-bridge-0001\","
    "\"ANTHROPIC_MODEL\":\"claude-sonnet-5-5\","
    "\"ANTHROPIC_CUSTOM_HEADERS\":\"x-fixture: 1\"}}";
// 夹具 settings_config：好配置（只有 API_KEY → api_key；名字与 DevDesk 已有行撞名用）
const char* const kClashConfig =
    "{\"env\":{\"ANTHROPIC_BASE_URL\":\"https://clash.example.com\","
    "\"ANTHROPIC_API_KEY\":\"sk-fixture-clash-0002\"}}";

// 弹窗面板节点（开合 opacity 判据的锚点，test_claude_ui 同口径）
constexpr const char* kPanelId = "claude.dialog.ccswitch.panel";

} // namespace

// ① 未开不渲染 + 点击「从 cc-switch 导入」打开
REGISTER_TEST(claude_ccswitch_ui_closed_not_rendered,
              "claude_module: ccswitch-ui ① 未开不渲染（panel opacity 0），点导入按钮后 opacity 1", {
    test_support::setEnvVar("CCSWITCH_DB_PATH", "/nonexistent-g3/cc-switch.db");
    TestEnv env;
    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();

    eui::Ui ui;
    composeFrame(*mod, ui, "cc_ui_closed");
    // 拔掉 drawCcSwitchImportDialog 接线 → 这里 nullptr 即红
    const auto* panel = ui.find(kPanelId);
    DOCTEST_REQUIRE(panel != nullptr);
    DOCTEST_CHECK_EQ(panel->opacity, 0.0f);   // 未开：不渲染

    // 点击列表底部「从 cc-switch 导入」→ openCcSwitchImportDialog
    DOCTEST_REQUIRE(clickById(ui, "claude.profiles.import.ccswitch.bg"));
    eui::Ui ui2;
    composeFrame(*mod, ui2, "cc_ui_closed2");
    const auto* panel2 = ui2.find(kPanelId);
    DOCTEST_REQUIRE(panel2 != nullptr);
    DOCTEST_CHECK_EQ(panel2->opacity, 1.0f);  // 开：渲染

    mod->onStop();
    test_support::setEnvVar("CCSWITCH_DB_PATH", "");
    return true;
})

// ② 打开后骨架节点 id 契约 + 勾选翻转 + app_type 过滤 + 映射失败行
REGISTER_TEST(claude_ccswitch_ui_skeleton_contract,
              "claude_module: ccswitch-ui ② 骨架节点 id 契约：标题/说明/全选/列表/行/按钮 + 勾选翻转", {
    // 夹具：坏配置（非 JSON）→ 深港桥（好）→ 撞名机（好）→ codex（不得进列表）
    test_support::TempDir dir("ccswitch_ui_skeleton");
    DOCTEST_REQUIRE(createCcSwitchDb(dir.file("cc.db"), {
        providerRow("cc-0", "claude", "坏配置", "not-json"),
        providerRow("cc-1", "claude", "深港桥", kGoodConfig),
        providerRow("cc-2", "claude", "撞名机", kClashConfig),
        providerRow("cc-3", "codex", "不该出现", kGoodConfig),
    }));
    test_support::setEnvVar("CCSWITCH_DB_PATH", dir.file("cc.db").string());
    TestEnv env;
    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();

    eui::Ui ui;
    composeFrame(*mod, ui, "cc_ui_skel");
    DOCTEST_REQUIRE(clickById(ui, "claude.profiles.import.ccswitch.bg"));
    eui::Ui ui2;
    composeFrame(*mod, ui2, "cc_ui_skel2");

    DOCTEST_CHECK_EQ(ui2.find(kPanelId)->opacity, 1.0f);
    // 骨架 id 契约（改节点 id 即红）
    const auto* title = ui2.find("claude.dialog.ccswitch.title");
    DOCTEST_REQUIRE(title != nullptr);
    DOCTEST_CHECK_EQ(title->text, std::string("从 CC 导入"));  // UAT5 文案缩短口径
    const auto* desc = ui2.find("claude.dialog.ccswitch.desc");
    DOCTEST_REQUIRE(desc != nullptr);
    DOCTEST_CHECK(desc->text.find("只读读取") != std::string::npos);
    // 密钥不出现在弹窗任何静态文案里（契约：密钥经加密管线，不在弹窗显示）
    DOCTEST_CHECK(ui2.find("claude.dialog.ccswitch.list") != nullptr);

    // 默认全选 → selall 显示「取消全选」，确认键计数 = claude 行数（codex 被滤掉）
    const auto* selall = ui2.find("claude.dialog.ccswitch.selall.text");
    DOCTEST_REQUIRE(selall != nullptr);
    DOCTEST_CHECK_EQ(selall->text, std::string("取消全选"));
    // W4 批2：全选钮改 Ghost 文字钮（透明底绿字）；反向锁死暖橙 surfaceElevated 与白底
    const auto* selallBg = ui2.find("claude.dialog.ccswitch.selall.bg");
    DOCTEST_REQUIRE(selallBg != nullptr);
    const auto& wc = core::theme::current().colors;
    auto sameColor = [](const eui::Color& x, const eui::Color& y) {
        return std::fabs(x.r - y.r) < 0.01f && std::fabs(x.g - y.g) < 0.01f &&
               std::fabs(x.b - y.b) < 0.01f;
    };
    // S2.1 Ghost：默认底透明（a=0）
    DOCTEST_CHECK_LT(selallBg->color.a, 0.01f);            // Ghost 默认底透明
    DOCTEST_CHECK(!sameColor(selallBg->color, wc.surfaceElevated));
    const auto* selallText = ui2.find("claude.dialog.ccswitch.selall.text");
    DOCTEST_REQUIRE(selallText != nullptr);
    DOCTEST_CHECK(sameColor(selallText->color, wc.primary));  // S2.1 Ghost 绿字（单节点 .text）
    const auto* confirm = ui2.find("claude.dialog.ccswitch.confirm.text.normal");
    DOCTEST_REQUIRE(confirm != nullptr);
    DOCTEST_CHECK_EQ(confirm->text, std::string("导入选中(3)"));
    const auto* cancel = ui2.find("claude.dialog.ccswitch.cancel.text.normal");
    DOCTEST_REQUIRE(cancel != nullptr);
    DOCTEST_CHECK_EQ(cancel->text, std::string("关闭"));

    //（W4 批2）候选行卡/勾选指示/文字四档 mint 色锁（默认全选帧）：
    // 选中行底 = primarySoftBg（S2.5 淡绿选中，替代硬编码 #E8F7EE）+ primary 绿边；
    // cb.box primaryPressed 绿圈（radius = D/2 = 10 → 20px 圆）+ 白 fa-check；文字四档。
    // 反向锁死暖橙 surface / legacy primary / legacy error。
    {
        const auto kSelRowBgColor = wc.primarySoftBg;  // S2.5 淡绿选中底（ccswitch source: t.colors.primarySoftBg）
        const auto* selRowBg = ui2.find("claude.dialog.ccswitch.row.1.bg");
        DOCTEST_REQUIRE(selRowBg != nullptr);
        DOCTEST_CHECK(sameColor(selRowBg->color, kSelRowBgColor));
        DOCTEST_CHECK(!sameColor(selRowBg->color, wc.surface));   // 反向：暖米行底
        // S1.2 #1: selected border → wc.primary
        DOCTEST_CHECK(sameColor(selRowBg->border.color, wc.primary));
        const auto* selBox = ui2.find("claude.dialog.ccswitch.row.1.cb.box");
        DOCTEST_REQUIRE(selBox != nullptr);
        // S1: source uses primaryPressed for checkbox fill
        DOCTEST_CHECK(sameColor(selBox->color, wc.primaryPressed));  // 绿圈白勾
        DOCTEST_CHECK(!sameColor(selBox->color, wc.primary));       // 反向：不是 primary（更亮）
        DOCTEST_CHECK_EQ(selBox->radius, 10.0f);  // 20px 圆形复选框（kBoxSize/2），非标准控件圆角
        DOCTEST_CHECK(sameColor(selBox->border.color, wc.primaryPressed));
        const auto* selMark = ui2.find("claude.dialog.ccswitch.row.1.cb.mark");
        DOCTEST_REQUIRE(selMark != nullptr);
        DOCTEST_CHECK(sameColor(selMark->textColor, wc.onPrimary));  // S1.2 #17: onPrimary 不变
        const auto* selName = ui2.find("claude.dialog.ccswitch.row.1.name");
        DOCTEST_REQUIRE(selName != nullptr);
        DOCTEST_CHECK(sameColor(selName->textColor, wc.textPrimary));  // S1.2 #10
        const auto* selUrl = ui2.find("claude.dialog.ccswitch.row.1.url");
        DOCTEST_REQUIRE(selUrl != nullptr);
        DOCTEST_CHECK(sameColor(selUrl->textColor, wc.textTertiary));  // S1.2 #12
        const auto* selModel = ui2.find("claude.dialog.ccswitch.row.1.model");
        DOCTEST_REQUIRE(selModel != nullptr);
        DOCTEST_CHECK(sameColor(selModel->textColor, wc.textSecondary));  // S1.2 #11
        const auto* failSkip = ui2.find("claude.dialog.ccswitch.row.0.skip");
        DOCTEST_REQUIRE(failSkip != nullptr);
        DOCTEST_CHECK(sameColor(failSkip->textColor, wc.error));  // S1.2 #15: error→error
        DOCTEST_CHECK(!sameColor(failSkip->textColor, wc.surface));  // 反向：不是表面色
    }

    // 行渲染：name + app_type · base_url；选中行有勾标；codex 行不存在
    const auto* badName = ui2.find("claude.dialog.ccswitch.row.0.name");
    DOCTEST_REQUIRE(badName != nullptr);
    DOCTEST_CHECK_EQ(badName->text, std::string("坏配置"));
    const auto* badSkip = ui2.find("claude.dialog.ccswitch.row.0.skip");
    DOCTEST_REQUIRE(badSkip != nullptr);   // 映射失败行画红字 skipReason
    DOCTEST_CHECK(badSkip->text.find("跳过：") != std::string::npos);
    DOCTEST_CHECK(badSkip->text.find("sk-") == std::string::npos);  // 密钥不进文案
    const auto* okUrl = ui2.find("claude.dialog.ccswitch.row.1.url");
    DOCTEST_REQUIRE(okUrl != nullptr);
    DOCTEST_CHECK(okUrl->text.find("claude · https://bridge.example.com") !=
                  std::string::npos);
    DOCTEST_CHECK(ui2.find("claude.dialog.ccswitch.row.1.cb.mark") != nullptr);
    const auto* clashName = ui2.find("claude.dialog.ccswitch.row.2.name");
    DOCTEST_REQUIRE(clashName != nullptr);
    DOCTEST_CHECK_EQ(clashName->text, std::string("撞名机"));
    DOCTEST_CHECK(ui2.find("claude.dialog.ccswitch.row.3.name") == nullptr);  // codex 滤掉
    const auto* rowHit = ui2.find("claude.dialog.ccswitch.row.1.hit");
    DOCTEST_REQUIRE(rowHit != nullptr);
    DOCTEST_REQUIRE(rowHit->onClick != nullptr);
    //（UAT6 修通单选）命中区几何锁：hit 必须与行卡同帧同位——曾经 hit 是 scrollView
    // 流式布局的兄弟节点，column 布局无视 position(0,0) 把命中区排进行间空隙，
    // 真机点行卡落空（headless 直调 onClick 测不出，必须锁 frame）。
    {
        const auto* rowBg = ui2.find("claude.dialog.ccswitch.row.1.bg");
        DOCTEST_REQUIRE(rowBg != nullptr);
        auto near = [](float a, float b) { return std::fabs(a - b) <= 0.5f; };
        DOCTEST_CHECK_MESSAGE(near(rowHit->frame.x, rowBg->frame.x) &&
                                  near(rowHit->frame.y, rowBg->frame.y) &&
                                  near(rowHit->frame.width, rowBg->frame.width) &&
                                  near(rowHit->frame.height, rowBg->frame.height),
                              "整行命中区与行卡不同位（真机点行将落空）");
        // 卡内文字不溢出卡底：model 行底 ≤ 行卡底（曾 model y40..52 越出 48 卡）
        const auto* model = ui2.find("claude.dialog.ccswitch.row.1.model");
        DOCTEST_REQUIRE(model != nullptr);
        DOCTEST_CHECK_MESSAGE(
            model->frame.y + model->frame.height <=
                rowBg->frame.y + rowBg->frame.height + 0.5f,
            "model 行跑出行卡底（" +
                std::to_string(model->frame.y + model->frame.height -
                               rowBg->frame.y - rowBg->frame.height) +
                "px）");
        // 行距收紧锁：相邻行卡顶距 = 62（kRowPitch 68 - 行间 scroll gap 影响）
        const auto* nextBg = ui2.find("claude.dialog.ccswitch.row.2.bg");
        if (nextBg != nullptr) {
            DOCTEST_CHECK_MESSAGE(
                near(nextBg->frame.y - rowBg->frame.y, 62.0f),
                "相邻行卡距应为 62，实测 " +
                    std::to_string(nextBg->frame.y - rowBg->frame.y));
        }
        // 全选钮带与列表间的呼吸位：列表顶 ≥ 钮带底 + 8（kListTopY=140, selectAll底=132）
        const auto* list = ui2.find("claude.dialog.ccswitch.list");
        const auto* selallBg = ui2.find("claude.dialog.ccswitch.selall.bg");
        if (list != nullptr && selallBg != nullptr) {
            DOCTEST_CHECK_MESSAGE(
                list->frame.y >= selallBg->frame.y + selallBg->frame.height +
                                    8.0f - 0.5f,
                "列表顶与全选钮带间应留 ≥8 空白");
        }
    }

    // 选择交互：点行翻勾选 → selall / 确认计数联动
    rowHit->onClick();
    eui::Ui ui3;
    composeFrame(*mod, ui3, "cc_ui_skel3");
    const auto* selall3 = ui3.find("claude.dialog.ccswitch.selall.text");
    DOCTEST_REQUIRE(selall3 != nullptr);
    DOCTEST_CHECK_EQ(selall3->text, std::string("全选"));
    const auto* confirm3 = ui3.find("claude.dialog.ccswitch.confirm.text.normal");
    DOCTEST_REQUIRE(confirm3 != nullptr);
    DOCTEST_CHECK_EQ(confirm3->text, std::string("导入选中(2)"));
    DOCTEST_CHECK(ui3.find("claude.dialog.ccswitch.row.1.cb.mark") == nullptr);
    //（UAT5 米黄收尾）未选中行色锁：bg == surface 白底灰边；cb.box 白圈灰边空圈
    //（20px 圆不缩水）；反向锁死暖橙 primary / legacy primary。
    {
        const auto* unRowBg = ui3.find("claude.dialog.ccswitch.row.1.bg");
        DOCTEST_REQUIRE(unRowBg != nullptr);
        DOCTEST_CHECK(sameColor(unRowBg->color, wc.surface));    // S1.2 #7: surface→surface
        DOCTEST_CHECK(!sameColor(unRowBg->color, wc.primary));   // 反向：不是绿色
        const auto* unBox = ui3.find("claude.dialog.ccswitch.row.1.cb.box");
        DOCTEST_REQUIRE(unBox != nullptr);
        DOCTEST_CHECK(sameColor(unBox->color, wc.surface));      // 白圈（source: surface）
        DOCTEST_CHECK(!sameColor(unBox->color, wc.primary));     // 反向：不是绿色
        DOCTEST_CHECK_EQ(unBox->radius, 10.0f);  // 20px 圆形复选框（kBoxSize/2），非标准控件圆角
        DOCTEST_CHECK(sameColor(unBox->border.color, wc.borderDefault));  // S1.2 #8
        DOCTEST_CHECK(!sameColor(unBox->border.color, wc.primary));       // 反向：不是绿色
    }
    // 翻回（后续用例不受此态影响；本用例到此为止）
    DOCTEST_REQUIRE(clickById(ui3, "claude.dialog.ccswitch.row.1.hit"));
    eui::Ui ui4;
    composeFrame(*mod, ui4, "cc_ui_skel4");
    const auto* confirm4 = ui4.find("claude.dialog.ccswitch.confirm.text.normal");
    DOCTEST_REQUIRE(confirm4 != nullptr);
    DOCTEST_CHECK_EQ(confirm4->text, std::string("导入选中(3)"));

    mod->onStop();
    test_support::setEnvVar("CCSWITCH_DB_PATH", "");
    return true;
})

// ③ 无内容空态：providers 表在但没有 Claude / Claude Desktop 行
REGISTER_TEST(claude_ccswitch_ui_empty_state,
              "claude_module: ccswitch-ui ③ 库里没有 Claude 供应商 → 空态文案，无列表无全选", {
    test_support::TempDir dir("ccswitch_ui_empty");
    DOCTEST_REQUIRE(createCcSwitchDb(dir.file("cc.db"), {
        providerRow("cc-x", "codex", "只有 codex", kGoodConfig),
    }));
    test_support::setEnvVar("CCSWITCH_DB_PATH", dir.file("cc.db").string());
    TestEnv env;
    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();

    eui::Ui ui;
    composeFrame(*mod, ui, "cc_ui_empty");
    DOCTEST_REQUIRE(clickById(ui, "claude.profiles.import.ccswitch.bg"));
    eui::Ui ui2;
    composeFrame(*mod, ui2, "cc_ui_empty2");

    DOCTEST_CHECK_EQ(ui2.find(kPanelId)->opacity, 1.0f);
    const auto* empty = ui2.find("claude.dialog.ccswitch.empty");
    DOCTEST_REQUIRE(empty != nullptr);   // 拔掉空态分支即红
    DOCTEST_CHECK(empty->text.find("无内容可导入") != std::string::npos);
    DOCTEST_CHECK(empty->text.find("Claude") != std::string::npos);
    // 空态不画列表 / 全选 / 行；确认键计数 0
    DOCTEST_CHECK(ui2.find("claude.dialog.ccswitch.list") == nullptr);
    DOCTEST_CHECK(ui2.find("claude.dialog.ccswitch.selall") == nullptr);
    DOCTEST_CHECK(ui2.find("claude.dialog.ccswitch.row.0.name") == nullptr);
    const auto* confirm = ui2.find("claude.dialog.ccswitch.confirm.text.normal");
    DOCTEST_REQUIRE(confirm != nullptr);
    DOCTEST_CHECK_EQ(confirm->text, std::string("导入选中(0)"));

    mod->onStop();
    test_support::setEnvVar("CCSWITCH_DB_PATH", "");
    return true;
})

// ④ 读库失败错误态：库不存在 → err 节点给中文原因，不画空态
REGISTER_TEST(claude_ccswitch_ui_load_error_state,
              "claude_module: ccswitch-ui ④ 库不存在 → 错误态文案（非空态、非静默）", {
    test_support::setEnvVar("CCSWITCH_DB_PATH", "/nonexistent-g3/cc-switch.db");
    TestEnv env;
    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();

    eui::Ui ui;
    composeFrame(*mod, ui, "cc_ui_err");
    DOCTEST_REQUIRE(clickById(ui, "claude.profiles.import.ccswitch.bg"));
    eui::Ui ui2;
    composeFrame(*mod, ui2, "cc_ui_err2");

    DOCTEST_CHECK_EQ(ui2.find(kPanelId)->opacity, 1.0f);  // 读库失败也开弹窗
    const auto* err = ui2.find("claude.dialog.ccswitch.err");
    DOCTEST_REQUIRE(err != nullptr);     // 拔掉错误态分支即红
    DOCTEST_CHECK(!err->text.empty());
    DOCTEST_CHECK(err->text.find("sk-") == std::string::npos);
    DOCTEST_CHECK(ui2.find("claude.dialog.ccswitch.empty") == nullptr);
    DOCTEST_CHECK(ui2.find("claude.dialog.ccswitch.list") == nullptr);

    mod->onStop();
    test_support::setEnvVar("CCSWITCH_DB_PATH", "");
    return true;
})

// ⑤ 导入链：选择 → 确认 → 落库（listProfiles 可见）+ 结果行 + toast
REGISTER_TEST(claude_ccswitch_ui_import_chain,
              "claude_module: ccswitch-ui ⑤ 导入链：选择→确认→落库+结果行+toast（同名跳过/成功各达）", {
    test_support::TempDir dir("ccswitch_ui_chain");
    DOCTEST_REQUIRE(createCcSwitchDb(dir.file("cc.db"), {
        providerRow("cc-0", "claude", "坏配置", "not-json"),
        providerRow("cc-1", "claude", "深港桥", kGoodConfig),
        providerRow("cc-2", "claude", "撞名机", kClashConfig),
    }));
    test_support::setEnvVar("CCSWITCH_DB_PATH", dir.file("cc.db").string());
    TestEnv env;
    test_support::ScopedAppDatabase scoped(env.db->db());
    // 预置同名 profile → 撞名机这条导入时必须走「跳过」桶（不覆盖）
    auto pre = env.svc->createProfile("撞名机", "https://existing.example.com",
                                      "sk-ant-pre-0003", "m-pre");
    DOCTEST_REQUIRE(pre.isOk());
    const int64_t preId = pre.value().id;
    auto before = env.svc->listProfiles();
    DOCTEST_REQUIRE(before.isOk());
    DOCTEST_REQUIRE_EQ(before.value().size(), std::size_t{1});

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();

    eui::Ui ui;
    composeFrame(*mod, ui, "cc_ui_chain");
    DOCTEST_REQUIRE(clickById(ui, "claude.profiles.import.ccswitch.bg"));
    eui::Ui ui2;
    composeFrame(*mod, ui2, "cc_ui_chain2");

    // 选择：先取消撞名机再勾回（选择交互进链路），最终 3 条全选导入
    DOCTEST_REQUIRE(clickById(ui2, "claude.dialog.ccswitch.row.2.hit"));
    eui::Ui ui3;
    composeFrame(*mod, ui3, "cc_ui_chain3");
    const auto* confirm3 = ui3.find("claude.dialog.ccswitch.confirm.text.normal");
    DOCTEST_REQUIRE(confirm3 != nullptr);
    DOCTEST_CHECK_EQ(confirm3->text, std::string("导入选中(2)"));
    DOCTEST_REQUIRE(clickById(ui3, "claude.dialog.ccswitch.row.2.hit"));
    eui::Ui ui4;
    composeFrame(*mod, ui4, "cc_ui_chain4");
    const auto* confirm4 = ui4.find("claude.dialog.ccswitch.confirm.text.normal");
    DOCTEST_REQUIRE(confirm4 != nullptr);
    DOCTEST_CHECK_EQ(confirm4->text, std::string("导入选中(3)"));

    // 导入动作：确认按钮回调（内部读 global service 现有名单 + confirmCcSwitchImport）
    DOCTEST_REQUIRE(clickById(ui4, "claude.dialog.ccswitch.confirm.bg"));
    eui::Ui ui5;
    composeFrame(*mod, ui5, "cc_ui_chain5");

    // 结果行：跳过（坏 JSON）/ 成功 / 跳过（同名）
    const auto* r0 = ui5.find("claude.dialog.ccswitch.result.0");
    DOCTEST_REQUIRE(r0 != nullptr);
    DOCTEST_CHECK(r0->text.find("跳过：坏配置") != std::string::npos);
    const auto* r1 = ui5.find("claude.dialog.ccswitch.result.1");
    DOCTEST_REQUIRE(r1 != nullptr);
    DOCTEST_CHECK_EQ(r1->text, std::string("成功：深港桥"));
    const auto* r2 = ui5.find("claude.dialog.ccswitch.result.2");
    DOCTEST_REQUIRE(r2 != nullptr);
    DOCTEST_CHECK(r2->text.find("跳过：撞名机") != std::string::npos);
    DOCTEST_CHECK(r2->text.find("同名") != std::string::npos);

    // toast：有跳过无失败 → 「导入完成」 headline
    const auto* toastTitle = ui5.find("claude.toast.title");
    DOCTEST_REQUIRE(toastTitle != nullptr);
    DOCTEST_CHECK_EQ(toastTitle->text, std::string("导入完成"));
    const auto* toastMsg = ui5.find("claude.toast.message");
    DOCTEST_REQUIRE(toastMsg != nullptr);
    DOCTEST_CHECK(toastMsg->text.find("成功 1") != std::string::npos);
    DOCTEST_CHECK(toastMsg->text.find("跳过 2") != std::string::npos);

    // 落库：新增恰 1 条（深港桥），字段逐项对上映射；撞名机原行未被覆盖
    auto after = env.svc->listProfiles();
    DOCTEST_REQUIRE(after.isOk());
    DOCTEST_REQUIRE_EQ(after.value().size(), std::size_t{2});
    const module::claude::Profile* imported = nullptr;
    const module::claude::Profile* untouched = nullptr;
    for (const auto& p : after.value()) {
        if (p.name == "深港桥") imported = &p;
        if (p.name == "撞名机") untouched = &p;
    }
    DOCTEST_REQUIRE(imported != nullptr);
    DOCTEST_REQUIRE(untouched != nullptr);
    DOCTEST_CHECK_EQ(imported->baseUrl, std::string("https://bridge.example.com"));
    DOCTEST_CHECK_EQ(imported->model, std::string("claude-sonnet-5-5"));
    DOCTEST_CHECK_EQ(imported->authField, std::string("bearer"));
    DOCTEST_CHECK(imported->extraEnvJson.find("ANTHROPIC_CUSTOM_HEADERS") !=
                  std::string::npos);
    DOCTEST_CHECK_EQ(untouched->id, preId);
    DOCTEST_CHECK_EQ(untouched->baseUrl, std::string("https://existing.example.com"));
    // 弹窗结果行不泄露密钥
    DOCTEST_CHECK(r1->text.find("sk-") == std::string::npos);

    mod->onStop();
    test_support::cleanupProfileKey(kTestKeychainService, preId);
    test_support::cleanupProfileKey(kTestKeychainService, imported->id);
    test_support::setEnvVar("CCSWITCH_DB_PATH", "");
    return true;
})
