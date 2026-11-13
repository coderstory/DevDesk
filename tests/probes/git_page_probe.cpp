// git_page_probe.cpp — Git 管理器页截图探针（need:7 批次二证据物）
//
// 运行：git_page_probe.exe --screenshot <路径> [--size WxH] [--light]
// 取证面：①空态页（无记忆首启）②主界面三栏（注入选仓 → fixture 仓打开：树+
// 变更列表+右栏占位动线）③深色主题主界面。布局断言组：三栏几何 + 关键节点在场。
// 骨架仿 claude_probe（init→create_module→onStart→compose→captureIfRequested）。
#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>
#include "test_support.h"
#include "core/module_interface.h"
#include "core/theme.h"
#include "platform/folder_picker.h"
#include "platform/git/git_libgit2.h"

#include <git2.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

// app:: 层桩（probe 不建应用级 UI，compose 走 probe::composeModuleFrame）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_git_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

extern "C" ::core::Module* create_git_module();

namespace {

// 阶段标记直落文件（stdio 重定向在 msys timeout 下的行为不可靠，取证走文件追加）
void stage(const std::string& what) {
    std::ofstream log("D:/project/gui/DevDesk/build/probe_stage.log",
                      std::ios::app);
    log << what << "\n";
}

} // namespace

namespace {

// fixture 仓（git2 API 直建，离线）：中文目录 + ignored 目录 + 修改/新增/删除面
struct FixtureRepo {
    std::filesystem::path dir;
    bool ok = false;
};

FixtureRepo makeProbeRepo(const std::filesystem::path& root) {
    FixtureRepo out;
    out.dir = root / "示例仓库";
    std::filesystem::create_directories(out.dir);
    auto checkRc = [](int rc, const char* what) {
        if (rc < 0) {
            const git_error* e = git_error_last();
            std::cerr << "git_page_probe: fixture " << what << ": "
                      << (e != nullptr && e->message != nullptr ? e->message : "?")
                      << "\n";
        }
        return rc >= 0;
    };
    git_repository* repo = nullptr;
    if (!checkRc(git_repository_init(&repo, platform::git::pathToUtf8(out.dir).c_str(), 0),
                 "init")) {
        return out;
    }
    git_repository_free(repo);

    const char* kFiles[][2] = {
        {".gitignore", "build/\n"},
        {"a.txt", "第一版\n"},
        {"说明文档/接口规范.md", "# 规范\n内容\n"},
    };
    for (const auto& f : kFiles) {
        const auto p = out.dir / platform::git::utf8ToPath(f[0]);
        std::filesystem::create_directories(p.parent_path());
        std::ofstream ofs(p, std::ios::binary);
        ofs << f[1];
    }
    // index + commit（git2 低阶，保持离线与零 CLI 依赖）
    if (!checkRc(git_repository_open(&repo, platform::git::pathToUtf8(out.dir).c_str()),
                 "reopen")) {
        return out;
    }
    git_index* index = nullptr;
    git_repository_index(&index, repo);
    for (const auto& f : kFiles) {
        git_index_add_bypath(index, f[0]);
    }
    git_oid treeOid{};
    git_index_write_tree(&treeOid, index);
    git_index_write(index);
    git_index_free(index);
    git_signature* sig = nullptr;
    git_signature_now(&sig, "probe", "probe@local");
    git_tree* tree = nullptr;
    git_tree_lookup(&tree, repo, &treeOid);
    git_oid commitOid{};
    git_commit_create(&commitOid, repo, "HEAD", sig, sig, "utf-8", "probe init",
                      tree, 0, nullptr);
    git_tree_free(tree);
    git_signature_free(sig);
    git_repository_free(repo);

    // 工作区变更面：modified（a.txt + 接口规范.md）+ untracked（新增.txt）+ ignored（build/）
    {
        std::ofstream ofs(out.dir / "a.txt", std::ios::binary);
        ofs << "第二版\n";
        // F-07⑥ 真横滚取证行：1500 字符超长行进 diff（批四）
        ofs << std::string(1500, 'x') << "\n";
    }
    {
        // md 也带改动（need:10 批次三）：保留「内容」公共行 → diff 出上下文行
        //（三色齐全：增/删/上下文）；变更列表直达 md 条目 → diff/内容高亮取证
        std::ofstream ofs(out.dir / "说明文档" / "接口规范.md", std::ios::binary);
        ofs << "# 接口规范 v2\n内容\n\n含 **粗体** 与 `行内代码`。\n\n```java\npublic class A {\n}\n```\n";
    }
    {
        std::ofstream ofs(out.dir / "新增文件.txt", std::ios::binary);
        ofs << "untracked\n";
    }
    {
        // bug#18 取证：untracked >1MiB 占位档（'0_' ASCII 前缀 = libgit2 路径字节序
        // 在 untracked 组内排最前 → 行序确定：组头后行4）。≤1MiB 闸短路不读内容。
        std::ofstream ofs(out.dir / "0_oversize_placeholder.bin", std::ios::binary);
        ofs << std::string(1024 * 1024 + 1, 'x');
    }
    {
        // 超长文件名（批四）：树行截断 + 悬浮全名 tooltip 取证
        std::ofstream ofs(out.dir / "这是一个相当长的中文文件名用来验证树行截断后悬浮提示能显出完整名称.txt",
                          std::ios::binary);
        ofs << "长名\n";
    }
    std::filesystem::create_directories(out.dir / "build");
    {
        std::ofstream ofs(out.dir / "build" / "out.o", std::ios::binary);
        ofs << "obj\n";
    }
    out.ok = true;
    return out;
}

// 列表第 idx 行（28px 定高槽）中心的点击坐标（virtualList 行盒命中层）
struct Pt {
    double x;
    double y;
};

Pt rowCenter(const core::dsl::Element* list, int idx) {
    const float rowH = 28.0f;  // 树/列表定高槽（scaled 基准=1.0 探针档）
    return {static_cast<double>(list->frame.x + list->frame.width * 0.5f),
            static_cast<double>(list->frame.y + rowH * idx + rowH * 0.5f)};
}

// 截图路径派生：<base>_diff.png / <base>_content.png / <base>_content_md.png
std::string shotPath(const std::string& base, const char* tag) {
    if (base.empty()) return {};
    const size_t dot = base.rfind(".png");
    if (dot == std::string::npos) return base + "_" + tag + ".png";
    return base.substr(0, dot) + "_" + tag + ".png";
}

} // namespace

