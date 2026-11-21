// notepad_probe.cpp — 记事本窗口层探针（need:273 批 1 + need:263 批 2 棒 2 + 批 3 棒 B 增段）
//
// headless compose 驱动 openNotepadWindow（临时真文件走真实载入链）：
//   A 正常态：工具条 id 全集（btnOpen/btnSave/toolbar）+ 无新建钮（N-14）+
//     编辑 input 存在（D-6「无编辑入口」断言反转）+ 状态栏（文件名/行:列/字符数）
//   B 只读降级：UTF-16 BOM 文件 → promptbar 提示条在场 + 保存钮 disabled
//   C 错误占位：不存在路径 → session 照建 + loaderror 占位（无编辑器无提示条）
//   D 查找条键盘全流程（批 2 F-06，窗口级 onKeyEvent 直驱）：M+F 开条 →
//     注入查询 → Enter 单调跳转（含环绕）→ Shift+Enter 反向 → Esc 清高亮留词
//     → 再唤起保留词全选
//   E 扩展名按钮矩阵 + 预览切换（批 3 棒 B F-11/F-13）：.json 会话出现格式化钮
//     （预览钮不在场）、.md 会话出现预览钮（格式化钮不在场）、.txt 两钮均不在场；
//     previewOn 后编辑 input 槽位让位 + 查找条不渲染 + A± 禁用；切回编辑
//     input 回归 + 光标保留
//   G 未保存保护链（批 4 棒 A F-05）：veto 裁决（干净直通/脏拦截）+ 三选弹窗
//     树（三钮 id 锚）+ 不保存→豁免→窗关（计数减）+ 保存→落盘关窗（内容核对）
//     + quit guard 全链（拒退→处理→放行 / 取消不卡死 / 全豁免放行 + requestExit
//     再入队语义，clue #269）
//   H 另存为+快捷键（need289 修复棒 1 F-04/§九）：另存为钮存在+禁用矩阵
//     （可编辑/只读/错误窗）+ M+Shift+S 只读·错误窗守卫零副作用 + M+H 替换
//     直开 + M+Shift+F 格式化置位-消费 + M+P·M+Shift+V 预览切换
// 运行：notepad_probe [--screenshot <路径>]（不带 = 零开销不落图）。
#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（openWindow=入队不建窗）

#include <components/input.h>  // 编辑 InputState 直读（光标/选区断言）
#include <components/scrollview.h>  // bug399 round3 差分实验：普通 scrollView+text
#include <components/markdown.h>  // bug399 round7：仪表分解 + A/B 基准
#include "module_notepad/notepad_module.h"
#include "module_notepad/notepad_protect.h"  // G 段：veto 裁决/quit guard 决策面
#include "module_notepad/notepad_session.h"  // notepadComposeSession（生产 compose 单点）
#include "core/component_theme.h"  // Q 段：MarkdownStyle 口径（与预览同源 token）

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <unordered_map>
#include <fstream>
#include <iostream>
#include <iterator>  // istreambuf_iterator（G3 落盘内容核对）
#include <string>
#include <thread>  // J 段惯性收尾帧间短 sleep（实机节奏）

// app:: 层桩（probe 不建应用级 UI；同 markdown_probe 口径）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_notepad_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

bool fail(const std::string& what) {
    std::cerr << "notepad_probe: FAIL — " << what << "\n";
    return false;
}

// bug399 round2 像素判据：统计矩形内「与主色不同的像素数」（文字墨迹簇）。
// 主色 = 矩形内量化众数（背景），任通道差值 > 12 计为内容像素；读帧一律 16
// 遍（probe_harness readFrame 注：retained 几何层需 kRetainedLayerResizeStableFrames=16
// 帧稳定门槛，未达门槛整层不画——探针冷层单遍捕获会假空白）。
int countInkPixels(const std::vector<unsigned char>& rgba, int width, int height,
                   float scale, float rx, float ry, float rw, float rh) {
    const int x0 = std::max(0, static_cast<int>(rx * scale));
    const int y0 = std::max(0, static_cast<int>(ry * scale));
    const int x1 = std::min(width, static_cast<int>((rx + rw) * scale));
    const int y1 = std::min(height, static_cast<int>((ry + rh) * scale));
    if (x1 - x0 <= 4 || y1 - y0 <= 4 || width <= 0 || height <= 0) return -1;
    int ink = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                   static_cast<std::size_t>(x)) * 4u;
            const int luma = (static_cast<int>(rgba[i]) * 299 +
                              static_cast<int>(rgba[i + 1]) * 587 +
                              static_cast<int>(rgba[i + 2]) * 114) / 1000;
            if (luma > 140) ++ink;
        }
    }
    return ink;
}

