// notepad_probe.cpp — 记事本批 1 窗口层探针（need:273，断言面仅批 1 范围）
//
// headless compose 驱动 openNotepadWindow（临时真文件走真实载入链）：
//   A 正常态：工具条 id 全集（btnOpen/btnSave/toolbar）+ 无新建钮（N-14）+
//     编辑 input 存在（D-6「无编辑入口」断言反转）+ 状态栏（文件名/行:列/字符数）
//   B 只读降级：UTF-16 BOM 文件 → promptbar 提示条在场 + 保存钮 disabled
//   C 错误占位：不存在路径 → session 照建 + loaderror 占位（无编辑器无提示条）
// 运行：notepad_probe [--screenshot <路径>]（不带 = 零开销不落图）。
// 查找条键盘流/预览切换/三选弹窗/JSON 格式化 = 批 2/3/4 面，不在本探针。
#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（openWindow=入队不建窗）

#include "module_notepad/notepad_module.h"
#include "module_notepad/notepad_session.h"  // notepadComposeSession（生产 compose 单点）

#include <filesystem>
#include <fstream>
#include <iostream>
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
    {
        std::ofstream note(notePath, std::ios::binary);
        note << "hello\nworld\n";  // 12 码点，无 BOM，LF
    }
    {
        std::ofstream u16(utf16Path, std::ios::binary);
        u16 << "\xFE\xFF\x00h\x00i";  // UTF-16BE BOM → 只读降级
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
              << "，3 session：正常/只读/错误占位）\n";
    return 0;
}
