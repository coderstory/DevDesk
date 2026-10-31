// tests/test_modules/test_ui_driver_schema.cpp
// 债-18 UI 驱动入口：driver_schema.h 无头单测（正例 + 负例矩阵）。
// 只 include 底层头（零 probe_harness 依赖——probe_harness 拖 png.h / GL loader /
// GLFW 窗口设施，本目标链接集没有 PNG 目标，混编排头即编译失败）。
#include "doctest.h"
#include "ui_driver/driver_schema.h"

#include "platform/json.h"

using namespace ui_drive;

namespace {

// 走完 parse 且成功取值；失败时把错误文案塞进 err 供断言
bool parseOk(const std::string& text, Scene* out, std::string* err = nullptr) {
    auto r = parseScene(text);
    if (r.isOk()) {
        if (out) *out = std::move(r.value());
        return true;
    }
    if (err) *err = r.errorMessage();
    std::cerr << "[uidrv/schema] parse 失败: " << r.errorMessage() << "\n";
    return false;
}

} // namespace

REGISTER_TEST(uiDriverSchemaPositiveFullScene, "uidrv/schema: 全字段正例解析", {
    const std::string text = R"({
        "version": 1,
        "settings": {
            "defaultWaitTimeoutMs": 2500,
            "failureStrategy": "keep-going",
            "window": {"width": 800, "height": 600},
            "seed": {"config": {"app.": {"enabledModules": ["todo"]}}}
        },
        "steps": [
            {"type": "click", "id": "todo.btn.add", "button": "left",
             "force": false, "readback": true, "_comment": "新建任务"},
            {"type": "type", "text": "缴费"},
            {"type": "key", "name": "Ctrl+S"},
            {"type": "scroll", "dx": 0, "dy": -3, "id": "todo.root"},
            {"type": "drag", "from": "a", "to": "b", "steps": 6},
            {"type": "wait", "until": "exists", "id": "todo.item.x", "timeoutMs": 900},
            {"type": "wait", "until": "text", "op": "equals", "text": "缴费"},
            {"type": "wait", "ms": 400},
            {"type": "assert", "check": "text", "id": "todo.h1", "op": "contains", "text": "任务"},
            {"type": "assert", "check": "geometry", "id": "todo.root", "op": "within-window"},
            {"type": "screenshot", "path": "out.png"},
            {"type": "dump", "path": "ids.txt"}
        ]
    })";
    Scene scene;
    if (!parseOk(text, &scene)) return false;
    DOCTEST_CHECK_EQ(scene.settings.defaultWaitTimeoutMs, 2500);
    DOCTEST_CHECK(scene.settings.keepGoing);
    DOCTEST_CHECK_EQ(scene.settings.windowWidth, 800);
    DOCTEST_CHECK_EQ(scene.settings.windowHeight, 600);
    DOCTEST_CHECK_EQ(scene.settings.seedConfigs.size(), std::size_t(1));
    DOCTEST_CHECK_EQ(scene.settings.seedConfigs[0].first, std::string("app."));
    DOCTEST_CHECK(scene.settings.seedConfigs[0].second.find("enabledModules") != std::string::npos);
    DOCTEST_CHECK_EQ(scene.steps.size(), std::size_t(12));
    DOCTEST_CHECK_EQ(scene.steps[0].type, std::string("click"));
    DOCTEST_CHECK_EQ(scene.steps[0].id, std::string("todo.btn.add"));
    DOCTEST_CHECK_EQ(scene.steps[0].button, std::string("left"));
    DOCTEST_CHECK_FALSE(scene.steps[0].force);
    DOCTEST_CHECK(scene.steps[0].readback);
    DOCTEST_CHECK_EQ(scene.steps[0].comment, std::string("新建任务"));
    DOCTEST_CHECK_EQ(scene.steps[2].keyName, std::string("Ctrl+S"));
    DOCTEST_CHECK_EQ(scene.steps[5].timeoutMs, 900);
    DOCTEST_CHECK_EQ(scene.steps[7].ms, 400);
    DOCTEST_CHECK_EQ(scene.steps[9].op, std::string("within-window"));
    return true;
})

REGISTER_TEST(uiDriverSchemaDefaults, "uidrv/schema: 缺省值（settings 省略 / op 省略 contains）", {
    Scene scene;
    if (!parseOk(R"({"version":1,"steps":[{"type":"assert","check":"text","text":"x"}]})", &scene))
        return false;
    DOCTEST_CHECK_EQ(scene.settings.defaultWaitTimeoutMs, 5000);
    DOCTEST_CHECK_FALSE(scene.settings.keepGoing);
    DOCTEST_CHECK_EQ(scene.settings.windowWidth, 1200);
    DOCTEST_CHECK_EQ(scene.steps[0].op, std::string());  // op 省略 = contains（F-03）
    // wait 存在类缺省 timeoutMs=0（执行期用 settings 默认）
    Scene waitScene;
    if (!parseOk(R"({"version":1,"steps":[{"type":"wait","until":"exists","id":"a"}]})", &waitScene))
        return false;
    DOCTEST_CHECK_EQ(waitScene.steps[0].timeoutMs, 0);
    return true;
})

REGISTER_TEST(uiDriverSchemaMissingVersion, "uidrv/schema: 缺 version 拒绝", {
    std::string err;
    DOCTEST_CHECK_FALSE(parseOk(R"({"steps":[{"type":"wait","ms":100}]})", nullptr, &err));
    DOCTEST_CHECK(err.find("version") != std::string::npos);
    return true;
})

