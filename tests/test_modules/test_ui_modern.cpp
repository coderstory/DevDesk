// tests/test_modules/test_ui_modern.cpp —— UI 现代化无头用例（2026-09-28）：
//   1. 统一锚点换算纯测（含 Y、含滚动偏移、dpi 退化）—— M1-1
//   2. todo 看板右键锚点 = 板几何经统一换算（含滚动偏移口径保留）—— M1-2
//   3. claude profile 列表右键菜单四项 + 动作同源 + 隐形卡守卫 —— M1-5
// 三个用例随各自任务（M1-1/M1-2/M1-5）分批加入本文件。
// 证据链 = 代码路径 + 本无头 compose 测试；真实鼠标点验留给用户人工确认
#include "doctest.h"
#include "test_support.h"

#include "core/database.h"
#include "core/module_interface.h"
#include "core/sqlite_db.h"
#include "core/theme.h"
#include "core/ui_geometry.h"
#include "module_claude_proxy/claude_service.h"
#include "module_todo/todo_service.h"
#include "module_todo/todo_ui_logic.h"
#include "ui_kit/metrics.h"  // kHeaderH（板几何断言）

#include <eui/dsl.h>

#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

extern "C" ::core::Module* create_todo_module();
extern "C" ::core::Module* create_claude_module();

namespace {

// ---- M1-2 用：todo 无头环境（照 test_todo_ui.cpp 的 TestEnv 形态）----
constexpr const char* kTodoDbPrefix = "devdesk_test_uimodern_todo_";

struct TodoEnv : test_support::TestEnv {
    std::filesystem::path statePath;
    std::unique_ptr<module::todo::TodoService> svc;

    TodoEnv() : test_support::TestEnv(kTodoDbPrefix, "", true) {
        svc = std::make_unique<module::todo::TodoService>(db->db());
        statePath = path.string() + ".view_state.json";
        std::error_code ec;
        std::filesystem::remove(statePath, ec);
        test_support::setEnvVar("DEVDESK_TODO_VIEW_STATE_PATH",
                                statePath.string());
    }

    ~TodoEnv() {
        test_support::setEnvVar("DEVDESK_TODO_VIEW_STATE_PATH", "");
        std::error_code ec;
        std::filesystem::remove(statePath, ec);
    }
};

// 无头 compose 一帧（todo），返回构建后的 Ui
struct TodoFrame {
    eui::Ui ui;
    eui::Screen screen;
    ::core::Module* mod = nullptr;

    TodoFrame(::core::Module* m, float w = 1600.0f, float h = 1000.0f)
        : mod(m) {
        screen.width = w;
        screen.height = h;
        refresh();
    }
    void refresh() {
        ui.begin("todo_uimodern_frame");
        mod->compose(ui, screen);
        ui.end();
    }
};

// ---- M1-5 用：claude 无头环境（照 test_claude_ui.cpp 的 TestEnv 形态）----
constexpr const char* kClaudeDbPrefix = "devdesk_test_uimodern_claude_";
constexpr const char* kClaudeKeychainService = "DevDeskTestUiModernClaude";

struct ClaudeEnv : test_support::TestDb {
    std::unique_ptr<module::claude::ClaudeService> svc;

    ClaudeEnv() : test_support::TestDb(kClaudeDbPrefix, "") {
        svc = std::make_unique<module::claude::ClaudeService>(
            db->db(), kClaudeKeychainService);
        module::claude::ClaudeService::setGlobal(svc.get());
    }

    ~ClaudeEnv() { module::claude::ClaudeService::setGlobal(nullptr); }
};

} // namespace

// ---- 1. 统一锚点换算纯测（M1-1）----

REGISTER_TEST(uiModernAnchorPure,
              "uiModern: 统一锚点换算（含 Y、含滚动偏移、dpi 退化）", {
    // 基准：窗口 (100, 200)，页面原点 x=20，dpi=1.0（onContextMenu 通道口径）
    const ::core::MenuAnchor a0 =
        ::core::menuAnchorForPointer(100.0f, 200.0f, 20.0f, 0.0f, 1.0f);
    DOCTEST_CHECK_EQ(a0.x, 80.0f);
    DOCTEST_CHECK_EQ(a0.y, 200.0f);
    // 滚动偏移（基于非滚动几何的锚点，4523e09 口径）：
    const ::core::MenuAnchor a1 =
        ::core::menuAnchorForPointer(100.0f, 200.0f, 20.0f, 36.0f, 1.0f);
    DOCTEST_CHECK_EQ(a1.y, 164.0f);
    // dpi=2.0（onPress/onDrag 通道口径：原始值 / 框架值）：
    const ::core::MenuAnchor a2 =
        ::core::menuAnchorForPointer(100.0f, 200.0f, 20.0f, 0.0f, 2.0f);
    DOCTEST_CHECK_EQ(a2.x, 30.0f);   // 100/2 - 20
    DOCTEST_CHECK_EQ(a2.y, 100.0f);
    // 退化 dpi（0/负/NaN）→ 按 1.0 口径（windowToCompose 同款）：
    DOCTEST_CHECK_EQ(
        ::core::menuAnchorForPointer(100.0f, 200.0f, 20.0f, 0.0f, 0.0f).x,
        80.0f);
    return true;
})