int main(int argc, char* argv[]) {
    std::ofstream("D:/project/gui/DevDesk/build/probe_stage.log",
                  std::ios::trunc)
        .close();  // 清上一轮
    stage("main entered");
    auto args = probe::parseProbeArgs(argc, argv);
    bool lightTheme = false;
    bool noOpen = false;  // --no-open：停在空态页取证（不开仓）
    bool noDiff = false;  // --no-diff：夹具无文本变更条目（纯二进制 untracked 仓）时跳过 ②b 文本 diff 动线
    bool bug18Shots = false;  // --untracked-diff：bug#18 两档取证（untracked 全增行 + >1MiB 占位）
    bool searchShots = false;  // --search：need:14 全局搜索取证（结果/定位/空态/ESC 取消）
    std::string externalRepo;  // --repo <路径>：外部夹具仓注入（qa12 F-09 大仓 probe）
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--light") lightTheme = true;
        if (arg == "--no-open") noOpen = true;
        if (arg == "--no-diff") noDiff = true;
        if (arg == "--untracked-diff") bug18Shots = true;
        if (arg == "--search") searchShots = true;
        if (arg == "--repo" && i + 1 < argc) externalRepo = argv[++i];
    }
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("git_page_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    // fixture 建仓先于模块 onStart：libgit2 全局 init 独立拿一份 refcount
    //（模块 onStart 的 scope 构造是第二份，refcount 语义嵌套安全）
    const ::platform::git::Libgit2Scope fixtureScope;

    // 数据根隔离（记忆仓写入不落真实用户目录）
    const auto dataRoot = test_support::makeTempDir("devdesk_git_probe_", "data");
    test_support::ScopedEnv dataRootEnv("DEVDESK_DATA_ROOT",
                                        platform::git::pathToUtf8(dataRoot).c_str());
    const FixtureRepo fixture = makeProbeRepo(dataRoot);
    if (!fixture.ok) {
        std::cerr << "git_page_probe: fixture 仓创建失败\n";
        probe::terminateWindowBackend();
        return 2;
    }
    // 选仓缝注入：空态页主按钮 → 返回 fixture 仓路径（不弹系统对话框）
    static std::string pickedRepo;
    pickedRepo = externalRepo.empty() ? platform::git::pathToUtf8(fixture.dir)
                                      : externalRepo;
    ::platform::folderpicker::setPickerForTest(
        +[](const std::string&, const std::string&) -> core::Result<std::string> {
            return core::Result<std::string>::ok(pickedRepo);
        });

    std::unique_ptr<::core::Module> mod(create_git_module());
    mod->onStart();

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    if (lightTheme) core::theme::setLight();

    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    bool passed = true;

    core::dsl::Ui* ui = nullptr;
    auto composeFrame = [&]() {
        probe::composeModuleFrame(
            runtime, window, "git", w, h,
            [&](eui::Ui& u, const eui::Screen& screen) {
                ui = &u;
                mod->compose(u, screen);
            });
    };

    // ① 空态页（无记忆首启）
    composeFrame();
    composeFrame();
    if (ui == nullptr || ui->find("git.empty.title") == nullptr) {
        std::cerr << "git_page_probe: FAIL——空态页未出（git.empty.title 缺失）\n";
        passed = false;
    }

    // ② 点主按钮 → 打开 fixture 仓 → 泵到 worker 完成（校验/status/根 walk）
    if (passed && !noOpen) {
        const core::dsl::Element* pick = ui->find("git.empty.pick.hit");
        if (pick == nullptr) {
            std::cerr << "git_page_probe: FAIL——主按钮缺失\n";
            passed = false;
        } else {
            stage("click pick button");
            probe::click(window, runtime,
                         static_cast<double>(pick->frame.x + pick->frame.width * 0.5f),
                         static_cast<double>(pick->frame.y + pick->frame.height * 0.5f));
            stage("click dispatched, pump frames");
            // worker 链：validate → open → status → walk；每帧 composePump 推进。
            // 外部大仓（--repo）：status 全量万条需真实等待，改收敛驱动泵
            //（50ms/帧 + 120s 硬超时）——收敛耗时即 F-09「不 hang」证据。
            const auto openT0 = std::chrono::steady_clock::now();
            try {
                if (!externalRepo.empty()) {
                    bool converged = false;
                    for (int i = 0; i < 2400 && !converged; ++i) {
                        composeFrame();
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(50));
                        converged = ui->find("git.changes.list") != nullptr &&
                                    ui->find("git.tree.list") != nullptr;
                    }
                    // 稳态补泵 2s：status 条目全量上屏
                    for (int i = 0; i < 40; ++i) {
                        composeFrame();
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(50));
                    }
                } else {
                    for (int i = 0; i < 60; ++i) {
                        composeFrame();
                        if (i % 10 == 9) stage("frame " + std::to_string(i + 1));
                    }
                }
            } catch (const std::exception& e) {
                stage(std::string("EXCEPTION in pump: ") + e.what());
                throw;
            }
            const auto openMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - openT0)
                    .count();
            stage("pump done, open->mainUI " + std::to_string(openMs) + " ms");
            std::cout << "  仓打开→主界面收敛 " << openMs << " ms\n";
            if (ui->find("git.top.name") == nullptr ||
                ui->find("git.tree.list") == nullptr) {
                std::cerr << "git_page_probe: FAIL——60 帧内主界面未出（worker 链未收敛？）\n";
                passed = false;
            } else {
                std::cout << "== 主界面节点 ==\n";
                for (const char* id : {"git.top.name", "git.top.path", "git.top.refresh",
                                       "git.left.tab.all", "git.left.tab.changes",
                                       "git.tree.list", "git.changes.list",
                                       "git.right.path"}) {
                    const core::dsl::Element* e = ui->find(id);
                    if (e != nullptr) {
                        std::cout << "  " << id << " = (" << e->frame.x << ","
                                  << e->frame.y << ") " << e->frame.width << "x"
                                  << e->frame.height << "\n";
                    } else {
                        std::cout << "  " << id << " = <缺失>\n";
                        passed = false;
                    }
                }
                // 三栏几何：左栏宽 = 260（1280 基准档）、中栏 280
                const core::dsl::Element* treeList = ui->find("git.tree.list");
                if (treeList != nullptr) {
                    if (std::fabs(treeList->frame.width - 260.0f) > 2.0f) {
                        std::cerr << "git_page_probe: 左栏宽偏离 260（实测 "
                                  << treeList->frame.width << "）\n";
                        passed = false;
                    }
                }
            }
        }
    }

    // 点击重试（探针注入偶发丢点击，clue#60：virtualList 注入点击可能丢失——
    // probe 取证一律 clickUntil 重试模式；真实鼠标影响标 QA）。批二/三/四阶段共用。
    const auto clickUntil = [&](double x, double y,
                                const std::function<bool()>& ok) {
        for (int attempt = 0; attempt < 6; ++attempt) {
            probe::click(window, runtime, x, y);
            for (int i = 0; i < 25; ++i) composeFrame();
            if (ok()) return true;
        }
        return false;
    };

    // ②b 批次三（need:10）：N7→N8 真实接线取证——单击变更条目 → 右栏真 diff。
    // 顺序（probe 实测约束）：变更行单击仅在右栏 diff 视图在场时可靠；content
    // 视图在场时变更行单击丢失（疑似 EUI virtualList 按压捕获面异常，已回流线索，
    // 深查归批次四/QA）。故取证序：a.txt diff → md diff（第二 per-task 计算）→
    // 内容 Tab → md 高亮内容。
    if (passed && !noOpen && !noDiff) {
        const core::dsl::Element* changes = ui->find("git.changes.list");
        if (changes == nullptr) {
            std::cerr << "git_page_probe: FAIL——变更列表缺失，跳过批次三阶段\n";
            passed = false;
        } else {
            // 顺序约束（probe 实测）：变更列表的「首个」点击必达、后续点击可能
            // 丢——故首击直取 md 条目（三色 diff + 内容高亮共用），Tab 点击族
            // 全程可靠。

            // 1) 变更条目 md（首击）→ 右栏真 diff：增/删/上下文三色 + 双行号
            //    + hunk 头（N7→N8 真接线，per-task 后台计算）
            const Pt md = rowCenter(changes, 2);
            if (!clickUntil(md.x, md.y,
                            [&] { return ui->find("git.diff.list") != nullptr; })) {
                std::cerr << "git_page_probe: FAIL——变更条目单击后 diff 视图未出\n";
                passed = false;
            } else {
                std::cout << "  git.diff.list 在场（unified 行模型）\n";
                if (!args.screenshotPath.empty() &&
                    !probe::captureFramePng(window, runtime,
                                            shotPath(args.screenshotPath, "diff"))) {
                    passed = false;
                }
            }
            // 1b) bug#18 取证（--untracked-diff）：untracked 条目 diff 两档。
            // 行序锚（libgit2 status 字节序）：行0=变更组头、行1=a.txt、行2=md、
            // 行3=未跟踪组头、行4='0_' ASCII 前缀超大文件（组内必最先）、
            // 行5=新增文件.txt、行6=长名文件。修复前该单击 = 「无内容变更」空态
            // （git.diff.list/占位元素均不出）→ 本阶段即红绿判别面。
            if (passed && bug18Shots) {
                // clue#60 丢点击对「紧随首击」的连点更敏感：坐标取自行命中元素
                //（virtualList slot=index，offset 0 时 slot.N=行 N）而非 rowH 推算，
                // 并用耐心重试（click + 25 帧 + 150ms ×10，跨过按压捕获死窗）
                const auto bug18ClickUntil = [&](double x, double y,
                                                 const std::function<bool()>& ok) {
                    for (int attempt = 0; attempt < 10; ++attempt) {
                        probe::click(window, runtime, x, y);
                        for (int i = 0; i < 25; ++i) composeFrame();
                        std::this_thread::sleep_for(std::chrono::milliseconds(150));
                        if (ok()) return true;
                    }
                    return false;
                };
                const auto clickChangeRow = [&](int slot,
                                                const std::function<bool()>& ok) {
                    const core::dsl::Element* hit = ui->find(
                        "git.changes.list.slot." + std::to_string(slot) + ".hit");
                    if (hit == nullptr) return false;
                    const Pt p{hit->frame.x + hit->frame.width * 0.5f,
                               hit->frame.y + hit->frame.height * 0.5f};
                    return bug18ClickUntil(p.x, p.y, ok);
                };
                // 档①：≤1MiB 文本 untracked（行5 = 新增文件.txt）→ 合成单 hunk 全增行
                if (!clickChangeRow(
                        5, [&] { return ui->find("git.diff.list") != nullptr; })) {
                    std::cerr << "git_page_probe: FAIL——untracked 小文本 diff 未出全增行"
                                 "（bug#18 空态回归？）\n"
                              << "  诊断：diff.empty="
                              << (ui->find("git.diff.empty") != nullptr)
                              << " diff.placeholder="
                              << (ui->find("git.diff.placeholder") != nullptr)
                              << " diff.binary="
                              << (ui->find("git.diff.binary") != nullptr)
                              << " diff.err=" << (ui->find("git.diff.err") != nullptr)
                              << " right.path="
                              << (ui->find("git.right.path") != nullptr) << "\n";
                    passed = false;
                } else {
                    std::cout << "  untracked 全增行 diff 在场（bug#18 档①）\n";
                    if (!args.screenshotPath.empty() &&
                        !probe::captureFramePng(
                            window, runtime,
                            shotPath(args.screenshotPath, "untracked_add"))) {
                        passed = false;
                    }
                }
                // 档②：>1MiB untracked（行4 = '0_' ASCII 前缀超大文件）→ 占位文案
                if (passed) {
                    if (!clickChangeRow(
                            4, [&] { return ui->find("git.diff.placeholder") != nullptr; })) {
                        std::error_code szEc;
                        const auto bigSz = std::filesystem::file_size(
                            fixture.dir / "0_oversize_placeholder.bin", szEc);
                        std::cerr << "git_page_probe: FAIL——untracked >1MiB 占位未出\n"
                                  << "  诊断：size=" << (szEc ? -1 : static_cast<long long>(bigSz))
                                  << " diff.list=" << (ui->find("git.diff.list") != nullptr)
                                  << " diff.empty=" << (ui->find("git.diff.empty") != nullptr)
                                  << " diff.skel=" << (ui->find("git.diff.skel.0") != nullptr)
                                  << " diff.err=" << (ui->find("git.diff.err.main") != nullptr)
                                  << " diff.binary=" << (ui->find("git.diff.binary") != nullptr)
                                  << "\n";
                        passed = false;
                    } else {
                        std::cout << "  untracked 超大占位在场（bug#18 档②）\n";
                        if (!args.screenshotPath.empty() &&
                            !probe::captureFramePng(
                                window, runtime,
                                shotPath(args.screenshotPath, "untracked_oversize"))) {
                            passed = false;
                        }
                    }
                }
                // 复位选中回 md（行2）：后续内容 Tab 阶段沿用原动线
                if (passed) {
                    if (!clickChangeRow(
                            2, [&] { return ui->find("git.diff.list") != nullptr; })) {
                        std::cerr << "git_page_probe: FAIL——复位 md 选中未回 diff\n";
                        passed = false;
                    }
                }
            }
            // 2) 内容 Tab → md 高亮内容（标题/行内码/围栏 java 关键字）
            if (passed) {
                const core::dsl::Element* contentTab =
                    ui->find("git.right.tab.content.hit");
                if (contentTab == nullptr) {
                    std::cerr << "git_page_probe: FAIL——内容 Tab 缺失\n";
                    passed = false;
                } else {
                    const Pt tab{contentTab->frame.x + contentTab->frame.width * 0.5f,
                                 contentTab->frame.y + contentTab->frame.height * 0.5f};
                    if (!clickUntil(tab.x, tab.y, [&] {
                            return ui->find("git.content.list") != nullptr;
                        })) {
                        std::cerr << "git_page_probe: FAIL——md 内容视图未出\n";
                        passed = false;
                    } else {
                        std::cout << "  git.content.list 在场（md 高亮）\n";
                        if (!args.screenshotPath.empty() &&
                            !probe::captureFramePng(
                                window, runtime,
                                shotPath(args.screenshotPath, "content_md"))) {
                            passed = false;
                        }
                    }
                }
            }
        }
    }

    // ②c 批次四（need:11）：F-13 刷新联动——外部改写打开中的文件 → watcher 置脏
    // → 过期条「内容已过期，点击重载」→ 点击重载恢复（当前右栏 = md 内容视图，
    // ②b 末态）。watcher debounce 300ms + 1s 节流 + worker：泵帧与真实等待交替。
    if (passed && !noOpen && externalRepo.empty()) {  // ②c 针对 fixture 仓文件，外部仓跳过
        const auto mdPath = fixture.dir / platform::git::utf8ToPath("说明文档") /
                            platform::git::utf8ToPath("接口规范.md");
        {
            std::ofstream ofs(mdPath, std::ios::binary);
            ofs << "# 接口规范 v3（外部编辑器改写）\n内容\n";
        }
        stage("external edit written; pump for watcher debounce+1s throttle");
        for (int i = 0; i < 40; ++i) {
            composeFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const core::dsl::Element* stale = ui->find("git.right.stale");
        if (stale == nullptr) {
            std::cerr << "git_page_probe: FAIL——外部改写打开文件后过期条未出"
                         "（watcher→过滤→节流链未生效）\n";
            passed = false;
        } else {
            std::cout << "  git.right.stale 在场（F-13③ 过期条）\n";
            if (!args.screenshotPath.empty() &&
                !probe::captureFramePng(window, runtime,
                                        shotPath(args.screenshotPath, "stale"))) {
                passed = false;
            }
            const Pt staleHit{stale->frame.x + stale->frame.width * 0.5f,
                              stale->frame.y + stale->frame.height * 0.5f};
            if (!clickUntil(staleHit.x, staleHit.y,
                            [&] { return ui->find("git.right.stale") == nullptr; })) {
                std::cerr << "git_page_probe: FAIL——过期条点击重载未消失\n";
                passed = false;
            } else {
                std::cout << "  过期条点击重载后消失（重载完成）\n";
            }
        }
    }

    // ②d 批次四（need:11）：F-07⑥ 真横滚——a.txt diff 含 1500 字符超长行，
    // 拖底部横向滚动条 thumb → 代码列整体平移（行号列固定），断言行内容元素
    // frame.x 左移（不换行错位的几何证据）。
    if (passed && !noOpen && externalRepo.empty()) {  // ②d 横滚依赖 fixture 长行，外部仓跳过
        const core::dsl::Element* changes = ui->find("git.changes.list");
        const core::dsl::Element* hscrollProbe = ui->find("git.diff.list");
        (void)hscrollProbe;
        if (changes == nullptr) {
            std::cerr << "git_page_probe: FAIL——变更列表缺失，跳过横滚阶段\n";
            passed = false;
        } else {
            const Pt aRow = rowCenter(changes, 1);  // 组头后首条 = a.txt
            if (!clickUntil(aRow.x, aRow.y,
                            [&] { return ui->find("git.diff.list") != nullptr; })) {
                std::cerr << "git_page_probe: FAIL——a.txt diff 未出（横滚前置失败）\n";
                passed = false;
            } else {
                const core::dsl::Element* thumb =
                    ui->find("git.diff.list.h.thumb");
                if (thumb == nullptr) {
                    std::cerr << "git_page_probe: FAIL——超长行 diff 未出横向滚动条"
                                 "（git.diff.list.hscroll.thumb 缺失）\n";
                    passed = false;
                } else {
                    const core::dsl::Element* codeBefore =
                        ui->find("git.diff.list.slot.1.code");
                    const double beforeX =
                        codeBefore != nullptr
                            ? static_cast<double>(codeBefore->frame.x)
                            : -1.0;
                    stage("drag hscroll thumb");
                    probe::drag(window, runtime,
                                static_cast<double>(thumb->frame.x +
                                                    thumb->frame.width * 0.5f),
                                static_cast<double>(thumb->frame.y +
                                                    thumb->frame.height * 0.5f),
                                static_cast<double>(thumb->frame.x) + 160.0,
                                static_cast<double>(thumb->frame.y +
                                                    thumb->frame.height * 0.5f),
                                10);
                    for (int i = 0; i < 20; ++i) composeFrame();
                    const core::dsl::Element* codeAfter =
                        ui->find("git.diff.list.slot.1.code");
                    const double afterX =
                        codeAfter != nullptr
                            ? static_cast<double>(codeAfter->frame.x)
                            : -1.0;
                    std::cout << "  横滚拖动后 diff 行内容 x：" << beforeX
                              << " → " << afterX << "\n";
                    if (codeAfter == nullptr || afterX >= beforeX - 40.0) {
                        std::cerr << "git_page_probe: FAIL——拖横滚后代码列未平移"
                                     "（x 位移不足）\n";
                        passed = false;
                    } else if (!args.screenshotPath.empty() &&
                               !probe::captureFramePng(
                                   window, runtime,
                                   shotPath(args.screenshotPath, "hscroll"))) {
                        passed = false;
                    }
                }
            }
        }
    }

    // ②e qa12 F-09①：大仓变更列表滚动不冻结——滚轮注入 10 步 + 100 帧泵，
    // 墙钟 30s 时限（超时=冻结）；滚动后截图供目视核对（滚轮为窗口级注入，
    // 落靶列表以截图为准，如实记录）。
    if (passed && !externalRepo.empty()) {
        const core::dsl::Element* cl = ui->find("git.changes.list");
        if (cl == nullptr) {
            std::cerr << "git_page_probe: FAIL——大仓变更列表缺失\n";
            passed = false;
        } else {
            stage("F-09 scroll probe begin");
            const auto s0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 10; ++i) {
                probe::queueScroll(window, runtime, 0.0, -3.0);
                for (int f = 0; f < 10; ++f) composeFrame();
            }
            const auto scrollMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - s0)
                    .count();
            stage("F-09 scroll probe done in " + std::to_string(scrollMs) +
                  " ms");
            std::cout << "  F-09 滚动取证：10 步滚轮 + 100 帧泵 " << scrollMs
                      << " ms\n";
            if (scrollMs > 30000 || ui->find("git.changes.list") == nullptr) {
                std::cerr << "git_page_probe: FAIL——大仓列表滚动冻结或列表消失\n";
                passed = false;
            } else if (!args.screenshotPath.empty() &&
                       !probe::captureFramePng(
                           window, runtime,
                           shotPath(args.screenshotPath, "changes_scrolled"))) {
                passed = false;
            }
        }
    }

    // ②f need:14 全局搜索（F-08①②④ 取证；--search）：真实键入（点击聚焦 →
    // queueText）→ Enter 提交 → 结果浮层（计数头 + match 高亮 runs）→ 点击命中
    // 行定位（右栏内容视图 + 命中行常亮底色）→ ESC 显式取消 → 空结果态。
    if (passed && !noOpen && searchShots) {
        const core::dsl::Element* box = ui->find("git.search.input");
        if (box == nullptr) {
            std::cerr << "git_page_probe: FAIL——顶栏搜索框缺失\n";
            passed = false;
        } else {
            const Pt boxC{static_cast<double>(box->frame.x + 30.0f),
                          static_cast<double>(box->frame.y + box->frame.height * 0.5f)};
            const auto pumpFor = [&](int frames, int ms = 0) {
                for (int i = 0; i < frames; ++i) {
                    composeFrame();
                    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                }
            };
            const auto typeAndEnter = [&](const std::string& text) {
                probe::click(window, runtime, boxC.x, boxC.y);
                pumpFor(5);
                probe::queueText(window, runtime, text);
                pumpFor(5);
                probe::queueKey(window, runtime, GLFW_KEY_ENTER, core::KeyAction::Press, 0);
                probe::pollOnce(window, runtime);
                probe::queueKey(window, runtime, GLFW_KEY_ENTER, core::KeyAction::Release, 0);
                probe::pollOnce(window, runtime);
            };

            // 档①：命中「第二版」（a.txt 行1）→ 浮层结果态
            typeAndEnter("第二版");
            bool popup = false;
            for (int i = 0; i < 120 && !popup; ++i) {
                composeFrame();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                popup = ui->find("git.search.popup") != nullptr;
            }
            if (!popup) {
                std::cerr << "git_page_probe: FAIL——搜索提交后浮层未出\n";
                passed = false;
            } else {
                pumpFor(30, 30);  // 稳帧 ≥24（worker 一包收敛 + 过渡态落定）
                if (ui->find("git.search.results") == nullptr) {
                    std::cerr << "git_page_probe: FAIL——命中浮层无结果列表（git.search.results 缺失）\n";
                    passed = false;
                } else {
                    std::cout << "  git.search.results 在场（F-08① 结果浮层）\n";
                    if (!args.screenshotPath.empty() &&
                        !probe::captureFramePng(
                            window, runtime,
                            shotPath(args.screenshotPath, "search_results"))) {
                        passed = false;
                    }
                    // 档②：点击首条命中行 → 右栏内容视图 + 命中行常亮（F-08②）
                    const core::dsl::Element* hitRow =
                        ui->find("git.search.results.slot.0.hit");
                    if (hitRow == nullptr) {
                        std::cerr << "git_page_probe: FAIL——浮层命中行缺失\n";
                        passed = false;
                    } else {
                        const Pt rowP{hitRow->frame.x + hitRow->frame.width * 0.5f,
                                      hitRow->frame.y + hitRow->frame.height * 0.5f};
                        if (!clickUntil(rowP.x, rowP.y, [&] {
                                return ui->find("git.content.list") != nullptr &&
                                       ui->find("git.content.list.slot.0.hlbg") !=
                                           nullptr;
                            })) {
                            std::cerr << "git_page_probe: FAIL——点击命中行后内容视图/命中行常亮未出\n";
                            passed = false;
                        } else {
                            std::cout << "  命中行定位 + 常亮底色在场（F-08②）\n";
                            if (!args.screenshotPath.empty() &&
                                !probe::captureFramePng(
                                    window, runtime,
                                    shotPath(args.screenshotPath, "search_jump"))) {
                                passed = false;
                            }
                        }
                    }
                }
                // 档③：ESC 显式取消 → 浮层消失（F-08④）
                typeAndEnter("");  // 仅点击聚焦（空文本不触发 onChange 提交）
                probe::queueKey(window, runtime, GLFW_KEY_ESCAPE,
                                core::KeyAction::Press, 0);
                probe::pollOnce(window, runtime);
                probe::queueKey(window, runtime, GLFW_KEY_ESCAPE,
                                core::KeyAction::Release, 0);
                probe::pollOnce(window, runtime);
                pumpFor(30);
                if (ui->find("git.search.popup") != nullptr) {
                    std::cerr << "git_page_probe: FAIL——ESC 后浮层未消失（取消语义失效）\n";
                    passed = false;
                } else {
                    std::cout << "  ESC 取消浮层消失（F-08④）\n";
                }
                // 档④：空结果态
                typeAndEnter("zzz__no_such_token__");
                bool emptyPopup = false;
                for (int i = 0; i < 120 && !emptyPopup; ++i) {
                    composeFrame();
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    emptyPopup = ui->find("git.search.popup.empty") != nullptr;
                }
                if (!emptyPopup) {
                    std::cerr << "git_page_probe: FAIL——空结果态未出\n";
                    passed = false;
                } else {
                    std::cout << "  空结果态在场\n";
                    if (!args.screenshotPath.empty() &&
                        !probe::captureFramePng(
                            window, runtime,
                            shotPath(args.screenshotPath, "search_empty"))) {
                        passed = false;
                    }
                }
                // 收尾复位：ESC 回无搜索态
                probe::queueKey(window, runtime, GLFW_KEY_ESCAPE,
                                core::KeyAction::Press, 0);
                probe::pollOnce(window, runtime);
                probe::queueKey(window, runtime, GLFW_KEY_ESCAPE,
                                core::KeyAction::Release, 0);
                probe::pollOnce(window, runtime);
                pumpFor(10);
            }
        }
    }

    // ③ 截图出口：--screenshot 主界面（当前主题档）
    if (!args.screenshotPath.empty() &&
        !probe::captureFramePng(window, runtime, args.screenshotPath)) {
        std::cerr << "git_page_probe: 主界面落图失败\n";
        passed = false;
    }

    mod->onStop();
    ::platform::folderpicker::setPickerForTest(nullptr);
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();

    if (!passed) {
        std::cerr << "FAIL\n";
        return 1;
    }
    std::cout << "git_page_probe: PASS（" << args.windowW << "x" << args.windowH
              << "）\n";
    return 0;
}