REGISTER_TEST(uiDriverSchemaWrongVersion, "uidrv/schema: version=2 拒绝", {
    std::string err;
    DOCTEST_CHECK_FALSE(
        parseOk(R"({"version":2,"steps":[{"type":"wait","ms":100}]})", nullptr, &err));
    DOCTEST_CHECK(err.find("仅支持 1") != std::string::npos);
    return true;
})

REGISTER_TEST(uiDriverSchemaUnknownTopKey, "uidrv/schema: 未知顶层键给相近候选", {
    std::string err;
    DOCTEST_CHECK_FALSE(
        parseOk(R"({"version":1,"step":[1],"steps":[{"type":"wait","ms":50}]})", nullptr, &err));
    DOCTEST_CHECK(err.find("step") != std::string::npos);
    DOCTEST_CHECK(err.find("steps") != std::string::npos);  // 相近候选
    return true;
})

REGISTER_TEST(uiDriverSchemaUnknownStepType, "uidrv/schema: 未知步骤 type 给相近候选", {
    std::string err;
    DOCTEST_CHECK_FALSE(
        parseOk(R"({"version":1,"steps":[{"type":"clicks","id":"a"}]})", nullptr, &err));
    DOCTEST_CHECK(err.find("clicks") != std::string::npos);
    DOCTEST_CHECK(err.find("click") != std::string::npos);
    return true;
})

REGISTER_TEST(uiDriverSchemaZeroStepsRejected, "uidrv/schema: 0 步防假绿拒绝", {
    std::string err;
    DOCTEST_CHECK_FALSE(parseOk(R"({"version":1,"steps":[]})", nullptr, &err));
    DOCTEST_CHECK(err.find("场景无步骤") != std::string::npos);
    return true;
})

REGISTER_TEST(uiDriverSchemaFieldTypeErrors, "uidrv/schema: 字段类型错 / 缺字段 / 互斥", {
    std::string err;
    // type 字段类型错
    DOCTEST_CHECK_FALSE(
        parseOk(R"({"version":1,"steps":[{"type":3}]})", nullptr, &err));
    DOCTEST_CHECK(err.find("type") != std::string::npos);
    // click 缺 id 与 point
    DOCTEST_CHECK_FALSE(
        parseOk(R"({"version":1,"steps":[{"type":"click"}]})", nullptr, &err));
    DOCTEST_CHECK(err.find("id") != std::string::npos);
    // click id 与 point 互斥
    DOCTEST_CHECK_FALSE(parseOk(
        R"({"version":1,"steps":[{"type":"click","id":"a","point":[1,2]}]})", nullptr, &err));
    DOCTEST_CHECK(err.find("互斥") != std::string::npos);
    // point 非二元数组
    DOCTEST_CHECK_FALSE(parseOk(
        R"({"version":1,"steps":[{"type":"click","point":[1]}]})", nullptr, &err));
    // assert.check 未知值
    DOCTEST_CHECK_FALSE(parseOk(
        R"({"version":1,"steps":[{"type":"assert","check":"size"}]})", nullptr, &err));
    DOCTEST_CHECK(err.find("exists") != std::string::npos);
    // key 键名查表 miss 给相近候选（Ente → Enter 前缀）
    DOCTEST_CHECK_FALSE(parseOk(
        R"({"version":1,"steps":[{"type":"key","name":"Ente"}]})", nullptr, &err));
    DOCTEST_CHECK(err.find("Enter") != std::string::npos);
    // wait until 与 ms 都缺
    DOCTEST_CHECK_FALSE(
        parseOk(R"({"version":1,"steps":[{"type":"wait"}]})", nullptr, &err));
    return true;
})

REGISTER_TEST(uiDriverSchemaStepKeyWhitelist, "uidrv/schema: 步骤字段白名单（click 不收 text）", {
    std::string err;
    DOCTEST_CHECK_FALSE(parseOk(
        R"({"version":1,"steps":[{"type":"click","id":"a","text":"b"}]})", nullptr, &err));
    DOCTEST_CHECK(err.find("text") != std::string::npos);
    return true;
})

REGISTER_TEST(uiDriverSchemaParseErrorLine, "uidrv/schema: 坏 JSON 报行号（platform::json ParseDiag 透出）", {
    // 第 2 行是坏点（bytePos 落在第一行换行之后）
    const std::string text = "{\n  \"version\": 1,\n  \"steps\": [BAD}\n}";
    std::string err;
    DOCTEST_CHECK_FALSE(parseOk(text, nullptr, &err));
    DOCTEST_CHECK(err.find("第 3 行") != std::string::npos);
    DOCTEST_CHECK(err.find("yyjson:") != std::string::npos);
    // ParseDiag 直接断言（bytePos / msg 非零非空）
    platform::json::ParseDiag diag;
    auto r = platform::json::parse(text, &diag);
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK(diag.bytePos > 0);
    DOCTEST_CHECK_FALSE(diag.msg.empty());
    // 单参 parse 委托后行为不变（同样失败，Result 链文案与旧口径一致）
    auto r2 = platform::json::parse(text);
    DOCTEST_CHECK(r2.isFail());
    DOCTEST_CHECK(r2.errorMessage().find("JSON 解析失败") != std::string::npos);
    return true;
})

REGISTER_TEST(uiDriverSchemaLoadMissingFile, "uidrv/schema: 文件不存在退出码 2 档错误", {
    auto r = loadSceneFile("/nonexistent/uidrv-scene-absent.json");
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK(r.errorMessage().find("场景文件不存在") != std::string::npos);
    return true;
})
