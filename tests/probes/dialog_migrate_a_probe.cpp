// dialog_migrate_a_probe.cpp — need156 批A（共享件旧壳批）弹窗迁移亮暗取证探针
//
// 覆盖四弹窗（headless 可达面）：memory 编辑（审核卡「编辑」）/ 掩码（配置页
// 脱敏开关二次确认）/ 清空（数据卡清除）+ claude 新建表单（Ctrl+N）。claude
// 删除/切换确认弹窗需 service 环境种 Profile，本探针不覆盖（见 need156 批A 终报）。
// 每弹窗亮暗各一张：先结构自报（panel/.title.line/.footer.line/.confirm 在否，
// before/after 对照读 SELF-CHECK 行，缺行不判败——旧壳树上无新壳节点），再落 PNG。
//
// 运行：dialog_migrate_a_probe [--out <目录>] [--size 1200x1000]
// 数据隔离：DEVDESK_DATA_ROOT 指临时目录 + 1 条种子提案，不碰真实数据根。

#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（同 md_scrim_probe）
#include <ctime>
#include <system_error>
#include "app_registry.h"
#include "core/module_interface.h"
#include "core/theme.h"
#include "module_memory/engine/telemem_config.h"
#include "module_memory/memory_module.h"
#include "module_memory/proposals/curator.h"
#include "module_memory/proposals/proposal_store.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_dialog_migrate_a_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