// 临时夹具目录（固定名 + 每次重建；探针进程独占，无并发共享）
std::filesystem::path makeFixtureDir() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      "devdesk_notepad_probe_fixture";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("notepad_probe", 900, 640);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    bool passed = true;
    module::notepad::resetNotepadSessionsForTest();

    const std::filesystem::path dir = makeFixtureDir();
    const std::string notePath = (dir / "note.md").string();
    const std::string utf16Path = (dir / "utf16.txt").string();
    const std::string ghostPath = (dir / "ghost_missing.md").string();
    const std::string jsonPath = (dir / "cfg.json").string();
    const std::string plainTxtPath = (dir / "plain.txt").string();
    {
        std::ofstream note(notePath, std::ios::binary);
        note << "hello\nworld\n";  // 12 码点，无 BOM，LF
    }
    {
        std::ofstream u16(utf16Path, std::ios::binary);
        u16 << "\xFE\xFF\x00h\x00i";  // UTF-16BE BOM → 只读降级
    }
    {
        std::ofstream jf(jsonPath, std::ios::binary);
        jf << "{\"b\":1,\"a\":2}";
    }
    {
        std::ofstream tf(plainTxtPath, std::ios::binary);
        tf << "plain text\n";
    }

    const float w = 900.0f;
    const float h = 640.0f;
    core::dsl::Ui* ui = nullptr;  // 每帧由 compose 回调刷新（markdown_probe 同款）
    auto composeSession = [&](module::notepad::NotepadSession& s) {
        probe::composeModuleFrame(
            runtime, window, s.pageId, w, h,
            [&](eui::Ui& u, const eui::Screen& screen) {
                ui = &u;
                module::notepad::notepadComposeSession(s, u, screen);
            });
    };

    // ── A. 正常态：真实载入链（openNotepadWindow → 注册表 → 生产 compose）──
    module::notepad::openNotepadWindow(notePath);
    if (module::notepad::notepadSessionCountForTest() != 1) {
        passed = fail("正常态开窗后 session 数 != 1");
    }
    if (passed) {
        module::notepad::NotepadSession& s =
            *module::notepad::detail::sessionRegistry().back();
        composeSession(s);
        composeSession(s);  // 稳帧：首帧建树，第二帧布局收敛
        if (ui == nullptr) passed = fail("compose 未建出 UI 树");
        if (ui->find(module::notepad::kNotepadToolbarId) == nullptr)
            passed = fail("工具条 notepad.toolbar 不在场");
        if (passed && ui->find(module::notepad::kNotepadBtnOpenId) == nullptr)
            passed = fail("打开钮 notepad.btnOpen 不在场");
        if (passed && ui->find(module::notepad::kNotepadBtnSaveId) == nullptr)
            passed = fail("保存钮 notepad.btnSave 不在场");
        if (passed) {
            const core::dsl::Element* save = ui->find(module::notepad::kNotepadBtnSaveId);
            if (save->disabled) passed = fail("正常态保存钮不应 disabled");
        }
        if (passed && ui->find("notepad.btnNew") != nullptr)
            passed = fail("出现新建钮（违反 N-14 无新建入口）");
        if (passed && ui->find(module::notepad::kNotepadEditorInputId) == nullptr)
            passed = fail("编辑 input notepad.editor.input 不在场（D-6 断言反转：记事本必有编辑入口）");
        if (passed && ui->find(module::notepad::kNotepadStatusbarId) == nullptr)
            passed = fail("状态栏 notepad.statusbar 不在场");
        if (passed && ui->find("notepad.promptbar") != nullptr)
            passed = fail("正常态不应出现提示条 notepad.promptbar");
        if (passed) {
            const core::dsl::Element* title = ui->find("notepad.status.title");
            const core::dsl::Element* cursor = ui->find("notepad.status.cursor");
            const core::dsl::Element* chars = ui->find("notepad.status.chars");
            if (title == nullptr || cursor == nullptr || chars == nullptr) {
                passed = fail("状态栏文本三件套（title/cursor/chars）缺件");
            } else {
                if (title->text != "note.md")
                    passed = fail("状态栏标题非文件名: " + title->text);
                if (passed && cursor->text != "行 1, 列 1")
                    passed = fail("状态栏行列非初始值: " + cursor->text);
                if (passed && chars->text != "12 字符")
                    passed = fail("状态栏字符数非 12: " + chars->text);
            }
        }
    }

    // ── B. 只读降级：UTF-16 BOM → promptbar + 保存钮禁用 ──
    if (passed) {
        module::notepad::openNotepadWindow(utf16Path);
        if (module::notepad::notepadSessionCountForTest() != 2)
            passed = fail("只读态开窗后 session 数 != 2");
    }
    if (passed) {
        module::notepad::NotepadSession& s =
            *module::notepad::detail::sessionRegistry().back();
        if (!s.readonly) passed = fail("UTF-16 BOM 文件未进只读降级");
        composeSession(s);
        composeSession(s);
        if (ui == nullptr) passed = fail("compose 未建出 UI 树");
        if (passed && ui->find("notepad.promptbar") == nullptr)
            passed = fail("只读态提示条 notepad.promptbar 不在场");
        if (passed && ui->find("notepad.promptbar.text") == nullptr)
            passed = fail("只读提示文案 notepad.promptbar.text 不在场");
        if (passed) {
            const core::dsl::Element* save = ui->find(module::notepad::kNotepadBtnSaveId);
            if (save == nullptr || !save->disabled)
                passed = fail("只读态保存钮未 disabled（禁用态断言）");
        }
        if (passed && ui->find(module::notepad::kNotepadEditorInputId) == nullptr)
            passed = fail("只读态编辑控件不在场（降级=只读仍开窗，非删编辑器）");
    }

    // ── C. 错误占位：不存在路径 → session 照建 + loaderror 占位 ──
    if (passed) {
        module::notepad::openNotepadWindow(ghostPath);
        if (module::notepad::notepadSessionCountForTest() != 3)
            passed = fail("错误态开窗后 session 数 != 3（A2：窗照开照关）");
    }
    if (passed) {
        module::notepad::NotepadSession& s =
            *module::notepad::detail::sessionRegistry().back();
        if (s.loadError.empty()) passed = fail("不存在路径未产生 loadError 占位");
        composeSession(s);
        composeSession(s);
        if (ui == nullptr) passed = fail("compose 未建出 UI 树");
        const core::dsl::Element* err = ui->find("notepad.loaderror");
        if (passed && err == nullptr) passed = fail("错误占位 notepad.loaderror 不在场");
        if (passed && err != nullptr && err->text != s.loadError)
            passed = fail("错误占位文案与 session.loadError 不一致");
        if (passed && ui->find(module::notepad::kNotepadEditorInputId) != nullptr)
            passed = fail("错误态不应有编辑 input");
        if (passed && ui->find("notepad.promptbar") != nullptr)
            passed = fail("错误态不应叠提示条（错误文案占编辑区）");
        if (passed) {
            const core::dsl::Element* save = ui->find(module::notepad::kNotepadBtnSaveId);
            if (save == nullptr || !save->disabled)
                passed = fail("错误态保存钮未 disabled");
        }
        // P2-1：错误窗字体触发器禁用（matrix.fontControlsEnabled 含 !errorState）
        if (passed) {
            const core::dsl::Element* ddFont =
                ui->find(module::notepad::kNotepadDdFontId);
            if (ddFont == nullptr || !ddFont->disabled)
                passed = fail("错误窗字体触发器未禁用（P2-1）");
        }
    }

    // ── D. 查找条键盘全流程（批 2 F-06；窗口级 onKeyEvent 直驱 + 会话态注入）──
    if (passed) {
        module::notepad::NotepadSession& s =
            *module::notepad::detail::sessionRegistry().front();  // A 正常态窗
        composeSession(s);
        composeSession(s);

        // 窗口级键事件直驱（KeyEvent 直构造，M = Cmd/Super（mac）或 Ctrl（win）
        // 双置位覆盖双平台 shortcut() 判定）
        const auto pressKey = [&](eui::InputKey key, bool withShortcut,
                                  bool withShift) {
            eui::KeyEvent ev;
            ev.key = key;
            ev.action = core::KeyAction::Press;
            ev.modifiers.control = withShortcut;
            ev.modifiers.super = withShortcut;
            ev.modifiers.shift = withShift;
            module::notepad::notepadHandleKeySession(s, ev);
        };

        pressKey(eui::InputKey::F, true, false);  // M+F 开条
        composeSession(s);
        composeSession(s);
        if (ui->find(module::notepad::kNotepadSearchBarId) == nullptr)
            passed = fail("M+F 后查找条 notepad.searchBar 不在场");
        if (passed && ui->find(module::notepad::kNotepadSearchQueryInputId) == nullptr)
            passed = fail("查找输入 notepad.search.query 不在场");

        // 注入查询（与 onChange 同通路 = 会话态直写；compose 门控应用）
        s.find.query = "l";
        composeSession(s);
        if (passed) {
            const core::dsl::Element* count = ui->find(module::notepad::kNotepadSearchCountId);
            if (count == nullptr || count->text != "1/3")
                passed = fail(std::string("计数非 1/3: ") +
                              (count ? count->text : "(缺件)"));
        }

        using InputState = components::input_detail::InputModel::InputState;
        const auto editorCursor = [&]() -> const InputState& {
            return ui->state<InputState>(module::notepad::kNotepadEditorInputId);
        };

        // Enter 单调跳转（严格后继，含环绕）：cursor=0 → 命中[0](2) →
        // 命中[1](3) → 命中[2](9) → 环绕回命中[0](2)
        pressKey(eui::InputKey::Enter, false, false);
        composeSession(s);
        if (passed && editorCursor().cursor != 2)
            passed = fail("首次 Enter 光标未到命中[0]（期望 2）: " +
                          std::to_string(editorCursor().cursor));
        pressKey(eui::InputKey::Enter, false, false);
        composeSession(s);
        if (passed && editorCursor().cursor != 3)
            passed = fail("二次 Enter 光标未到命中[1]（期望 3）: " +
                          std::to_string(editorCursor().cursor));
        pressKey(eui::InputKey::Enter, false, false);
        composeSession(s);
        if (passed && editorCursor().cursor != 9)
            passed = fail("三次 Enter 光标未到命中[2]（期望 9）: " +
                          std::to_string(editorCursor().cursor));
        pressKey(eui::InputKey::Enter, false, false);
        composeSession(s);
        if (passed && editorCursor().cursor != 2)
            passed = fail("四次 Enter 未环绕回首命中（期望 2）: " +
                          std::to_string(editorCursor().cursor));
        // Shift+Enter 反向：cursor=2 → 环绕回末命中（byte 9）
        pressKey(eui::InputKey::Enter, false, true);
        composeSession(s);
        if (passed && editorCursor().cursor != 9)
            passed = fail("Shift+Enter 未环绕回末命中（期望 9）: " +
                          std::to_string(editorCursor().cursor));
        if (passed && editorCursor().selectionStart != 9)
            passed = fail("跳转后选区未随命中（期望 selectionStart=9）");

        // Esc 清高亮留词（F-06）
        pressKey(eui::InputKey::Escape, false, false);
        composeSession(s);
        if (passed && ui->find(module::notepad::kNotepadSearchBarId) != nullptr)
            passed = fail("Esc 后查找条仍在场");
        if (passed && s.find.query != "l")
            passed = fail("Esc 未保留查找词（F-06 留词）");
        if (passed && !s.find.hits.empty())
            passed = fail("Esc 未清命中集（高亮未清）");

        // 再唤起：保留词全选 + 重算恢复高亮（cursor=9 → 当前命中=命中[2]）
        pressKey(eui::InputKey::F, true, false);
        composeSession(s);
        composeSession(s);
        if (passed && ui->find(module::notepad::kNotepadSearchBarId) == nullptr)
            passed = fail("再唤起后查找条不在场");
        if (passed) {
            const InputState& qst =
                ui->state<InputState>(module::notepad::kNotepadSearchQueryInputId);
            if (qst.text != "l")
                passed = fail("再唤起输入框未保留词: " + qst.text);
            if (passed && !(qst.selectionStart == 0 && qst.selectionEnd == 1))
                passed = fail("再唤起未全选保留词");
            const core::dsl::Element* count = ui->find(module::notepad::kNotepadSearchCountId);
            if (passed && (count == nullptr || count->text != "3/3"))
                passed = fail(std::string("再唤起计数非 3/3: ") +
                              (count ? count->text : "(缺件)"));
        }

        // P2-3：关条焦点回归编辑器（Esc 关条 → 焦点从查找输入切回编辑 input）
        if (passed) {
            pressKey(eui::InputKey::Escape, false, false);
            composeSession(s);
            composeSession(s);
            if (ui->find(module::notepad::kNotepadSearchBarId) != nullptr)
                passed = fail("P2-3: Esc 后查找条仍在场");
            // need376：聚焦目标是组件的可聚焦元素 `<id>.hit`，不是组件根 id——
            // 实现侧（notepad_window.cpp:1432）已是 `requestFocus(kNotepadEditorInputId + ".hit")`，
            // 本断言必须同形，否则恒假（bug373）。
            if (passed &&
                !ui->isFocused(std::string(module::notepad::kNotepadEditorInputId) + ".hit"))
                passed = fail("P2-3: 关条后焦点未回归编辑器（isFocused 编辑 input.hit 为假）");
        }
    }

    // ── E. 扩展名按钮矩阵 + 预览切换（批 3 棒 B F-11/F-13）──
    if (passed) {
        module::notepad::openNotepadWindow(jsonPath);   // session #4
        module::notepad::openNotepadWindow(plainTxtPath);  // session #5
        if (module::notepad::notepadSessionCountForTest() != 5)
            passed = fail("E 段开窗后 session 数 != 5");
    }
    // .json 会话：格式化钮在场且可用，预览钮不在场
    if (passed) {
        module::notepad::NotepadSession& js =
            *module::notepad::detail::sessionRegistry().end()[-2];
        composeSession(js);
        composeSession(js);
        const core::dsl::Element* fmt =
            ui->find(module::notepad::kNotepadBtnFormatId);
        if (fmt == nullptr) passed = fail(".json 会话格式化钮 notepad.btnFormat 不在场");
        if (passed && fmt->disabled) passed = fail(".json 会话格式化钮不应禁用");
        if (passed && ui->find(module::notepad::kNotepadBtnPreviewId) != nullptr)
            passed = fail(".json 会话不应出现预览钮（按钮矩阵）");
    }
    // .md 会话（A 段 note.md）：预览钮在场，格式化钮不在场
    if (passed) {
        module::notepad::NotepadSession& ms =
            *module::notepad::detail::sessionRegistry().front();
        composeSession(ms);
        composeSession(ms);
        if (ui->find(module::notepad::kNotepadBtnPreviewId) == nullptr)
            passed = fail(".md 会话预览钮 notepad.btnPreview 不在场");
        if (passed && ui->find(module::notepad::kNotepadBtnFormatId) != nullptr)
            passed = fail(".md 会话不应出现格式化钮（按钮矩阵）");
    }
    // .txt 会话：两钮均不在场（按扩展名的按钮矩阵）
    if (passed) {
        module::notepad::NotepadSession& ts =
            *module::notepad::detail::sessionRegistry().back();
        composeSession(ts);
        composeSession(ts);
        if (ui->find(module::notepad::kNotepadBtnFormatId) != nullptr)
            passed = fail(".txt 会话不应出现格式化钮");
        if (passed && ui->find(module::notepad::kNotepadBtnPreviewId) != nullptr)
            passed = fail(".txt 会话不应出现预览钮");
    }
    // 预览切换树断言：编辑 input 槽位让位 + 查找条不渲染 + A± 禁用 +
    // 切回后编辑 input 回归且光标保留（InputState 不动）
    if (passed) {
        module::notepad::NotepadSession& ms =
            *module::notepad::detail::sessionRegistry().front();
        composeSession(ms);
        composeSession(ms);  // ui 指针随最后 compose 的树走：先切回 ms 树再写状态
        using InputState = components::input_detail::InputModel::InputState;
        {  // 光标落在中间（切回保留断言锚）
            InputState& st =
                ui->state<InputState>(module::notepad::kNotepadEditorInputId);
            st.cursor = 6;
            st.selectionStart = 6;
            st.selectionEnd = 6;
        }
        ms.find.visible = true;  // 条开着切预览 → 预览态不渲染（§4.6 查找禁用）
        ms.previewOn = true;
        composeSession(ms);
        composeSession(ms);
        if (ui->find(module::notepad::kNotepadPreviewId) == nullptr)
            passed = fail("预览态 notepad.preview 滚动槽位不在场");
        if (passed && ui->find(module::notepad::kNotepadEditorInputId) != nullptr)
            passed = fail("预览态编辑 input 未让位（槽位未替换）");
        if (passed && ui->find(module::notepad::kNotepadSearchBarId) != nullptr)
            passed = fail("预览态查找条不应渲染（§4.6 查找禁用）");
        if (passed) {
            const core::dsl::Element* minus =
                ui->find(module::notepad::kNotepadStepperFontMinusId);
            if (minus == nullptr || !minus->disabled)
                passed = fail("预览态 A± 未禁用（§4.6 字体/字号禁用）");
        }
        ms.previewOn = false;
        composeSession(ms);
        composeSession(ms);
        if (ui->find(module::notepad::kNotepadEditorInputId) == nullptr)
            passed = fail("切回编辑后编辑 input 未回归");
        if (passed && ui->find(module::notepad::kNotepadPreviewId) != nullptr)
            passed = fail("切回编辑后预览槽位未退场");
        if (passed) {
            const InputState& st =
                ui->state<InputState>(module::notepad::kNotepadEditorInputId);
            if (st.cursor != 6)
                passed = fail("切回后光标未保留（期望 6）: " +
                              std::to_string(st.cursor));
        }
    }

    // ── F. 换行钮（批 3 棒 C F-19：存在性 + 选中联动 + largeFile 禁用态）──
    if (passed) {
        // .txt 会话（E 段 session #5）：钮在场、可用、默认选中（prefs 缺省 wrap=true）
        module::notepad::NotepadSession& ts =
            *module::notepad::detail::sessionRegistry().back();
        composeSession(ts);
        composeSession(ts);  // ui 指针随最后 compose 的树走：先切回 ts 树再断言
        const core::dsl::Element* wrap =
            ui->find(module::notepad::kNotepadBtnWrapId);
        if (wrap == nullptr) passed = fail("换行钮 notepad.btnWrap 不在场");
        if (passed && wrap->disabled) passed = fail("普通 .txt 会话换行钮不应禁用");
        if (passed) {
            using InputState = components::input_detail::InputModel::InputState;
            const InputState& st =
                ui->state<InputState>(module::notepad::kNotepadEditorInputId);
            if (!st.softWrap)
                passed = fail("wrapOn=true 会话编辑 input softWrap 未随动（EUI 旋钮未接线）");
        }
        // 翻转 session.wrapOn → compose 同步：softWrap=false 随动（钮面态不读树，
        // session 字段是唯一真源；点击路径由 notepadToggleWrap 单点翻转）
        ts.wrapOn = false;
        composeSession(ts);
        composeSession(ts);
        if (passed) {
            using InputState = components::input_detail::InputModel::InputState;
            const InputState& st =
                ui->state<InputState>(module::notepad::kNotepadEditorInputId);
            if (st.softWrap)
                passed = fail("wrapOn=false 会话编辑 input softWrap 未随动");
        }
        ts.wrapOn = true;
    }
    if (passed) {
        // 大文件会话（>1MB 可编辑档）：load 强制 wrapOn=false + 钮禁用
        const std::string largePath = (dir / "large.txt").string();
        {
            std::ofstream lf(largePath, std::ios::binary);
            const std::string chunk(64, 'x');
            for (int i = 0; i < 18000; ++i) {  // ≈1.15MB > kNotepadLargeFileBytes
                lf << chunk << "\n";
            }
        }
        module::notepad::openNotepadWindow(largePath);
        if (module::notepad::notepadSessionCountForTest() != 6)
            passed = fail("大文件开窗后 session 数 != 6");
        if (passed) {
            module::notepad::NotepadSession& ls =
                *module::notepad::detail::sessionRegistry().back();
            if (!ls.largeFile) passed = fail("1.15MB 文件未进大文件模式");
            if (ls.wrapOn) passed = fail("大文件模式 wrapOn 未强制关（load 侧）");
            composeSession(ls);
            composeSession(ls);
            const core::dsl::Element* wrap =
                ui->find(module::notepad::kNotepadBtnWrapId);
            if (wrap == nullptr) passed = fail("大文件模式换行钮不在场（应禁用在场）");
            if (passed && !wrap->disabled)
                passed = fail("大文件模式换行钮未禁用");
            if (passed) {
                using InputState = components::input_detail::InputModel::InputState;
                const InputState& st =
                    ui->state<InputState>(module::notepad::kNotepadEditorInputId);
                if (st.softWrap)
                    passed = fail("大文件模式编辑 input softWrap 未关");
            }
        }
    }

    // ── G. 未保存保护链（批 4 棒 A F-05）：veto 裁决 + 三选弹窗树（三钮锚）+
    //    不保存→窗关 / 保存→落盘关窗 + quit guard 全链（拒退→处理→放行 /
    //    取消不卡死 / 全豁免放行，clue #269 三条穷尽路径）──
    // 注：onCloseRequested 闭包随 DslWindowConfig 走，探针 openWindow=入队不建窗
    //（app 桩），故 veto 语义以纯决策面 notepadCloseShouldIntercept 等价断言
    //（lambda 体 = 该函数 + protectOpen 置位两行）。
    if (passed) {
        // G1 关窗 veto：干净直通 / 脏拦截 + 三选弹窗树（关窗语境）
        module::notepad::NotepadSession& s =
            *module::notepad::detail::sessionRegistry().back();  // 大文件会话（可编辑档）
        composeSession(s);
        composeSession(s);
        if (module::notepad::notepadCloseShouldIntercept(s))
            passed = fail("G1: 干净窗被 veto 拦截（应直通放行）");
        module::notepad::sessionSetDirty(s, true);  // 保护链只读 session.dirty（批 3 基线比对单点产出）
        if (passed && !module::notepad::notepadCloseShouldIntercept(s))
            passed = fail("G1: 脏窗未拦截（onCloseRequested 应回 false）");
        // veto 回调置位镜像（lambda 两行等价）→ 该窗 Ui 树内弹三选
        s.protectOpen = true;
        s.protectQuitContext = false;
        composeSession(s);
        composeSession(s);
        if (ui->find(module::notepad::kNotepadProtectDialogId) == nullptr)
            passed = fail("G1: 三选弹窗 notepad.protectDialog 不在场");
        if (passed && ui->find(std::string(module::notepad::kNotepadProtectDialogId) + ".title") == nullptr)
            passed = fail("G1: 弹窗标题缺件");
        if (passed) {
            const core::dsl::Element* msg =
                ui->find(std::string(module::notepad::kNotepadProtectDialogId) + ".message");
            if (msg == nullptr || msg->text.find("有未保存的更改") == std::string::npos ||
                msg->text.find(s.title) == std::string::npos) {
                passed = fail("G1: 弹窗正文非「“文件名”有未保存的更改…」文案");
            }
        }
        const auto protectBtn = [&](const char* id) -> const core::dsl::Element* {
            const core::dsl::Element* el = ui->find(id);
            return (el != nullptr && el->onClick != nullptr)
                       ? el
                       : ui->find(std::string(id) + ".bg");
        };
        if (passed) {
            const core::dsl::Element* save = protectBtn(module::notepad::kNotepadProtectBtnSaveId);
            const core::dsl::Element* discard = protectBtn(module::notepad::kNotepadProtectBtnDiscardId);
            const core::dsl::Element* cancel = protectBtn(module::notepad::kNotepadProtectBtnCancelId);
            if (save == nullptr || discard == nullptr || cancel == nullptr)
                passed = fail("G1: 三钮（保存/不保存/取消）缺件");
            else if (save->disabled || discard->disabled || cancel->disabled)
                passed = fail("G1: 三钮在弹窗开启态被 disabled");
        }

        // G2 「不保存」：豁免旗标一次性置位 → veto 放行 → 窗关（session 计数减）
        if (passed) {
            const core::dsl::Element* discard =
                protectBtn(module::notepad::kNotepadProtectBtnDiscardId);
            if (discard == nullptr || discard->onClick == nullptr) {
                passed = fail("G2: 不保存钮不可点");
            } else {
                discard->onClick();  // 关窗语境：置豁免 + 请求关窗（探针无实窗句柄，跳过）
            }
            if (passed && !s.skipCloseConfirm)
                passed = fail("G2: 不保存后豁免旗标未置位");
            if (passed && s.protectOpen)
                passed = fail("G2: 不保存后弹窗未关");
            if (passed && module::notepad::notepadCloseShouldIntercept(s))
                passed = fail("G2: 豁免后关窗仍被拦截（窗应关闭）");
            // 窗销毁注销单点（handle 空 = 几何落库跳过，仅注销）→ 计数减
            module::notepad::notepadSessionWindowDestroyed(&s);
            if (passed && module::notepad::notepadSessionCountForTest() != 5)
                passed = fail("G2: 不保存关窗后 session 计数未减（≠5）");
        }

        // G3 「保存」：落盘成功 → dirty 清零 → 关窗语境放行（文件内容核对）
        if (passed) {
            module::notepad::NotepadSession& ms =
                *module::notepad::detail::sessionRegistry().front();  // note.md
            ms.editText = "edited body\n";
            module::notepad::sessionSetDirty(ms, true);
            ms.protectOpen = true;
            ms.protectQuitContext = false;
            composeSession(ms);
            composeSession(ms);
            const core::dsl::Element* save = protectBtn(module::notepad::kNotepadProtectBtnSaveId);
            if (save == nullptr || save->onClick == nullptr) {
                passed = fail("G3: 保存钮不可点");
            } else {
                save->onClick();
            }
            if (passed && ms.dirty)
                passed = fail("G3: 保存后 dirty 未清零");
            if (passed && ms.protectOpen)
                passed = fail("G3: 保存后弹窗未关");
            std::ifstream check(notePath, std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(check)),
                             std::istreambuf_iterator<char>());
            if (passed && body != "edited body\n")
                passed = fail("G3: 保存落盘内容不符: " + body);
        }

        // G4 quit guard 全链（D-3 逐窗 + clue #269 再入队语义）
        if (passed) {
            module::notepad::NotepadSession& js =
                *module::notepad::detail::sessionRegistry()[3];   // cfg.json
            module::notepad::NotepadSession& ts =
                *module::notepad::detail::sessionRegistry()[4];   // plain.txt
            module::notepad::sessionSetDirty(js, true);
            module::notepad::sessionSetDirty(ts, true);
            // 退出请求 #1：guard 拦截，对首个脏窗（创建序）弹三选
            if (module::notepad::notepadQuitGuard())
                passed = fail("G4: 有脏窗时 guard 放行（应拦截）");
            if (passed && !js.protectOpen)
                passed = fail("G4: guard 未对首个脏窗弹三选");
            if (passed && ts.protectOpen)
                passed = fail("G4: guard 叠开第二张弹窗（应逐窗串行）");
            composeSession(js);
            composeSession(js);
            // 「保存」成功 → rearmExit（clue #269：模块处理完再调 requestExit 重新置位）
            const core::dsl::Element* save = protectBtn(module::notepad::kNotepadProtectBtnSaveId);
            if (save == nullptr || save->onClick == nullptr) {
                passed = fail("G4: 退出语境保存钮不可点");
            } else {
                save->onClick();
            }
            if (passed && !app::detail::consumeExitRequest())
                passed = fail("G4: 保存后未 requestExit 再入队（guard 链断头）");
            // 退出请求 #2：json 已干净 → 跳到下一脏窗 plain.txt
            if (passed && module::notepad::notepadQuitGuard())
                passed = fail("G4: 仍有脏窗时 guard 放行");
            if (passed && !ts.protectOpen)
                passed = fail("G4: guard 未继续下一脏窗");
            // 「不保存」→ 豁免 + 再入队 → 全豁免放行（穷尽路径①）
            composeSession(ts);
            composeSession(ts);
            const core::dsl::Element* discard = protectBtn(module::notepad::kNotepadProtectBtnDiscardId);
            if (discard == nullptr || discard->onClick == nullptr) {
                passed = fail("G4: 退出语境不保存钮不可点");
            } else {
                discard->onClick();
            }
            if (passed && !ts.skipCloseConfirm)
                passed = fail("G4: 退出语境不保存未置豁免");
            if (passed && !app::detail::consumeExitRequest())
                passed = fail("G4: 不保存后未 requestExit 再入队");
            if (passed && !module::notepad::notepadQuitGuard())
                passed = fail("G4: 全豁免后 guard 未放行（穷尽路径①断链）");
            if (passed && !ts.skipCloseConfirm)
                passed = fail("G4: guard 扫描消费了豁免旗标（应保留到窗关）");

            // 取消场景：拒退后可正常退出 + 不卡死（D-3 中止整个退出）
            module::notepad::NotepadSession& ms =
                *module::notepad::detail::sessionRegistry().front();
            module::notepad::sessionSetDirty(ms, true);
            if (passed && module::notepad::notepadQuitGuard())
                passed = fail("G4: 取消场景 guard 未拦截");
            composeSession(ms);
            composeSession(ms);
            const core::dsl::Element* cancel = protectBtn(module::notepad::kNotepadProtectBtnCancelId);
            if (cancel == nullptr || cancel->onClick == nullptr) {
                passed = fail("G4: 取消钮不可点");
            } else {
                cancel->onClick();
            }
            if (passed && ms.protectOpen)
                passed = fail("G4: 取消后弹窗未关");
            if (passed && app::detail::consumeExitRequest())
                passed = fail("G4: 取消后仍 rearm（应中止整个退出）");
            // 取消不卡死：再次退出请求可重进链（重新弹三选），随后「不保存」放行
            if (passed && module::notepad::notepadQuitGuard())
                passed = fail("G4: 取消后重入链未拦截");
            if (passed && !ms.protectOpen)
                passed = fail("G4: 重入链未重新弹三选（不卡死）");
            composeSession(ms);
            composeSession(ms);
            const core::dsl::Element* discard2 = protectBtn(module::notepad::kNotepadProtectBtnDiscardId);
            if (passed && (discard2 == nullptr || discard2->onClick == nullptr)) {
                passed = fail("G4: 重入链不保存钮不可点");
            } else if (passed) {
                discard2->onClick();
            }
            if (passed && !app::detail::consumeExitRequest())
                passed = fail("G4: 重入链不保存后未再入队");
            if (passed && !module::notepad::notepadQuitGuard())
                passed = fail("G4: 拒退一次后无法正常退出（穷尽路径②断链）");
        }
    }

    // ── H. F-04 另存为 + 快捷键接线（need289 修复棒 1）：另存为钮存在+禁用
    //    矩阵（可编辑=可用 / 只读与错误窗=禁用，§7.2）+ M+Shift+S 键路由守卫
    //    （只读窗直驱零副作用——守卫若缺失会弹真保存对话框挂起 = 超时红，故
    //    可编辑窗禁直驱，对话框面归真机 QA）+ M+H 替换条直开 / M+Shift+F
    //    格式化置位-消费 / M+P·M+Shift+V 预览切换
    if (passed) {
        const auto pressKey = [&](module::notepad::NotepadSession& s,
                                  eui::InputKey key, bool withShortcut,
                                  bool withShift) {
            eui::KeyEvent ev;
            ev.key = key;
            ev.action = core::KeyAction::Press;
            ev.modifiers.control = withShortcut;
            ev.modifiers.super = withShortcut;
            ev.modifiers.shift = withShift;
            module::notepad::notepadHandleKeySession(s, ev);
        };
        module::notepad::NotepadSession& ms =
            *module::notepad::detail::sessionRegistry().front();  // note.md（可编辑）
        module::notepad::NotepadSession& rs =
            *module::notepad::detail::sessionRegistry()[1];       // utf16（只读）
        module::notepad::NotepadSession& es =
            *module::notepad::detail::sessionRegistry()[2];       // ghost（错误占位）
        module::notepad::NotepadSession& js =
            *module::notepad::detail::sessionRegistry()[3];       // cfg.json
        module::notepad::NotepadSession& ts =
            *module::notepad::detail::sessionRegistry().back();   // plain.txt

        // H1 另存为钮存在 + 禁用矩阵
        composeSession(ms);
        composeSession(ms);
        const core::dsl::Element* saveAs =
            ui->find(module::notepad::kNotepadBtnSaveAsId);
        if (saveAs == nullptr)
            passed = fail("另存为钮 notepad.btnSaveAs 不在场（F-04）");
        if (passed && saveAs->disabled)
            passed = fail("可编辑窗另存为钮不应禁用");
        composeSession(rs);
        composeSession(rs);
        saveAs = ui->find(module::notepad::kNotepadBtnSaveAsId);
        if (passed && (saveAs == nullptr || !saveAs->disabled))
            passed = fail("只读窗另存为钮未禁用（§7.2 矩阵）");
        composeSession(es);
        composeSession(es);
        saveAs = ui->find(module::notepad::kNotepadBtnSaveAsId);
        if (passed && (saveAs == nullptr || !saveAs->disabled))
            passed = fail("错误窗另存为钮未禁用（§7.2 矩阵）");

        // H2 M+Shift+S 键路由守卫：只读/错误窗直驱零副作用（可编辑窗禁直驱）
        const std::string roPath = rs.path;
        const std::string ghostPathBefore = es.path;
        pressKey(rs, eui::InputKey::S, true, true);
        if (rs.path != roPath || rs.dirty)
            passed = fail("M+Shift+S 只读窗未守卫拦截（应零副作用）");
        pressKey(es, eui::InputKey::S, true, true);
        if (es.path != ghostPathBefore || es.dirty)
            passed = fail("M+Shift+S 错误窗未守卫拦截（应零副作用）");

        // H3 M+H 替换条直开（D3）：开条 + 替换行展开 + 替换输入在场
        composeSession(ts);
        composeSession(ts);
        pressKey(ts, eui::InputKey::H, true, false);
        composeSession(ts);
        composeSession(ts);
        if (ui->find(module::notepad::kNotepadSearchBarId) == nullptr)
            passed = fail("M+H 后查找条不在场（替换直开断链）");
        if (passed && !ts.find.replaceOpen)
            passed = fail("M+H 未直开替换行（replaceOpen 未展开）");
        if (passed && ui->find(module::notepad::kNotepadSearchReplaceInputId) == nullptr)
            passed = fail("M+H 后替换输入框不在场");
        pressKey(ts, eui::InputKey::Escape, false, false);  // 收条还原
        composeSession(ts);

        // H4 M+Shift+F 格式化：.txt 不置位 / .json 置位 → compose 消费 pretty
        pressKey(ts, eui::InputKey::F, true, true);
        if (ts.pendingFormatJson)
            passed = fail("M+Shift+F 非 .json 会话不应置位");
        pressKey(js, eui::InputKey::F, true, true);
        if (passed && !js.pendingFormatJson)
            passed = fail("M+Shift+F .json 可编辑窗未置位 pendingFormatJson");
        composeSession(js);
        composeSession(js);
        if (passed && js.pendingFormatJson)
            passed = fail("pendingFormatJson 未被 compose 消费");
        if (passed && js.editText.find('\n') == std::string::npos)
            passed = fail("M+Shift+F 后编辑镜像未变 pretty（格式化未落地）");

        // H5 M+P / M+Shift+V 预览切换：.md 翻转两轮 / .txt 不动作
        pressKey(ms, eui::InputKey::P, true, false);
        if (passed && !ms.previewOn)
            passed = fail("M+P 未切进预览（.md 会话）");
        pressKey(ms, eui::InputKey::P, true, false);
        if (passed && ms.previewOn)
            passed = fail("M+P 未切回编辑");
        pressKey(ms, eui::InputKey::V, true, true);
        if (passed && !ms.previewOn)
            passed = fail("M+Shift+V 未切进预览（PRD §九）");
        pressKey(ms, eui::InputKey::V, true, true);
        if (passed && ms.previewOn)
            passed = fail("M+Shift+V 未切回编辑");
        pressKey(ts, eui::InputKey::P, true, false);
        if (passed && ts.previewOn)
            passed = fail("M+P 非 .md 会话不应切预览");
        pressKey(ts, eui::InputKey::V, true, true);
        if (passed && ts.previewOn)
            passed = fail("M+Shift+V 非 .md 会话不应切预览");
    }

    // ── I. 修复棒 2 面（need289 P2-4/P2-5/P3-5/P3-7）：字体弹层 scrim/Esc 收起
    //    + 状态栏统计缓存键随动 + 大文件提示条 info 档可关 + .json 换行钮让位 ──
    if (passed) {
        const auto pressKey = [&](module::notepad::NotepadSession& s,
                                  eui::InputKey key) {
            eui::KeyEvent ev;
            ev.key = key;
            ev.action = core::KeyAction::Press;
            module::notepad::notepadHandleKeySession(s, ev);
        };
        module::notepad::NotepadSession& ts =
            *module::notepad::detail::sessionRegistry().back();   // plain.txt
        module::notepad::NotepadSession& js =
            *module::notepad::detail::sessionRegistry()[3];       // cfg.json
        const std::string scrimId =
            std::string(module::notepad::kNotepadDdFontId) + ".popup.scrim";

        // I1 字体弹层外点/Esc 收起（P2-4，设计稿 §5.2「收起不改值」）
        composeSession(ts);
        composeSession(ts);
        const core::dsl::Element* trigger =
            ui->find(module::notepad::kNotepadDdFontId);
        if (passed && (trigger == nullptr || trigger->onClick == nullptr))
            passed = fail("I1: 字体触发器不可点");
        else if (passed) trigger->onClick();
        composeSession(ts);
        composeSession(ts);
        const core::dsl::Element* scrim = ui->find(scrimId);
        if (passed && (scrim == nullptr || scrim->onClick == nullptr))
            passed = fail("I1: 弹层开启后全窗遮罩点击层不在场（P2-4）");
        else if (passed) scrim->onClick();
        composeSession(ts);
        composeSession(ts);
        if (passed && ui->find(scrimId) != nullptr)
            passed = fail("I1: 遮罩点击后字体弹层未收起（P2-4）");
        if (passed) {
            const core::dsl::Element* trigger2 =
                ui->find(module::notepad::kNotepadDdFontId);
            if (trigger2 == nullptr || trigger2->onClick == nullptr)
                passed = fail("I1: 二次开弹层触发器不可点");
            else trigger2->onClick();
        }
        composeSession(ts);
        composeSession(ts);
        if (passed && ui->find(scrimId) == nullptr)
            passed = fail("I1: 二次开弹层遮罩未出现");
        pressKey(ts, eui::InputKey::Escape);
        composeSession(ts);
        composeSession(ts);
        if (passed && ui->find(scrimId) != nullptr)
            passed = fail("I1: Esc 未收起字体弹层（P2-4）");

        // I2 状态栏统计缓存键随动（P2-5）：cursor 变化 → 行列重算；
        // textRevision 变化 → 字符数重算（镜像同步 = value 相等不触发回置）
        if (passed) {
            using InputState = components::input_detail::InputModel::InputState;
            InputState& st =
                ui->state<InputState>(module::notepad::kNotepadEditorInputId);
            st.cursor = 6;  // "plain text\n" 第 7 字节 → 行 1, 列 7
            composeSession(ts);
            composeSession(ts);
            const core::dsl::Element* cursorEl = ui->find("notepad.status.cursor");
            if (cursorEl == nullptr || cursorEl->text != "行 1, 列 7")
                passed = fail("I2: 光标移动后行列未重算（P2-5 缓存键）: " +
                              (cursorEl ? cursorEl->text : "(缺件)"));
            st.text = "abc";
            ++st.textRevision;
            ts.editText = "abc";  // 镜像同步（value 相等跳过回置）
            composeSession(ts);
            composeSession(ts);
            const core::dsl::Element* charsEl = ui->find("notepad.status.chars");
            if (charsEl == nullptr || charsEl->text != "3 字符")
                passed = fail("I2: 文本修订后字符数未重算（P2-5 缓存键）: " +
                              (charsEl ? charsEl->text : "(缺件)"));
        }

        // I3 .json 窗换行钮紧跟格式化钮（P3-7：不留预览位空洞）
        composeSession(js);
        composeSession(js);
        if (passed) {
            const core::dsl::Element* wrap =
                ui->find(module::notepad::kNotepadBtnWrapId);
            if (wrap == nullptr || wrap->x != 564.0f)
                passed = fail("I3: .json 窗换行钮未在 x=564（P3-7 布局空洞）: " +
                              std::to_string(wrap ? wrap->x : -1.0f));
        }

        // I4 大文件提示条 info 档可关（P3-5：只读档仍 warning 不可关对照见 B 段）
        const std::string largePath2 = (dir / "large2.txt").string();
        {
            std::ofstream lf(largePath2, std::ios::binary);
            const std::string chunk(64, 'x');
            for (int i = 0; i < 18000; ++i) {
                lf << chunk << "\n";
            }
        }
        module::notepad::openNotepadWindow(largePath2);
        if (passed) {
            module::notepad::NotepadSession& ls =
                *module::notepad::detail::sessionRegistry().back();
            composeSession(ls);
            composeSession(ls);
            if (ui->find("notepad.promptbar") == nullptr)
                passed = fail("I4: 大文件提示条不在场");
            const core::dsl::Element* closeBtn =
                ui->find("notepad.promptbar.close");
            if (passed && (closeBtn == nullptr || closeBtn->onClick == nullptr))
                passed = fail("I4: 大文件提示条关闭钮不在场（P3-5 info 档可关）");
            else if (passed) closeBtn->onClick();
            composeSession(ls);
            composeSession(ls);
            if (passed && ui->find("notepad.promptbar") != nullptr)
                passed = fail("I4: 关闭钮点击后提示条未收起（P3-5）");
        }
    }

    // ── J. bug399：预览滚动后底部大片区空白（裁剪范围冻结在末次 compose 的滚动值）──
    // 复现：老板真机 2026-11-21 — 打开 3.5KB/43 行 md → 预览 → 滚到底，出现大片
    // 白板、底部内容不再显示。根因：EUI 滚轮不置 composeRequested（
    // runtime_update.h setScrollOffset 仅当滚动 owner 带 composeOnScrollOffsetChange
    // 才置位），notepad 预览 scrollView 未开该标志且 onChange 只回写偏移 →
    // visibleRange（bug382 视口裁剪）冻结在末次 compose 的 scroll 值，滚出 ≥1200px
    // overscan 后新暴露区只剩空包装元素。本段按**应用主循环语义**驱动
    // （dsl_app_impl.h:617-634：仅首帧/尺寸变/composeRequested 才 compose）——
    // 探针常规的逐帧无条件 compose 会把该缺陷掩盖成假绿。夹具 =
    // tests/ui_driver/fixtures/bug399-invoice-prd.md（老板复现文件原样副本）。
    {  // J 段独立于前段 passed：HEAD 已有 P2-3 无关红（见汇报），不因它跳过本段
        bool jOk = true;
        std::filesystem::path repoRoot;
        for (std::filesystem::path p = std::filesystem::current_path(); !p.empty();
             p = p.parent_path()) {
            if (std::filesystem::exists(
                    p / "tests/ui_driver/fixtures/bug399-invoice-prd.md")) {
                repoRoot = p;
                break;
            }
            if (!p.has_parent_path()) break;
        }
        if (repoRoot.empty()) {
            jOk = fail("J: 定位不到仓库根（tests/ui_driver/fixtures/bug399-invoice-prd.md）");
        } else {
            module::notepad::openNotepadWindow(
                (repoRoot / "tests/ui_driver/fixtures/bug399-invoice-prd.md").string());
            module::notepad::NotepadSession& ps =
                *module::notepad::detail::sessionRegistry().back();
            ps.previewOn = true;
            // 记事本最小窗档（模块规格 720×480 的最小高，取 900×480 组合）：
            // 预览视口比默认 640 档更矮 = 更早暴露裁剪冻结（真实可达尺寸，非合成）。
            const float jw = 720.0f;
            const float jh = 480.0f;
            int composeCount = 0;
            auto composeApp = [&] {
                runtime.compose(ps.pageId, jw, jh,
                                [&](eui::Ui& u, const eui::Screen& screen) {
                                    ui = &u;
                                    module::notepad::notepadComposeSession(ps, u, screen);
                                });
                ++composeCount;
            };
            auto appFrame = [&] {  // 应用主循环一帧：update → 按需 compose
                probe::pollOnce(window, runtime);
                if (runtime.composeRequested()) {
                    composeApp();
                    probe::pollOnce(window, runtime);
                }
            };
            composeApp();
            composeApp();  // 首帧建树 + 布局稳态
            const core::dsl::Element* sc = ui->find(module::notepad::kNotepadPreviewId);
            const float vpH = sc != nullptr ? sc->frame.height : 0.0f;
            const float maxOff = sc != nullptr ? sc->scrollMaxOffset : 0.0f;
            const core::dsl::Element* wrap =
                ui->find(std::string(module::notepad::kNotepadPreviewContentId) + ".wrap");
            const float docH = wrap != nullptr ? wrap->frame.height : 0.0f;
            if (sc == nullptr || maxOff <= 0.0f) {
                jOk = fail("J: 预览不可滚动（maxOffset=" + std::to_string(maxOff) +
                              "）——夹具/窗高不满足复现前件");
            }
            // 像素证据链（bug399 round2）：scroll=0 对照帧 + 滚到底帧。各 16 遍
            // render 跑满 retained 层稳定门槛（否则探针冷层单遍捕获整层空白=
            // 假证据）。主内容区取预览元素逻辑框，按 framebuffer/scale 换像素。
            probe::FrameCapture frameCap(window);
            auto captureInk = [&](const std::string& path, const std::string& id) -> int {
                // 元素运行时实例（frame/transform/text 实例）由 update 建，
                // 不 poll 直接 render 会读到空实例 = 整帧不画（假空白）。
                for (int i = 0; i < 4; ++i) probe::pollOnce(window, runtime);
                std::vector<unsigned char> rgba;
                int iw = 0;
                int ih = 0;
                if (!frameCap.ok() || !frameCap.readFrame(runtime, rgba, iw, ih, 24)) {
                    std::cerr << "J: 读帧失败（"
                              << (frameCap.ok() ? "readFrame" : frameCap.error()) << "）"
                              << "\n";
                    return -1;
                }
                if (!probe::writeRgbaToPng(path, rgba.data(), iw, ih)) {
                    std::cerr << "J: 写 PNG 失败: " << path << "\n";
                    return -1;
                }
                const core::dsl::Element* cur = ui->find(id);
                if (cur == nullptr) return -1;
                const int ink = countInkPixels(rgba, iw, ih, frameCap.scale(),
                                               cur->frame.x, cur->frame.y,
                                               cur->frame.width, cur->frame.height);
                const int frameInk = countInkPixels(
                    rgba, iw, ih, frameCap.scale(), 0.0f, 0.0f,
                    static_cast<float>(iw) / frameCap.scale(),
                    static_cast<float>(ih) / frameCap.scale());
                std::cerr << "J 像素: " << path << " ink=" << ink
                          << " frameInk=" << frameInk << "\n";
                return ink;
            };
            const int inkTop = jOk ? captureInk("evidence/bug399/round4-scroll0.png", module::notepad::kNotepadPreviewId) : -1;
            if (jOk && inkTop < 0) jOk = fail("J: scroll=0 对照帧读取失败");
            else if (jOk && inkTop <= 200)
                jOk = fail("J: scroll=0 对照帧内容区无文字像素（ink=" +
                           std::to_string(inkTop) + "）= 绘制链本身不落像素，先查渲染侧");

            // 滚轮注入（驱动 execScroll 同款时序）：指针 motion 独立成帧落地 →
            // 注入 scroll → 惯性收尾帧带真实短 sleep（实机节奏）。事件数取 8 轮
            // （单轮 -20 约 600px，远超 maxOffset）——到底后溢出冲量自行归零，
            // 不依赖回读（element->scrollOffset 是 compose 输入值，无重排不回写）。
            if (jOk) {
                const float cx = sc->frame.x + sc->frame.width * 0.5f;
                const float cy = sc->frame.y + sc->frame.height * 0.5f;
                for (int i = 0; i < 8; ++i) {
                    core::queuePointerMotion(window, cx, cy, {}, {});
                    probe::pollOnce(window, runtime);
                    probe::queueScroll(window, runtime, 0.0, -20.0);
                    probe::pollOnce(window, runtime);
                    for (int f = 0; f < 40; ++f) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(16));
                        appFrame();
                    }
                }
            }
            const int inkBottom = jOk ? captureInk("evidence/bug399/round4-scrolled.png", module::notepad::kNotepadPreviewId) : -1;
            // bug399 round9 / debt398 P2②：白板根因已修（kNotepadPreviewContentId
            // 改名不再与 markdown 根同 id），此处不再打印「已知红记录」误导行；
            // 失败统一走下方 fail()。
            // bug399 round4 判据：滚动后内容区文字像素必须与 scroll=0 同量级
            // （根因 = 滚动内容列与 markdown 根同 id，instances_ 同 key 被洗回 0）。
            if (jOk && inkBottom <= 200)
                jOk = fail("J: 滚到底后内容区无文字像素（ink=" + std::to_string(inkBottom) +
                           "）= 像素级白板");
            // 判据：滚到底后文档末块的 .text 子树必须已建（裁剪跟随滚动）。修前
            // 裁剪冻结 → 末块在 overscan 外只剩包装 → 缺件 = 白板。
            {
                int lastIdx = -1;
                float lastY = 0.0f;
                float lastH = 0.0f;
                for (int i = 0; i < 2000; ++i) {
                    const core::dsl::Element* blk = ui->find(
                        std::string(module::notepad::kNotepadPreviewContentId) +
                        ".block." + std::to_string(i));
                    if (blk == nullptr) continue;
                    lastIdx = i;
                    lastY = blk->frame.y;
                    lastH = blk->frame.height;
                }
                std::cerr << "J 诊断: viewportH=" << vpH << " docH=" << docH
                          << " maxOffset=" << maxOff
                          << " lastBlock=" << lastIdx << " y=" << lastY
                          << " h=" << lastH
                          << " 滚动期 compose=" << (composeCount - 2) << "\n";
                if (lastIdx < 0) {
                    jOk = fail("J: 整篇块包装都不在场（visibleRange 契约破坏）");
                } else if (ui->find(std::string(module::notepad::kNotepadPreviewContentId) +
                                   ".block." + std::to_string(lastIdx) + ".text") == nullptr) {
                    jOk = fail("J: 滚到底后末块 block." + std::to_string(lastIdx) +
                               " 无 .text 内容（裁剪范围未随滚动重排 = bug399 白板）");
                }
                if (jOk) {
                    std::cerr << "J: PASS — bug399 滚动裁剪跟随滚动（末块 .text 已建）"
                              << "\n";
                } else {
                    passed = false;
                }
            }
        }
    }

    // ── K. bug399 round3 差分实验：普通 scrollView + ui.text，无流质帧泵 ──
    // 目的：判定「render-only 滚动下文本不落像素」是 EUI 通用缺陷还是 notepad 预览
    // 特有。同套 no-pump 循环（仅 composeRequested 重排；本用例不开
    // composeOnScrollOffsetChange = 纯 render-only 滚动）+ 真实滚轮注入，滚后读像素。
    {
        const std::string diffId = "diff.scroll";
        float diffOffset = 0.0f;
        core::dsl::Ui* kUi = nullptr;
        int kComposes = 0;
        auto kCompose = [&] {
            runtime.compose("diff", 720.0f, 480.0f, [&](eui::Ui& u, const eui::Screen&) {
                kUi = &u;
                components::scrollView(u, diffId)
                    .position(0.0f, 0.0f)
                    .size(720.0f, 480.0f)
                    .offset(diffOffset)
                    .onChange([&diffOffset](float v) { diffOffset = v; })
                    .contentKey("v1")
                    .content([&diffId](eui::Ui& inner, float innerW, float) {
                        for (int i = 0; i < 40; ++i) {
                            inner.text(diffId + ".t" + std::to_string(i))
                                .width(innerW)
                                .height(24.0f)
                                .text("diff line " + std::to_string(i))
                                .fontSize(15.0f)
                                .build();
                        }
                    })
                    .build();
            });
            ++kComposes;
        };
        auto kFrame = [&] {
            probe::pollOnce(window, runtime);
            if (runtime.composeRequested()) {
                kCompose();
                probe::pollOnce(window, runtime);
            }
        };
        auto kCapture = [&](const std::string& path) -> int {
            for (int i = 0; i < 4; ++i) probe::pollOnce(window, runtime);
            probe::FrameCapture capture(window);
            std::vector<unsigned char> rgba;
            int iw = 0;
            int ih = 0;
            if (!capture.ok() || !capture.readFrame(runtime, rgba, iw, ih, 24)) return -1;
            probe::writeRgbaToPng(path, rgba.data(), iw, ih);
            const core::dsl::Element* sc = kUi != nullptr ? kUi->find(diffId) : nullptr;
            if (sc == nullptr) return -1;
            return countInkPixels(rgba, iw, ih, capture.scale(), sc->frame.x, sc->frame.y,
                                  sc->frame.width, sc->frame.height);
        };
        kCompose();
        kCompose();
        const core::dsl::Element* sc = kUi->find(diffId);
        const float kMax = sc != nullptr ? sc->scrollMaxOffset : 0.0f;
        const int kInk0 = kCapture("evidence/bug399/round3-diff-scroll0.png");
        if (sc != nullptr && kMax > 0.0f) {
            const float kcx = sc->frame.x + sc->frame.width * 0.5f;
            const float kcy = sc->frame.y + sc->frame.height * 0.5f;
            for (int i = 0; i < 8; ++i) {
                core::queuePointerMotion(window, kcx, kcy, {}, {});
                probe::pollOnce(window, runtime);
                probe::queueScroll(window, runtime, 0.0, -20.0);
                probe::pollOnce(window, runtime);
                for (int f = 0; f < 40; ++f) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                    kFrame();
                }
            }
        }
        const int kInk1 = kCapture("evidence/bug399/round3-diff-scrolled.png");
        std::cerr << "K 差分: maxOffset=" << kMax << " scroll0 ink=" << kInk0
                  << " scrolled ink=" << kInk1 << " composes=" << kComposes
                  << "\n";
        std::cerr << (kInk1 > 200 ? "K 结论: 普通 scrollView+text 滚后仍有像素 = 缺陷是"
                                  " notepad 预览特有（对比元素/实例差异）"
                                : "K 结论: 普通 scrollView+text 滚后也归零 = render-only"
                                  " 滚动的文本链通用缺陷（EUI 侧）")
                  << "\n";
    }

    // ── L. bug399 round5：53KB/581 行真文件预览性能（debt398 汇流）──
    // 计时口径：单次 runtime.compose（真窗口 900×640）的墙钟；首帧含解析+内联测高
    // 预热，滚动段只统计滚动触发的重排。阈值对齐旧查看器同量级（每格 ≤40ms 上限，
    // 目标 16-30ms 档；滚动全程 8 格累计 ≤1200ms）。
    {
        std::filesystem::path repoRoot;
        for (std::filesystem::path p = std::filesystem::current_path(); !p.empty();
             p = p.parent_path()) {
            if (std::filesystem::exists(p / "tests/ui_driver/fixtures/bug399-git-redesign.md")) {
                repoRoot = p;
                break;
            }
            if (!p.has_parent_path()) break;
        }
        if (repoRoot.empty()) {
            passed = fail("L: 定位不到仓库根（tests/ui_driver/fixtures/bug399-git-redesign.md）");
        } else {
            module::notepad::openNotepadWindow(
                (repoRoot / "tests/ui_driver/fixtures/bug399-git-redesign.md").string());
            module::notepad::NotepadSession& ps =
                *module::notepad::detail::sessionRegistry().back();
            ps.previewOn = true;
            const float lw = 900.0f;
            const float lh = 640.0f;
            double maxCompMs = 0.0;
            double totalCompMs = 0.0;
            int compCount = 0;
            auto timedCompose = [&] {
                const auto t0 = std::chrono::steady_clock::now();
                runtime.compose(ps.pageId, lw, lh,
                                [&](eui::Ui& u, const eui::Screen& screen) {
                                    ui = &u;
                                    module::notepad::notepadComposeSession(ps, u, screen);
                                });
                const double ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - t0).count();
                ++compCount;
                maxCompMs = std::max(maxCompMs, ms);
                totalCompMs += ms;
                return ms;
            };
            const double firstMs = timedCompose();
            probe::pollOnce(window, runtime);
            maxCompMs = 0.0;
            totalCompMs = 0.0;
            compCount = 0;
            const auto scrollT0 = std::chrono::steady_clock::now();
            const core::dsl::Element* sc = ui->find(module::notepad::kNotepadPreviewId);
            if (sc != nullptr) {
                const float cx = sc->frame.x + sc->frame.width * 0.5f;
                const float cy = sc->frame.y + sc->frame.height * 0.5f;
                for (int i = 0; i < 8; ++i) {
                    core::queuePointerMotion(window, cx, cy, {}, {});
                    probe::pollOnce(window, runtime);
                    probe::queueScroll(window, runtime, 0.0, -20.0);
                    probe::pollOnce(window, runtime);
                    for (int f = 0; f < 8; ++f) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(16));
                        probe::pollOnce(window, runtime);
                        if (runtime.composeRequested()) {
                            timedCompose();
                            probe::pollOnce(window, runtime);
                        }
                    }
                }
            }
            const double scrollMs = std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - scrollT0).count();
            std::cerr << "L 计时: 首次compose=" << firstMs << "ms 每格最大=" << maxCompMs
                      << "ms 滚动期compose均值="
                      << (compCount > 0 ? totalCompMs / compCount : 0.0) << "ms 次数=" << compCount
                      << " 8格累计=" << scrollMs << "ms" << "\n";
            if (maxCompMs > 40.0)
                std::cerr << "L: 已知红记录 — 每格 compose " << maxCompMs
                          << "ms 超 40ms 上限（debt398 性能债）" << "\n";
        }
    }

    // ── M. bug399 round7：性能仪表分解 + A/B 基准（debt398 汇流）──
    // 每帧 compose 的 markdown 内部构成（parse 查找/解析、inline 键构造/查找/
    // 布局、build 走查、裁剪块数、kind 计数）与旧查看器组件级形态（整篇
    // components::markdown，无 scrollView/裁剪/估高包装）的 compose 基准。
    {
        std::filesystem::path repoRoot;
        for (std::filesystem::path p = std::filesystem::current_path(); !p.empty();
             p = p.parent_path()) {
            if (std::filesystem::exists(p / "tests/ui_driver/fixtures/bug399-git-redesign.md")) {
                repoRoot = p;
                break;
            }
            if (!p.has_parent_path()) break;
        }
        if (repoRoot.empty()) {
            std::cerr << "M: 定位不到夹具（跳过仪表段）\n";
        } else {
            const std::string fixturePath =
                (repoRoot / "tests/ui_driver/fixtures/bug399-git-redesign.md").string();
            const auto dump = [](const char* tag, double totalMs,
                                 const components::detail::MarkdownPerfStats& b) {
                const components::detail::MarkdownPerfStats& p =
                    components::detail::markdownPerfStats();
                std::cerr << "M " << tag << ": compose=" << totalMs
                          << "ms | md build=" << (p.buildMs - b.buildMs)
                          << "ms calls=" << (p.buildCalls - b.buildCalls)
                          << " blocks=" << (p.blocksVisited - b.blocksVisited)
                          << " culled=" << (p.blocksCulled - b.blocksCulled)
                          << " | parse hit=" << (p.parseHits - b.parseHits)
                          << " miss=" << (p.parseMisses - b.parseMisses)
                          << " lookup=" << (p.parseLookupMs - b.parseLookupMs)
                          << " build=" << (p.parseBuildMs - b.parseBuildMs)
                          << "ms | inline key=" << (p.inlineKeyMs - b.inlineKeyMs)
                          << "ms lookup=" << (p.inlineLookupMs - b.inlineLookupMs)
                          << "ms layout=" << (p.inlineLayoutMs - b.inlineLayoutMs)
                          << "ms hit=" << (p.inlineHits - b.inlineHits)
                          << " miss=" << (p.inlineMisses - b.inlineMisses)
                          << " | kinds h=" << (p.kindHeading - b.kindHeading)
                          << " p=" << (p.kindParagraph - b.kindParagraph)
                          << " li=" << (p.kindListItem - b.kindListItem)
                          << " code=" << (p.kindCode - b.kindCode)
                          << " html=" << (p.kindHtml - b.kindHtml)
                          << " div=" << (p.kindDivider - b.kindDivider)
                          << " tr=" << (p.kindTableRow - b.kindTableRow)
                          << " | caches doc=" << p.docCacheEntries
                          << " inline=" << p.inlineCacheEntries << "/" << p.inlineCacheBytes
                          << "B\n";
            };
            module::notepad::openNotepadWindow(fixturePath);
            module::notepad::NotepadSession& ms =
                *module::notepad::detail::sessionRegistry().back();
            ms.previewOn = true;
            const float mw = 900.0f;
            const float mh = 640.0f;
            auto timedM = [&] {
                const auto t0 = std::chrono::steady_clock::now();
                runtime.compose(ms.pageId, mw, mh,
                                [&](eui::Ui& u, const eui::Screen& screen) {
                                    ui = &u;
                                    module::notepad::notepadComposeSession(ms, u, screen);
                                });
                return std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - t0).count();
            };
            {
                const auto before = components::detail::markdownPerfStats();
                const double firstMs = timedM();
                probe::pollOnce(window, runtime);
                dump("首帧", firstMs, before);
            }
            const core::dsl::Element* msc = ui->find(module::notepad::kNotepadPreviewId);
            if (msc != nullptr) {
                const float cx = msc->frame.x + msc->frame.width * 0.5f;
                const float cy = msc->frame.y + msc->frame.height * 0.5f;
                for (int i = 0; i < 4; ++i) {
                    core::queuePointerMotion(window, cx, cy, {}, {});
                    probe::pollOnce(window, runtime);
                    probe::queueScroll(window, runtime, 0.0, -20.0);
                    probe::pollOnce(window, runtime);
                    for (int f = 0; f < 8; ++f) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(16));
                        probe::pollOnce(window, runtime);
                        if (runtime.composeRequested()) {
                            const auto before = components::detail::markdownPerfStats();
                            const double ms = timedM();
                            probe::pollOnce(window, runtime);
                            const std::string tag = "滚格" + std::to_string(i);
                            dump(tag.c_str(), ms, before);
                        }
                    }
                }
            }
            // A/B：旧查看器组件级形态（整篇、无 scrollView 包装/裁剪/估高缓存）
            std::ifstream mIn(fixturePath, std::ios::binary);
            const std::string mText((std::istreambuf_iterator<char>(mIn)),
                                    std::istreambuf_iterator<char>());
            const components::MarkdownStyle mStyle;
            {
                const auto before = components::detail::markdownPerfStats();
                const auto t0 = std::chrono::steady_clock::now();
                runtime.compose(ms.pageId, mw, mh,
                                [&](eui::Ui& u, const eui::Screen&) {
                                    components::markdown(u, "bug399.ab.md")
                                        .markdown(mText)
                                        .width(880.0f)
                                        .style(mStyle)
                                        .build();
                                });
                const double abMs = std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - t0).count();
                dump("A/B整篇无裁剪(冷)", abMs, before);
            }
            // 热缓存复跑：旧查看器稳态量级（布局缓存命中后仍全篇构建无裁剪）
            {
                const auto before = components::detail::markdownPerfStats();
                const auto t0 = std::chrono::steady_clock::now();
                runtime.compose(ms.pageId, mw, mh,
                                [&](eui::Ui& u, const eui::Screen&) {
                                    components::markdown(u, "bug399.ab.md")
                                        .markdown(mText)
                                        .width(880.0f)
                                        .style(mStyle)
                                        .build();
                                });
                const double abMs = std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - t0).count();
                dump("A/B整篇无裁剪(热)", abMs, before);
            }
            // 搜索链路等价量：预览态查找条按 §4.6 禁用，命中高亮唯一入口 =
            // 选区反查 hitRectsForDisplayRangeDoc（同链路）
            {
                const std::string dt = components::MarkdownBuilder::displayTextOf(mText);
                const auto t0 = std::chrono::steady_clock::now();
                const auto rects = components::MarkdownBuilder::hitRectsForDisplayRangeDoc(
                    mText, 880.0f, mStyle, 0, dt.size());
                const double qMs = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - t0).count();
                std::cerr << "M 搜索链路: displayCp=" << dt.size() << " rects=" << rects.size()
                          << " 全篇区间反查=" << qMs << "ms\n";
            }
        }
    }

    // ── N. bug399 round8：滚动重排按位移步长节流验证（composeOnScrollOffsetChange）──
    // ①单格滚轮 compose 次数：修前惯性每帧 tick 都置重排（8 帧 → ~8 次），修后
    //    位移 ~600px / 步长 256 → ~2 次；②惯性期重排总耗时与主线程占用率对比；
    // ③视觉平移由渲染期 transform 逐帧生效（J 段像素断言守）。夹具/窗档同 L。
    {
        std::filesystem::path repoRoot;
        for (std::filesystem::path p = std::filesystem::current_path(); !p.empty();
             p = p.parent_path()) {
            if (std::filesystem::exists(p / "tests/ui_driver/fixtures/bug399-git-redesign.md")) {
                repoRoot = p;
                break;
            }
            if (!p.has_parent_path()) break;
        }
        if (repoRoot.empty()) {
            passed = fail("N: 定位不到仓库根（tests/ui_driver/fixtures/bug399-git-redesign.md）");
        } else {
            module::notepad::openNotepadWindow(
                (repoRoot / "tests/ui_driver/fixtures/bug399-git-redesign.md").string());
            module::notepad::NotepadSession& ns =
                *module::notepad::detail::sessionRegistry().back();
            ns.previewOn = true;
            const float nw = 900.0f;
            const float nh = 640.0f;
            int nCompCount = 0;
            double nCompMs = 0.0;
            double nCompMaxMs = 0.0;
            auto timedN = [&] {
                const auto t0 = std::chrono::steady_clock::now();
                runtime.compose(ns.pageId, nw, nh,
                                [&](eui::Ui& u, const eui::Screen& screen) {
                                    ui = &u;
                                    module::notepad::notepadComposeSession(ns, u, screen);
                                });
                const double ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - t0).count();
                ++nCompCount;
                nCompMs += ms;
                nCompMaxMs = std::max(nCompMaxMs, ms);
            };
            timedN();
            timedN();  // 首帧建树 + 布局稳态（不计入滚动期统计）
            const double nFirstMs = nCompMs;
            nCompCount = 0;
            nCompMs = 0.0;
            nCompMaxMs = 0.0;
            const core::dsl::Element* nsc = ui->find(module::notepad::kNotepadPreviewId);
            std::string perEvent;
            int scrollComposes = 0;
            const auto nT0 = std::chrono::steady_clock::now();
            if (nsc != nullptr) {
                const float cx = nsc->frame.x + nsc->frame.width * 0.5f;
                const float cy = nsc->frame.y + nsc->frame.height * 0.5f;
                for (int i = 0; i < 8; ++i) {
                    const int before = nCompCount;
                    core::queuePointerMotion(window, cx, cy, {}, {});
                    probe::pollOnce(window, runtime);
                    probe::queueScroll(window, runtime, 0.0, -20.0);
                    probe::pollOnce(window, runtime);
                    for (int f = 0; f < 8; ++f) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(16));
                        probe::pollOnce(window, runtime);
                        if (runtime.composeRequested()) {
                            timedN();
                            probe::pollOnce(window, runtime);
                        }
                    }
                    perEvent += (i == 0 ? "" : "/") + std::to_string(nCompCount - before);
                }
                scrollComposes = nCompCount;
                // 滚轮停止后残余惯性继续推进位移，直到不再请求重排（节流后可能
                // 再跨 1-2 个步长）；收尾期计入总次数但不进「每格」判据。
                for (int f = 0; f < 120; ++f) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                    probe::pollOnce(window, runtime);
                    if (!runtime.composeRequested()) break;
                    timedN();
                    probe::pollOnce(window, runtime);
                }
            }
            const double nWallMs = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - nT0).count();
            std::cerr << "N 节流: 首帧=" << nFirstMs << "ms 滚动期compose次数=" << scrollComposes
                      << "（每格=" << perEvent << "）含收尾总次数=" << nCompCount
                      << " 重排均值=" << (nCompCount > 0 ? nCompMs / nCompCount : 0.0)
                      << "ms 最大=" << nCompMaxMs << "ms 总重排=" << nCompMs
                      << "ms 墙钟=" << nWallMs << "ms 主线程占用率="
                      << (nWallMs > 0.0 ? (nCompMs / nWallMs * 100.0) : 0.0) << "%\n";
            if (nsc == nullptr) {
                passed = fail("N: 预览不可滚动（夹具/窗档不满足复现前件）");
            } else if (scrollComposes > 32) {
                passed = fail("N: 滚动期重排未按位移步长节流（8 格 compose " +
                              std::to_string(scrollComposes) + " 次 > 32 = 修前红判据）");
            } else {
                std::cerr << "N: PASS — 8 格滚轮滚动期重排 " << scrollComposes
                          << " 次（修前每帧一次），视觉位移不受影响（J 像素断言）\n";
            }
        }
    }

    // ── P. bug399 round9：尺寸切换族仪表 + P1-A/P1-B 修复回归锁（debt398）──
    // 老板装机实测：「最小化最大化窗口会卡顿」「最大化后很难滚动，上下滚动距离很短」。
    // 同会话按档切逻辑宽高（base 900×640 / small 720×480 / max 1600×1000 / restore），
    // 每档冷+稳态各 compose 一次，读 docH/maxOffset/viewport/滚轮步长 + markdown 缓存
    // 命中率 + 段表体积。判据：①restore 后 maxOffset 回 base（几何不跨档污染）；
    // ②稳态 warm compose parse miss=0（P1-B 解析缓存全命中）；③段表 perEnt 体积
    // 受控（P1-A 段合并生效，修前未合并 ~KB 级/条）。
    {
        std::filesystem::path repoRoot;
        for (std::filesystem::path p = std::filesystem::current_path(); !p.empty();
             p = p.parent_path()) {
            if (std::filesystem::exists(p / "tests/ui_driver/fixtures/bug399-git-redesign.md")) {
                repoRoot = p;
                break;
            }
            if (!p.has_parent_path()) break;
        }
        if (repoRoot.empty()) {
            passed = fail("P: 定位不到仓库根（bug399-git-redesign.md）");
        } else {
            module::notepad::openNotepadWindow(
                (repoRoot / "tests/ui_driver/fixtures/bug399-git-redesign.md").string());
            module::notepad::NotepadSession& ps =
                *module::notepad::detail::sessionRegistry().back();
            ps.previewOn = true;
            struct SizeStep {
                const char* tag;
                float w;
                float h;
            };
            const SizeStep steps[] = {
                {"base-1000x640", 1000.0f, 640.0f},
                {"small-760x480", 760.0f, 480.0f},
                {"max-1600x1000", 1600.0f, 1000.0f},
                {"restore-1000x640", 1000.0f, 640.0f},
            };
            auto timedP = [&](float w, float h) {
                const auto t0 = std::chrono::steady_clock::now();
                runtime.compose(ps.pageId, w, h,
                                [&](eui::Ui& u, const eui::Screen& screen) {
                                    ui = &u;
                                    module::notepad::notepadComposeSession(ps, u, screen);
                                });
                return std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - t0).count();
            };
            auto readGeom = [&](float& docH, float& maxOff, float& vpH, float& step) {
                const core::dsl::Element* sc = ui->find(module::notepad::kNotepadPreviewId);
                const core::dsl::Element* wrap = ui->find(
                    std::string(module::notepad::kNotepadPreviewContentId) + ".wrap");
                docH = wrap != nullptr ? wrap->frame.height : 0.0f;
                maxOff = sc != nullptr ? sc->scrollMaxOffset : 0.0f;
                vpH = sc != nullptr ? sc->frame.height : 0.0f;
                step = sc != nullptr ? sc->scrollStep : 0.0f;
            };
            double baseMaxOff = -1.0;
            double restoreMaxOff = -1.0;
            long long baseColdLayouts = 0;
            long long baseColdSegs = 0;
            long long baseColdRuns = 0;
            bool p1aOk = true;
            bool p1bOk = true;
            for (std::size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
                const SizeStep& st = steps[i];
                const auto beforeCold = components::detail::markdownPerfStats();
                const double coldMs = timedP(st.w, st.h);  // 宽度可能变 → 重新测量
                const auto afterCold = components::detail::markdownPerfStats();
                const long long coldLayouts =
                    afterCold.inlineLayoutBuilds - beforeCold.inlineLayoutBuilds;
                const long long coldSegs =
                    afterCold.inlineSegmentsBuilt - beforeCold.inlineSegmentsBuilt;
                const long long coldRuns =
                    afterCold.inlineRunsBuilt - beforeCold.inlineRunsBuilt;
                const auto beforeWarm = components::detail::markdownPerfStats();
                const double warmMs = timedP(st.w, st.h);  // 稳态帧
                const auto& pf = components::detail::markdownPerfStats();
                float docH = 0.0f, maxOff = 0.0f, vpH = 0.0f, step = 0.0f;
                readGeom(docH, maxOff, vpH, step);
                const long long warmParseMiss = pf.parseMisses - beforeWarm.parseMisses;
                if (warmParseMiss != 0) p1bOk = false;
                std::cerr << "P " << st.tag << ": cold=" << coldMs << "ms warm=" << warmMs
                          << "ms | docH=" << docH << " vp=" << vpH << " maxOffset=" << maxOff
                          << " wheelStep=" << step
                          << " | parse hit=" << (pf.parseHits - beforeWarm.parseHits)
                          << " miss=" << warmParseMiss
                          << " tooLarge=" << (pf.docTooLarge - beforeWarm.docTooLarge)
                          << " docBytes=" << pf.lastDocBytes
                          << " | inline hit=" << (pf.inlineHits - beforeWarm.inlineHits)
                          << " miss=" << (pf.inlineMisses - beforeWarm.inlineMisses)
                          << " coldLayouts=" << coldLayouts << " coldSegs=" << coldSegs
                          << " coldRuns=" << coldRuns
                          << " segEnt=" << pf.inlineCacheEntries
                          << " segBytes=" << pf.inlineCacheBytes << "\n";
                if (i == 0) {
                    baseMaxOff = maxOff;
                    baseColdLayouts = coldLayouts;
                    baseColdSegs = coldSegs;
                    baseColdRuns = coldRuns;
                }
                if (i == 3) restoreMaxOff = maxOff;
            }
            // P1-A 段表体积回归锁：比值口径（bug399 round10 修正——旧 avgSegs ≤ 64
            // 判据对「回退合并」不敏感：每段几个视觉 run 时 ~20 段/条仍绿）。合并生效
            // = 每条布局段数 ≤ run 数 × 2（同 run 同行多码点已并成一段，折行最多按
            // 行再分；回退成对合并即 8× 以上）。
            const double avgSegs =
                baseColdLayouts > 0 ? static_cast<double>(baseColdSegs) / baseColdLayouts : 0.0;
            const double segPerRun =
                baseColdRuns > 0 ? static_cast<double>(baseColdSegs) / baseColdRuns : 0.0;
            if (baseColdLayouts <= 0 || baseColdRuns <= 0 || segPerRun > 2.0) p1aOk = false;
            if (baseMaxOff <= 0.0) {
                passed = fail("P: 预览不可滚动（夹具/窗档不满足复现前件）");
            } else if (std::fabs(restoreMaxOff - baseMaxOff) > 1.0) {
                passed = fail("P: 尺寸档往返后 maxOffset 漂移（base=" +
                              std::to_string(baseMaxOff) + " restore=" +
                              std::to_string(restoreMaxOff) + "）= 跨档几何污染");
            } else if (!p1aOk) {
                passed = fail("P1-A: 段合并未生效（冷布局 segs/run=" +
                              std::to_string(segPerRun) + " > 2；coldSegs=" +
                              std::to_string(baseColdSegs) + " coldRuns=" +
                              std::to_string(baseColdRuns) + "）");
            } else if (!p1bOk) {
                passed = fail("P1-B: 稳态 compose 仍解析缓存 miss（应全命中）");
            } else {
                std::cerr << "P: PASS — 尺寸档 maxOffset 往返稳定 base=" << baseMaxOff
                          << " restore=" << restoreMaxOff << "；冷布局 avgSegs="
                          << avgSegs << "/条，segs/run=" << segPerRun << "（segs=" << baseColdSegs
                          << " runs=" << baseColdRuns << "）；稳态 parse 全命中\n";
            }
        }
    }

    // ── Q. bug399 round10：段间空隙均匀性（P0 段合并未吸收 gap 的视觉回归锁）──
    // 修前：布局级合并只累加 segmentWidth 不吸收 gap → 合并段声明宽比游标推进窄
    // 4px/次 → 渲染期 CJK 两字一组、组间 8px 字距抖动（round4-scrolled.png）。
    // 像素 ink 断言对位移不敏感拦不住这类回归，故直接在渲染元素树取证：
    //  ①正文行每个 .seg. 元素声明宽 = measure(整段文本) + (码点数-1)×4（gap 已并入
    //    宽度累计；修前成对合并段恒缺 4px → 红；回退合并则单码点成段亦合规，
    //    故配 P 段 segs/run 比值锁）；
    //  ②同行相邻 .seg. 元素间隙 ∈ {0, 4}±0.6，出现 8px 成组模式 → 红。
    {
        std::filesystem::path repoRoot;
        for (std::filesystem::path p = std::filesystem::current_path(); !p.empty();
             p = p.parent_path()) {
            if (std::filesystem::exists(p / "tests/ui_driver/fixtures/bug399-git-redesign.md")) {
                repoRoot = p;
                break;
            }
            if (!p.has_parent_path()) break;
        }
        if (repoRoot.empty()) {
            passed = fail("Q: 定位不到仓库根（bug399-git-redesign.md）");
        } else {
            module::notepad::openNotepadWindow(
                (repoRoot / "tests/ui_driver/fixtures/bug399-git-redesign.md").string());
            module::notepad::NotepadSession& qs =
                *module::notepad::detail::sessionRegistry().back();
            qs.previewOn = true;
            const float qw = 1000.0f;
            const float qh = 640.0f;
            runtime.compose(qs.pageId, qw, qh,
                            [&](eui::Ui& u, const eui::Screen& screen) {
                                ui = &u;
                                module::notepad::notepadComposeSession(qs, u, screen);
                            });
            struct QSeg {
                float x = 0.0f;
                float y = 0.0f;
                float w = 0.0f;
                float h = 0.0f;
                std::string text;
            };
            std::vector<QSeg> qsegs;
            std::function<void(const core::dsl::Element&)> walk =
                [&](const core::dsl::Element& el) {
                    const std::string marker = ".seg.";
                    const std::size_t at = el.id.rfind(marker);
                    if (at != std::string::npos) {
                        const std::string tail = el.id.substr(at + marker.size());
                        const bool digits =
                            !tail.empty() && std::all_of(tail.begin(), tail.end(), [](char c) {
                                return c >= '0' && c <= '9';
                            });
                        if (digits) {
                            // .seg.N 是定位 stack，文本串在子元素 .seg.N.text 上；
                            // chip（行内代码/引用胶囊）宽含 12px 内衬，量宽口径不同 → 排除。
                            std::string segText;
                            bool chip = false;
                            for (const auto& child : el.children) {
                                const std::string& cid = child->id;
                                if (cid.size() >= 5 &&
                                    cid.compare(cid.size() - 5, 5, ".text") == 0) {
                                    segText = child->text;
                                } else if (cid.size() >= 3 &&
                                           cid.compare(cid.size() - 3, 3, ".bg") == 0) {
                                    chip = true;
                                }
                            }
                            if (!chip && !segText.empty()) {
                                qsegs.push_back({el.frame.x, el.frame.y, el.frame.width,
                                                 el.frame.height, segText});
                            }
                        }
                    }
                    for (const auto& child : el.children) walk(*child);
                };
            for (const auto& root : ui->roots()) walk(*root);

            const components::MarkdownStyle qStyle(
                core::theme::componentTokensWeChat(core::theme::current()));
            auto cpCount = [](const std::string& t) {
                std::size_t n = 0;
                for (std::size_t i = 0; i < t.size();) {
                    const unsigned char c = static_cast<unsigned char>(t[i]);
                    i += c >= 0xF0U ? 4 : c >= 0xE0U ? 3 : c >= 0xC0U ? 2 : 1;
                    ++n;
                }
                return n;
            };
            // 只取正文行：行高 == bodyLineHeight（排除标题/代码等异字号族）
            std::vector<const QSeg*> body;
            for (const QSeg& seg : qsegs) {
                if (std::fabs(seg.h - qStyle.bodyLineHeight) <= 0.6f) body.push_back(&seg);
            }
            if (body.empty()) {
                passed = fail("Q: 预览首屏无正文 .seg. 元素（元素数=" +
                              std::to_string(qsegs.size()) + "）= 取证前提不成立");
            } else {
                std::unordered_map<int, std::vector<const QSeg*>> byLine;
                for (const QSeg* seg : body) {
                    byLine[static_cast<int>(std::lround(seg->y))].push_back(seg);
                }
                std::vector<const QSeg*>* best = nullptr;
                long long bestCps = -1;
                for (auto& kv : byLine) {
                    long long sum = 0;
                    for (const QSeg* seg : kv.second) sum += static_cast<long long>(cpCount(seg->text));
                    if (sum > bestCps) {
                        bestCps = sum;
                        best = &kv.second;
                    }
                }
                std::sort(best->begin(), best->end(),
                          [](const QSeg* a, const QSeg* b) { return a->x < b->x; });
                // ①段盒宽必须容得下自己画的文本（delta = w - measure >= 0）：修前成对
                //   合并的声明宽 = Σ part measure（不含 gap）却在游标上占了 Σ+gap，盒
                //   比分配窄 → 字被压成组；上界 = (码点数-1)×4（每码点最多吸收一个
                //   gap）+ 1 浮点余量，防重复吸收。
                std::vector<std::string> badWidth;
                int badWidthCount = 0;
                long long bodyCps = 0;
                for (const QSeg* seg : body) {
                    const std::size_t cps = cpCount(seg->text);
                    bodyCps += static_cast<long long>(cps);
                    const float measured = components::detail::measureMarkdownTextWidth(
                        seg->text, qStyle.fontFamily, qStyle.bodySize, 400);
                    const float delta = seg->w - measured;
                    const float upper =
                        (cps > 1 ? static_cast<float>(cps - 1) * 4.0f : 0.0f) + 1.0f;
                    if (delta < -0.6f || delta > upper) {
                        ++badWidthCount;
                        if (badWidth.size() < 5) {
                            badWidth.push_back("w=" + std::to_string(seg->w) + " m=" +
                                               std::to_string(measured) + " delta=" +
                                               std::to_string(delta) + " cps=" +
                                               std::to_string(cps));
                        }
                    }
                }
                // ②合并生效度：正文段数 / 码点数 <= 0.25（成对合并 = 0.5、逐码点 = 1）
                const double segPerCp = bodyCps > 0
                    ? static_cast<double>(body.size()) / static_cast<double>(bodyCps) : 1.0;
                // ②同行相邻段间隙均匀性
                std::string gapBad;
                for (std::size_t i = 0; i + 1 < best->size(); ++i) {
                    const float gap = (*best)[i + 1]->x - ((*best)[i]->x + (*best)[i]->w);
                    const bool okZero = std::fabs(gap) <= 0.6f;
                    const bool okGap = std::fabs(gap - 4.0f) <= 0.6f;
                    if (!okZero && !okGap && gapBad.empty()) {
                        gapBad = "段#" + std::to_string(i) + " 间隙=" + std::to_string(gap) +
                                 "px（应为 0 或 4；8px 成组=合并段宽未吸收 gap）";
                    }
                }
                std::cerr << "Q 采样行 y=" << std::lround((*best)[0]->y) << " 段数="
                          << best->size() << " 码点=" << bestCps << "：";
                for (std::size_t i = 0; i < best->size() && i < 8; ++i) {
                    std::cerr << " [" << (*best)[i]->x << "," << (*best)[i]->w << "]";
                }
                std::cerr << "\n";
                if (badWidthCount > 0) {
                    std::string detail;
                    for (const std::string& d : badWidth) detail += " " + d;
                    passed = fail("Q: 段盒宽不容文本 / 越界（" +
                                  std::to_string(badWidthCount) + " 处）:" + detail);
                } else if (!gapBad.empty()) {
                    passed = fail("Q: " + gapBad);
                } else if (segPerCp > 0.25) {
                    passed = fail("Q: 段合并未生效（段数/码点数=" +
                                  std::to_string(segPerCp) + " > 0.25）");
                } else {
                    std::cerr << "Q: PASS — 段盒宽含 gap（n=" << body.size()
                              << "，段数/码点数=" << segPerCp
                              << "），同行段间隙均匀（采样行段数=" << best->size()
                              << "）\n";
                }
            }
        }
    }

    // --screenshot 落图（未传 = 零开销；传了失败 → 非零退出码）
    probe::captureIfRequested(args, window, runtime, &passed);

    module::notepad::resetNotepadSessionsForTest();
    std::filesystem::remove_all(dir);
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();

    if (!passed) {
        std::cerr << "FAIL\n";
        return 1;
    }
    std::cout << "notepad_probe: PASS（" << args.windowW << "x" << args.windowH
              << "，8 session（含 J bug399 预览滚动）：正常/只读/错误占位 + 查找条键盘流（含关条焦点回归）+ 按钮矩阵/预览切换 + 换行钮 + 未保存保护链 + 另存为钮矩阵/M+Shift+S 守卫/三快捷键接线 + 弹层收起/统计缓存/提示条可关/换行钮让位）\n";
    return 0;
}
