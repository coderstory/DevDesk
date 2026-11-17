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
#include "module_notepad/notepad_module.h"
#include "module_notepad/notepad_protect.h"  // G 段：veto 裁决/quit guard 决策面
#include "module_notepad/notepad_session.h"  // notepadComposeSession（生产 compose 单点）

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>  // istreambuf_iterator（G3 落盘内容核对）
#include <string>

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
              << "，6 session：正常/只读/错误占位 + 查找条键盘流 + 按钮矩阵/预览切换 + 换行钮 + 未保存保护链 + 另存为钮矩阵/M+Shift+S 守卫/三快捷键接线）\n";
    return 0;
}