void setEnvVar(const char* key, const std::string& value) {
#ifdef _WIN32
    _putenv_s(key, value.c_str());
#else
    setenv(key, value.c_str(), 1);
#endif
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    std::string outDir = ".verify-screenshots";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc) outDir = argv[++i];
    }
    std::error_code outEc;
    std::filesystem::create_directories(outDir, outEc);

    // 独立数据根（先于一切 memoryDataDir 消费）+ 1 条种子提案（编辑弹窗入口）
    const auto tmpRoot =
        std::filesystem::temp_directory_path() /
        ("dlg-mig-a-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(tmpRoot);
    setEnvVar("DEVDESK_DATA_ROOT", tmpRoot.string());
    const auto dataDir = module::memory::engine::memoryDataDir();
    std::string proposalId;
    {
        std::filesystem::create_directories(dataDir);
        const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        auto prop =
            module::memory::proposals::ProposalStore::open(dataDir / "proposals.db");
        if (prop.isFail()) {
            std::cerr << "probe: 提案库打开失败: " << prop.errorMessage() << "\n";
            return 3;
        }
        using module::memory::proposals::ProposalEvidence;
        const ProposalEvidence ev{"probe-session", 1, "探针种子原文", now - 60};
        auto addP = module::memory::proposals::Curator::proposeAdd(
            prop.value(), "探针种子提案：编辑弹窗用", {ev}, 0.9, now);
        if (addP.isFail()) {
            std::cerr << "probe: 种子提案创建失败\n";
            return 3;
        }
        auto pend = prop.value().listPending();
        if (pend.isFail() || pend.value().empty()) {
            std::cerr << "probe: 种子提案不可见\n";
            return 3;
        }
        proposalId = pend.value().front().id;
    }

    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("dialog_migrate_a_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    std::unique_ptr<::core::Module> mem(create_memory_module());
    std::unique_ptr<::core::Module> claude(create_claude_module());
    mem->onStart();
    claude->onStart();
    auto* memMod = dynamic_cast<module::memory::MemoryModule*>(mem.get());

    core::dsl::Runtime runtime;
    runtime.initialize(window);
    const auto [logicalW, logicalH] = probe::windowLogicalSize(window);
    const float w = logicalW > 1.0f ? logicalW : static_cast<float>(args.windowW);
    const float h = logicalH > 1.0f ? logicalH : static_cast<float>(args.windowH);

    eui::Ui* ui = nullptr;
    auto composeMem = [&]() {
        probe::composeModuleFrame(
            runtime, window, "memory", w, h,
            [&](eui::Ui& u, const eui::Screen& screen) { ui = &u; mem->compose(u, screen); });
    };
    auto composeClaude = [&]() {
        probe::composeModuleFrame(
            runtime, window, "claude", w, h,
            [&](eui::Ui& u, const eui::Screen& screen) { ui = &u; claude->compose(u, screen); });
    };
    bool passed = true;
    auto clickId = [&](const std::string& id) {
        const auto* el = ui->find(id);
        if (el == nullptr || el->onClick == nullptr) {
            std::cerr << "probe: 不可点击或缺节点 '" << id << "'\n";
            passed = false;
            return false;
        }
        el->onClick();
        return true;
    };
    auto reportShell = [&](const std::string& id) {
        for (const char* suffix : {".panel", ".title.line", ".footer.line", ".confirm"}) {
            std::cout << "SELF-CHECK " << id << suffix << " "
                      << (ui->find(id + suffix) != nullptr ? "present" : "MISSING")
                      << "\n";
        }
    };
    auto settle = [&]() {
        for (int i = 0; i < 24; ++i) probe::pollOnce(window, runtime);
    };
    auto shoot = [&](const std::string& name) {
        probe::FrameCapture capture(window);
        if (!capture.capturePng(runtime, (std::filesystem::path(outDir) / name).string())) {
            std::cerr << "probe: 落图失败 " << name << "\n";
            passed = false;
        } else {
            std::cout << "probe: screenshot " << name << "\n";
        }
    };

    // 单趟四弹窗：触发 → 稳帧 → 结构自报 → 落图 → 副钮关闭
    auto onePass = [&](const std::string& tag) {
        // 1) memory 编辑弹窗
        if (memMod != nullptr) memMod->setActiveViewForTest(1);
        composeMem();
        // ui_kit::button 的 onClick 挂在 rect 子节点 <id>.bg 上（stack 自身无
        // onClick）；点 stack 会「不可点击」且弹窗不打开。
        clickId("memory.rv.edit." + proposalId + ".bg");
        composeMem();
        settle();
        reportShell("memory.dialog.edit");
        shoot("memory-edit-" + tag + ".png");
        clickId("memory.dialog.edit.cancel.bg");
        composeMem();
        // 2) memory 掩码二次确认
        if (memMod != nullptr) memMod->setActiveViewForTest(2);
        composeMem();
        clickId("memory.cfg.mask.switch.track");
        composeMem();
        settle();
        reportShell("memory.dialog.mask");
        shoot("memory-mask-" + tag + ".png");
        clickId("memory.dialog.mask.cancel.bg");
        composeMem();
        // 3) memory 清空确认
        clickId("memory.cfg.data.clear.bg");
        composeMem();
        settle();
        reportShell("memory.dialog.clear");
        shoot("memory-clear-" + tag + ".png");
        clickId("memory.dialog.clear.cancel.bg");
        composeMem();
        // 4) claude 新建表单（Ctrl+N；macOS=Cmd+N 同用户键通道）
        composeClaude();
        eui::KeyEvent cn;
        cn.key = eui::InputKey::N;
        cn.action = eui::KeyAction::Press;
#if defined(__APPLE__)
        cn.modifiers.super = true;
#else
        cn.modifiers.control = true;
#endif
        if (!claude->onKeyEvent(cn)) {
            std::cerr << "probe: Ctrl+N 未被 claude 模块消费\n";
            passed = false;
        }
        composeClaude();
        settle();
        reportShell("claude.dialog.form");
        shoot("claude-form-" + tag + ".png");
        clickId("claude.dialog.form.cancel.bg");
        composeClaude();
    };

    core::theme::setLight();
    onePass("light");
    core::theme::setDark();
    onePass("dark");

    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    std::cout << (passed ? "probe: PASS" : "probe: FAIL") << "\n";
    return passed ? 0 : 1;
}
