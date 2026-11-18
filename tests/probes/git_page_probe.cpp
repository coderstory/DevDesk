// git_page_probe.cpp — Git 管理器页截图探针（need:299/300 打磨轮重写：
// 旧场景层引用已删的搜索框/仅修改 tab/三栏树页，随 d61cbd56 重设计整体陈旧失效）。
//
// 运行：git_page_probe.exe --screenshot <路径> [--size WxH]
// 取证面（设计文档 docs/design/2026-11-17-git管理器重设计.md v3）：
//   ①空态页 ②两栏主界面（树）③平铺 ④过滤激活/无匹配 ⑤干净仓空态 ⑥仓下拉
//   （3 仓含 1 失效）⑦diff/内容 ⑧组头折叠 ⑨左栏折叠 ⑩untracked 懒展开
//   ⑪失效仓重开错误块；--size 1200x720 附加窄窗档。
// 边界断言：徽标字母/长名省略/深嵌套缩进钳制/组头计数只算文件行/untracked 自动
//   枚举/切仓清过滤/刷新保过滤/×与 Esc 清空。
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
#include <exception>
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

// fixture 仓（git2 API 直建，离线零 CLI）：files 全量入库并 commit（单提交基线）
struct FixtureRepo {
    std::filesystem::path dir;
    bool ok = false;
};

FixtureRepo makeRepo(const std::filesystem::path& root,
                     const std::string& name,
                     const std::vector<std::pair<std::string, std::string>>& files) {
    FixtureRepo out;
    out.dir = root / name;
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
    for (const auto& f : files) {
        const auto p = out.dir / platform::git::utf8ToPath(f.first);
        std::filesystem::create_directories(p.parent_path());
        std::ofstream ofs(p, std::ios::binary);
        ofs << f.second;
    }
    if (!checkRc(git_repository_open(&repo, platform::git::pathToUtf8(out.dir).c_str()),
                 "reopen")) {
        return out;
    }
    git_index* index = nullptr;
    git_repository_index(&index, repo);
    for (const auto& f : files) {
        git_index_add_bypath(index, f.first.c_str());
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
    out.ok = true;
    return out;
}

// 主仓工作区变更面：修改（含 12 级深嵌套链/长名）+ 重命名 + staged 一条 +
// untracked（根文件 + 目录首层文件 + 嵌套子目录）。修改落盘 + staged 文件进 index。
bool applyWorkdirChanges(const FixtureRepo& repo) {
    std::error_code ec;
    const auto write = [](const std::filesystem::path& p,
                          const std::string& body) {
        std::ofstream ofs(p, std::ios::binary);
        ofs << body;
    };
    // 修改（unstaged M）
    write(repo.dir / "a.txt", "第二版\n");
    write(repo.dir / platform::git::utf8ToPath("说明文档") /
              platform::git::utf8ToPath("指南.md"),
          "# 指南 v2\n正文\n补充段\n");
    std::filesystem::path deep = repo.dir / "deep";
    for (int i = 1; i <= 12; ++i) deep = deep / ("n" + std::to_string(i));
    std::filesystem::create_directories(deep, ec);
    write(deep / "verydeep.txt", "深层 v2\n");
    // 嵌套链逐级改写（每级直接文件断开单链压缩 → 深度目录行真实存在）
    {
        std::filesystem::path chainDir = repo.dir;
        for (int i = 0; i <= 11; ++i) {
            chainDir = chainDir / ("a" + (i == 0 ? std::string("0")
                                                 : std::to_string(i)));
            std::filesystem::create_directories(chainDir, ec);
            write(chainDir / ("m" + std::to_string(i) + ".txt"),
                  "分叉链 v2 " + std::to_string(i) + "\n");
        }
    }
    write(repo.dir / platform::git::utf8ToPath(
              "长名验证_这是一个非常长的中文文件名用来触发列表行省略号截断以及悬浮提示完整路径显示的边界用例.txt"),
          "长名第二版\n");
    // 重命名（R：双侧 rename 检测，StatusEntry.oldPath 通道）
    std::filesystem::rename(
        repo.dir / platform::git::utf8ToPath("旧名_待重命名文档.txt"),
        repo.dir / platform::git::utf8ToPath("新名_已重命名文档.txt"), ec);
    if (ec) {
        std::cerr << "git_page_probe: fixture rename 失败: " << ec.message() << "\n";
        return false;
    }
    // staged：接口文档改写「先落盘后入 index」（顺序颠倒会退化成 unstaged）
    write(repo.dir / platform::git::utf8ToPath("接口文档.md"),
          "# 接口 v2（已暂存）\n内容\n");
    {
        git_repository* r = nullptr;
        if (git_repository_open(&r, platform::git::pathToUtf8(repo.dir).c_str()) < 0) {
            return false;
        }
        git_index* index = nullptr;
        git_repository_index(&index, r);
        git_index_add_bypath(index, "接口文档.md");
        git_index_write(index);
        git_index_free(index);
        git_repository_free(r);
    }
    // untracked：根文件 + 目录（首层文件自动枚举并入 + 嵌套子目录懒展开）。
    // 目录名「0_」前缀 = 字节序排最前 → 懒锚行在视口内可点（无需先折叠深链）
    write(repo.dir / platform::git::utf8ToPath("新增文件.txt"), "untracked\n");
    std::filesystem::create_directories(repo.dir / "0_untracked" / "nested", ec);
    write(repo.dir / "0_untracked" / "inner1.txt", "首层文件\n");
    write(repo.dir / "0_untracked" / "nested" / "nested_file.txt", "嵌套文件\n");
    return true;
}

// 截图路径派生：<base>_<tag>.png
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
    const bool narrowRun = args.windowW <= 1250;  // --size 1200x720 窄窗档短跑
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("git_page_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    // fixture 建仓先于模块 onStart：libgit2 全局 init 独立拿一份 refcount
    const ::platform::git::Libgit2Scope fixtureScope;

    // 数据根隔离（git_manager.json 记忆仓写入不落真实用户目录）
    const auto dataRoot = test_support::makeTempDir("devdesk_git_probe_", "data");
    test_support::ScopedEnv dataRootEnv("DEVDESK_DATA_ROOT",
                                        platform::git::pathToUtf8(dataRoot).c_str());

    // 深嵌套链基线：deep/n1..n12/verydeep.txt（单链压缩取证面）+
    // a0/a1/.../a11 嵌套链（每级一个直接修改文件 m{i}.txt 断开单链压缩 →
    // 真实 depth 10/11 目录行 → §4.4 缩进钳制取证面）
    std::string deepRel = "deep";
    for (int i = 1; i <= 12; ++i) deepRel += "/n" + std::to_string(i);
    deepRel += "/verydeep.txt";
    std::vector<std::pair<std::string, std::string>> mainFiles = {
        {".gitignore", "build/\n"},
        {"a.txt", "第一版\n"},
        {"接口文档.md", "# 接口\n内容\n"},
        {"说明文档/指南.md", "# 指南\n正文\n"},
        {deepRel, "深层\n"},
        {"旧名_待重命名文档.txt", "旧内容\n"},
        {"长名验证_这是一个非常长的中文文件名用来触发列表行省略号截断以及悬浮提示完整路径显示的边界用例.txt",
         "长名第一版\n"},
    };
    {
        std::string chain;
        for (int i = 0; i <= 11; ++i) {
            chain += (i == 0) ? "a0" : ("/a" + std::to_string(i));
            mainFiles.emplace_back(chain + "/m" + std::to_string(i) + ".txt",
                                   "分叉链 v1 " + std::to_string(i) + "\n");
        }
    }
    const FixtureRepo repoA = makeRepo(dataRoot, "示例仓库", mainFiles);
    const FixtureRepo repoB = makeRepo(
        dataRoot, "干净仓库",
        {{"readme.md", "# 干净\n"}, {"lib/core.txt", "core\n"}});
    const FixtureRepo repoC = makeRepo(dataRoot, "失效仓库",
                                       {{"gone.txt", "将被删除\n"}});
    if (!repoA.ok || !repoB.ok || !repoC.ok || !applyWorkdirChanges(repoA)) {
        std::cerr << "git_page_probe: fixture 仓创建失败\n";
        probe::terminateWindowBackend();
        return 2;
    }
    // 预置最近仓历史：假失效仓（目录从未存在）→ 下拉「已失效」行 + 重开失败块
    // 取证。免删真仓（Windows 句柄延迟释放，remove_all 实测挂住不干净）。
    // lastRepoPath 留空 → onStart 不自动开仓，空态页取证面保留。
    {
        const std::string fakeRepo =
            platform::git::pathToUtf8(dataRoot / "假失效仓库");
        std::string js = "{\"lastRepoPath\":\"\",\"recentRepos\":[\"";
        for (char c : fakeRepo) {
            if (c == '\\') js += "\\\\";
            else js += c;
        }
        js += "\"]}";
        std::ofstream ofs(dataRoot / "git_manager.json", std::ios::binary);
        ofs << js;
    }

    // 选仓缝注入：空态页主按钮/下拉「浏览目录…」→ 返回 pickedRepo（不弹系统对话框）
    static std::string pickedRepo;
    pickedRepo = platform::git::pathToUtf8(repoA.dir);
    ::platform::folderpicker::setPickerForTest(
        +[](const std::string&, const std::string&) -> core::Result<std::string> {
            return core::Result<std::string>::ok(pickedRepo);
        });

    std::unique_ptr<::core::Module> mod(create_git_module());
    mod->onStart();

    core::dsl::Runtime runtime;
    runtime.initialize(window);
    // need299 打磨轮取证默认浅色档（设计文档 token 表为浅色基线；视觉评审对照面）
    core::theme::setLight();

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
    const auto pumpFor = [&](int frames, int ms = 0) {
        for (int i = 0; i < frames; ++i) {
            composeFrame();
            if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
    };
    const auto pumpUntil = [&](const std::function<bool()>& cond, int maxMs) {
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            composeFrame();
            if (cond()) return true;
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
            if (ms > maxMs) return cond();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    };

    // ---- 行/组头文本锚点定位（bug#280 口径：text 子串匹配，virtualList slot 序）----
    struct RowRef {
        const core::dsl::Element* name = nullptr;  // .name 元素（含 frame 几何）
        std::string base;                          // 行 id 前缀（拼 .badge/.hit/.tip）
    };
    const auto findRow = [&](const std::string& namePart) -> RowRef {
        for (int i = 0; i < 64; ++i) {
            const std::string base =
                "git.changes.list.slot." + std::to_string(i);
            const core::dsl::Element* n = ui->find(base + ".name");
            if (n != nullptr && n->text.find(namePart) != std::string::npos) {
                return {n, base};
            }
        }
        return {};
    };
    // 目录行定位：目录名恒以「/」收尾（文件短名不含）——「deep」子串会同时命中
    // verydeep.txt 文件行，目录断言必须用本助手
    const auto findDirRow = [&](const std::string& namePart) -> RowRef {
        for (int i = 0; i < 64; ++i) {
            const std::string base =
                "git.changes.list.slot." + std::to_string(i);
            const core::dsl::Element* n = ui->find(base + ".name");
            if (n != nullptr && n->text.find(namePart) != std::string::npos &&
                !n->text.empty() && n->text.back() == '/') {
                return {n, base};
            }
        }
        return {};
    };
    const auto findGroupHeader = [&](const std::string& prefix)
        -> const core::dsl::Element* {
        for (int i = 0; i < 64; ++i) {
            const core::dsl::Element* t = ui->find(
                "git.changes.list.slot." + std::to_string(i) + ".text");
            if (t != nullptr && t->text.rfind(prefix, 0) == 0) return t;
        }
        return nullptr;
    };
    // 点击重试（clue#60：virtualList 注入点击可能丢——一律 clickUntil 重试模式）
    const auto clickEl = [&](const core::dsl::Element* e) {
        if (e == nullptr) return;
        probe::click(window, runtime,
                     static_cast<double>(e->frame.x + e->frame.width * 0.5f),
                     static_cast<double>(e->frame.y + e->frame.height * 0.5f));
    };
    const auto clickUntil = [&](const core::dsl::Element* e,
                                const std::function<bool()>& ok) {
        if (e == nullptr) return false;
        for (int attempt = 0; attempt < 6; ++attempt) {
            clickEl(e);
            pumpFor(25);
            if (ok()) return true;
        }
        return false;
    };
    const auto shot = [&](const char* tag) {
        if (args.screenshotPath.empty()) return true;
        stage(std::string("shot ") + tag);
        return probe::captureFramePng(window, runtime,
                                      shotPath(args.screenshotPath, tag));
    };
    const auto expect = [&](bool cond, const std::string& what) {
        if (!cond) {
            std::cerr << "git_page_probe: FAIL——" << what << "\n";
            passed = false;
        } else {
            std::cout << "  ok " << what << "\n";
        }
        return cond;
    };
    const auto typeIntoFilter = [&](const std::string& text) {
        const core::dsl::Element* box = ui->find("git.list.filter");
        if (box == nullptr) return;
        clickEl(box);
        pumpFor(5);
        probe::queueText(window, runtime, text);
        pumpFor(8);
    };
    const auto pressEsc = [&]() {
        probe::queueKey(window, runtime, GLFW_KEY_ESCAPE, core::KeyAction::Press, 0);
        probe::pollOnce(window, runtime);
        probe::queueKey(window, runtime, GLFW_KEY_ESCAPE, core::KeyAction::Release, 0);
        probe::pollOnce(window, runtime);
        pumpFor(10);
    };

    // ① 空态页（无记忆首启；fresh 数据根）
    composeFrame();
    composeFrame();
    expect(ui->find("git.empty.title") != nullptr, "空态页出（git.empty.title）");
    shot("01_empty_page");

    // ② 点主按钮 → 开主仓 → 泵到 worker 链收敛（validate→open→status→untracked 枚举）
    const core::dsl::Element* pick = ui->find("git.empty.pick.hit");
    if (pick == nullptr) {
        std::cerr << "git_page_probe: FAIL——空态页主按钮缺失\n";
        passed = false;
    } else {
        stage("click pick button");
        clickEl(pick);
        if (!pumpUntil([&] { return ui->find("git.changes.list") != nullptr; },
                       20000)) {
            std::cerr << "git_page_probe: FAIL——20s 内主界面未收敛\n";
            passed = false;
        } else {
            // untracked 顶层自动枚举落地（裁决④ worker 往返）：等首层文件行出现
            pumpUntil([&] { return findRow("inner1.txt").name != nullptr; }, 10000);
            const core::dsl::Element* topName = ui->find("git.top.name");
            const core::dsl::Element* listBg = ui->find("git.list.bg");
            expect(topName != nullptr && topName->text == "示例仓库",
                   "顶栏仓名=示例仓库");
            expect(listBg != nullptr &&
                       std::fabs(listBg->frame.width -
                                 (args.windowW < 1200 ? 280.0f : 320.0f)) <= 2.0f,
                   "左卡宽 " + std::to_string(args.windowW < 1200 ? 280 : 320) +
                       "（§9 档位，实测 " +
                       std::to_string(listBg ? listBg->frame.width : -1.0f) + "）");
            expect(ui->find("git.top.switch") != nullptr &&
                       ui->find("git.top.refresh") != nullptr,
                   "顶栏 [切换仓库][刷新] 次钮在场");

            if (!narrowRun) {
                // ---- 主跑全场景 ----
                // ③ 树形态主界面：untracked 自动枚举（裁决④）+ 徽标字母 + 长名省略
                const RowRef inner = findRow("inner1.txt");
                expect(inner.name != nullptr, "untracked 首层文件行 inner1.txt 自动并入");
                expect(!inner.base.empty() &&
                           ui->find(inner.base + ".badge.text") != nullptr &&
                           ui->find(inner.base + ".badge.text")->text == "U",
                       "inner1.txt 带 U 徽标");
                const RowRef nested = findRow("nested");
                expect(nested.name != nullptr, "嵌套子目录 nested=派生懒目录行");
                expect(!nested.base.empty() &&
                           ui->find(nested.base + ".badge") == nullptr,
                       "目录行不带徽标（原则令：纯结构装置）");
                const RowRef aRow = findRow("a.txt");
                expect(!aRow.base.empty() &&
                           ui->find(aRow.base + ".badge.text") != nullptr &&
                           ui->find(aRow.base + ".badge.text")->text == "M",
                       "a.txt 带 M 徽标");
                // 组头计数（原则令：只算文件行；目录行不计）
                const core::dsl::Element* stagedH = findGroupHeader("已暂存");
                const core::dsl::Element* unstagedH = findGroupHeader("未暂存");
                expect(stagedH != nullptr &&
                           stagedH->text.find("(1)") != std::string::npos,
                       "组头「已暂存 (1)」");
                expect(unstagedH != nullptr &&
                           unstagedH->text.find("(19)") != std::string::npos,
                       "组头「未暂存 (19)」只算文件行（目录行不计；实测 " +
                           (unstagedH != nullptr ? unstagedH->text : "?") + "）");
                // 诊断（need299 走查）：rename 检测出 R(new) 后旧名是否残留 D 行
                //（platform 双侧 rename 口径核验；残留=平台行为 finding，非页面缺陷）
                const RowRef oldNameRow = findRow("旧名_待重命名文档.txt");
                std::cout << "  [diag] 旧名行残留="
                          << (oldNameRow.name != nullptr ? "有(" : "无")
                          << (oldNameRow.base.empty()
                                  ? ""
                                  : ui->find(oldNameRow.base + ".badge.text") != nullptr
                                      ? ui->find(oldNameRow.base + ".badge.text")->text
                                      : "")
                          << (oldNameRow.name != nullptr ? ")" : "") << "\n";
                shot("02_main_tree");

                // ④ 深嵌套缩进钳制（§4.4：名区最小 96px，超限后不再增缩进）。
                // a0..a12 分叉链（每级直接文件断开单链压缩）出真实 depth 11/12 行；
                // 先折叠已暂存组腾视口（钳制行入画面取证），断言后恢复
                const core::dsl::Element* shFold = findGroupHeader("已暂存");
                clickUntil(shFold, [&] { return findRow("接口文档.md").name == nullptr; });
                const RowRef a10 = findDirRow("a10");
                const RowRef a11 = findDirRow("a11");
                const RowRef m11 = findRow("m11.txt");
                if (expect(a10.name != nullptr && a11.name != nullptr &&
                               m11.name != nullptr,
                           "深链目录行 a10/a11 与子文件 m11 在场（链不压缩）")) {
                    // 目录行缩进锚 = .arrow 左缘；文件行缩进锚 = .badge 左缘
                    //（两者都 = 行缩进值 + 列表 x）——同 depth 必同 x
                    const core::dsl::Element* ar10 = ui->find(a10.base + ".arrow");
                    const core::dsl::Element* ar11 = ui->find(a11.base + ".arrow");
                    const core::dsl::Element* bg11 = ui->find(m11.base + ".badge");
                    const float x10 = ar10 != nullptr ? ar10->frame.x : -1.0f;
                    const float x11 = ar11 != nullptr ? ar11->frame.x : -1.0f;
                    const float xm11 = bg11 != nullptr ? bg11->frame.x : -1.0f;
                    std::cout << "  缩进 x(a10)=" << x10 << " x(a11)=" << x11
                              << " x(m11 badge)=" << xm11 << "\n";
                    expect(x11 > 0 && std::fabs(x11 - xm11) <= 1.0,
                           "缩进钳制：a11 与 m11 同位（depth 11 不再增缩进）");
                    expect(x10 < x11 - 0.5,
                           "钳制前逐级缩进仍递增（a10 < a11）");
                    shot("16_indent_clamp");
                }
                // 恢复已暂存组展开
                const core::dsl::Element* shUnfold = findGroupHeader("已暂存");
                clickUntil(shUnfold,
                           [&] { return findRow("接口文档.md").name != nullptr; });

                // ⑤ untracked 嵌套懒展开（F-04：点击→worker walk→子行；
                // 0_untracked/ 字节序排最前 → nested 锚行在视口内可直接点击）
                const RowRef nestedRow = findDirRow("nested");
                if (!expect(nestedRow.name != nullptr, "nested 懒目录行可见")) {
                } else if (!clickUntil(nestedRow.name, [&] {
                               return findRow("nested_file.txt").name != nullptr;
                           })) {
                    expect(false, "nested 懒展开出 nested_file.txt");
                } else {
                    const RowRef nf = findRow("nested_file.txt");
                    expect(ui->find(nf.base + ".badge.text") != nullptr &&
                               ui->find(nf.base + ".badge.text")->text == "U",
                           "nested_file.txt 带 U 徽标（懒枚举并入）");
                    const core::dsl::Element* uh = findGroupHeader("未暂存");
                    expect(uh != nullptr &&
                               uh->text.find("(20)") != std::string::npos,
                           "懒枚举后组头计数 19→20（枚举文件计入；实测 " +
                               (uh != nullptr ? uh->text : "?") + "）");
                }
                shot("11_untracked_lazy");

                // ⑦ 组头折叠/展开（折叠=行集直切；选中与右栏不受影响）
                const core::dsl::Element* sh = findGroupHeader("已暂存");
                if (!clickUntil(sh, [&] { return findRow("接口文档.md").name == nullptr; })) {
                    expect(false, "点组头折叠已暂存组（接口文档.md 行隐藏）");
                } else {
                    shot("12_group_folded");
                    const core::dsl::Element* sh2 = findGroupHeader("已暂存");
                    if (!clickUntil(sh2,
                                    [&] { return findRow("接口文档.md").name != nullptr; })) {
                        expect(false, "再点组头恢复已暂存组");
                    }
                }

                // ⑧ 平铺形态（D1-C：segmented 切换；tracked 目录行消失+平铺目录段）
                const core::dsl::Element* flatBtn = ui->find("git.list.btn.flat.hit");
                if (!clickUntil(flatBtn, [&] {
                        return findDirRow("deep").name == nullptr &&
                               findDirRow("说明文档").name == nullptr;
                    })) {
                    expect(false, "平铺切换后 tracked 目录行消失");
                } else {
                    // 平铺目录弱化段：根文件 a.txt 无目录段（dirSeg 空），改取
                    // 嵌套文件 指南.md（目录段=说明文档）
                    const RowRef flatGuide = findRow("指南.md");
                    expect(!flatGuide.base.empty() &&
                               ui->find(flatGuide.base + ".dir") != nullptr,
                           "平铺嵌套文件行带目录弱化段（.dir）");
                    expect(findRow("verydeep.txt").name != nullptr,
                           "平铺：深链文件行在场（路径归并同目录相邻）");
                    // R 徽标 / 长名省略（平铺行数少 → 目标行在物化窗内可断言；
                    // 树形态同机制，树段已断言 U/M 徽标与省略机制本身）
                    const RowRef rRow = findRow("新名_已重命名文档.txt");
                    expect(!rRow.base.empty() &&
                               ui->find(rRow.base + ".badge.text") != nullptr &&
                               ui->find(rRow.base + ".badge.text")->text == "R",
                           "重命名行带 R 徽标（rename 检测）");
                    expect(!rRow.base.empty() &&
                               ui->find(rRow.base + ".rename.tip") != nullptr,
                           "重命名行挂 old→new tooltip");
                    const RowRef longRow = findRow("长名验证_");
                    const std::string& lt = longRow.name != nullptr
                                                ? longRow.name->text
                                                : std::string();
                    expect(lt.size() >= 3 &&
                               lt.compare(lt.size() - 3, 3, "\xE2\x80\xA6") == 0,
                           "长名行以省略号收尾（码点截断）");
                    expect(!longRow.base.empty() &&
                               ui->find(longRow.base + ".name.tip") != nullptr,
                           "长名行挂全路径 tooltip");
                    shot("03_flat");
                }

                // ⑨ 过滤（§4.9 全路径子串）：激活/无匹配/×清空/Esc 清空/刷新保留
                typeIntoFilter("指南");
                expect(findRow("指南.md").name != nullptr && findRow("a.txt").name == nullptr,
                       "过滤「指南」命中说明文档/指南.md（全路径子串）");
                shot("04_filter_active");
                typeIntoFilter("指南zzz");
                expect(ui->find("git.list.empty.message") != nullptr &&
                           ui->find("git.list.empty.guide") != nullptr,
                       "无匹配空态主句+引导（与干净空态严格区分）");
                expect(findGroupHeader("未暂存") == nullptr,
                       "过滤无匹配=组头临时隐藏");
                shot("05_filter_empty");
                const core::dsl::Element* clearBtn = ui->find("git.list.filter.clear.hit");
                if (!clickUntil(clearBtn,
                                [&] { return findRow("a.txt").name != nullptr; })) {
                    expect(false, "× 清空恢复全列表");
                }
                // Esc 清空（§6.9：输入框内 Esc=清空不冒泡）
                typeIntoFilter("指南");
                pressEsc();
                expect(findRow("a.txt").name != nullptr,
                       "输入框内 Esc 清空恢复全列表");
                // 刷新保留过滤词并重应用（§4.9 跨操作保持）
                typeIntoFilter("指南");
                clickEl(ui->find("git.top.refresh.hit"));
                pumpFor(60, 50);  // 静默刷新（1s 节流+worker）
                expect(findRow("指南.md").name != nullptr &&
                           findRow("a.txt").name == nullptr,
                       "刷新后过滤词保留并重应用");
                clickEl(ui->find("git.list.filter.clear.hit"));
                pumpFor(10);

                // ⑩ 切仓（下拉「浏览目录…」）→ 过滤词清空（§4.9：切仓清空）
                pickedRepo = platform::git::pathToUtf8(repoB.dir);
                const core::dsl::Element* switchBtn = ui->find("git.top.switch.hit");
                clickUntil(switchBtn, [&] { return ui->find("git.repo.dd") != nullptr; });
                clickEl(ui->find("git.repo.dd.browse.hit"));
                expect(pumpUntil([&] { return ui->find("git.top.name") != nullptr &&
                                            ui->find("git.top.name")->text == "干净仓库"; },
                                 15000),
                       "浏览目录…→切换到干净仓库");
                pumpUntil([&] { return ui->find("git.list.empty.message") != nullptr; }, 8000);
                expect(ui->find("git.list.empty.message") != nullptr,
                       "干净仓左卡弱提示「没有未提交变更」（kit 空态档）");
                expect(ui->find("git.list.empty.guide") == nullptr,
                       "切仓后过滤词清空（干净空态而非无匹配空态）");
                expect(ui->find("git.right.empty.main") != nullptr &&
                           ui->find("git.right.empty.action") != nullptr,
                       "干净仓右卡空态含 [刷新] 就近操作");
                shot("06_clean_repo");

                // ⑪ 开失效仓 → 删目录 → 下拉三行（1 失效置灰 + 当前 ✓）
                pickedRepo = platform::git::pathToUtf8(repoC.dir);
                clickUntil(ui->find("git.top.switch.hit"),
                           [&] { return ui->find("git.repo.dd") != nullptr; });
                clickEl(ui->find("git.repo.dd.browse.hit"));
                expect(pumpUntil([&] { return ui->find("git.top.name") != nullptr &&
                                            ui->find("git.top.name")->text == "失效仓库"; },
                                 15000),
                       "切换到失效仓（开仓成功）");
                std::error_code rmEc;
                (void)rmEc;
                // 下拉点主仓行 → LRU=[A,C,B,D假]
                clickUntil(ui->find("git.top.switch.hit"),
                           [&] { return ui->find("git.repo.dd") != nullptr; });
                {
                    const core::dsl::Element* aName = nullptr;
                    for (int i = 0; i < 8; ++i) {
                        const core::dsl::Element* e = ui->find(
                            "git.repo.dd.row." + std::to_string(i) + ".name");
                        if (e != nullptr && e->text == "示例仓库") aName = e;
                    }
                    clickUntil(aName, [&] { return ui->find("git.top.name") != nullptr &&
                                                 ui->find("git.top.name")->text == "示例仓库"; });
                }
                // 重开下拉：4 仓（A 当前 ✓ / C / B / D假 已失效）
                clickUntil(ui->find("git.top.switch.hit"),
                           [&] { return ui->find("git.repo.dd") != nullptr; });
                {
                    int rowCount = 0;
                    std::string staleRowName;
                    bool currentHasCheck = false;
                    for (int i = 0; i < 8; ++i) {
                        const std::string base =
                            "git.repo.dd.row." + std::to_string(i);
                        const core::dsl::Element* n = ui->find(base + ".name");
                        if (n == nullptr) continue;
                        ++rowCount;
                        if (ui->find(base + ".stale") != nullptr) {
                            staleRowName = n->text;
                            expect(ui->find(base + ".stale")->text == "已失效",
                                   "失效仓行右缘「已失效」标签");
                        }
                        if (n->text == "示例仓库" && ui->find(base + ".check") != nullptr) {
                            currentHasCheck = true;
                        }
                    }
                    expect(rowCount == 4, "下拉仓行数=4（LRU 新→旧，含预置假失效仓）");
                    expect(staleRowName == "假失效仓库", "失效行=假失效仓库（置灰行）");
                    expect(currentHasCheck, "当前仓行右缘 ✓ 标记");
                    expect(ui->find("git.repo.dd.label") != nullptr,
                           "节标「最近仓库」在场（历史非空时显示）");
                    shot("07_repo_dropdown");
                }
                clickEl(ui->find("git.top.switch.hit"));  // toggle 收起
                pumpFor(5);

                // ⑫ diff 主动线（F-05）+ 头部统计 + 内容 tab
                const RowRef diffRow = findRow("a.txt");
                if (!clickUntil(diffRow.name,
                                [&] { return ui->find("git.diff.list") != nullptr; })) {
                    expect(false, "文件行点击→右栏 diff（git.diff.list）");
                } else {
                    const core::dsl::Element* stats = ui->find("git.right.head.stats");
                    expect(stats != nullptr &&
                               stats->text.find("+1") != std::string::npos &&
                               stats->text.find("−1") != std::string::npos,
                           "diff 头部统计 +N −N（实测 " +
                               (stats != nullptr ? stats->text : "?") + "）");
                    expect(ui->find("git.right.head.badge.text") != nullptr,
                           "diff 头部状态徽标在场");
                    shot("08_diff");
                    const core::dsl::Element* contentTab =
                        ui->find("git.right.head.tab.content.hit");
                    if (!clickUntil(contentTab,
                                    [&] { return ui->find("git.content.list") != nullptr; })) {
                        expect(false, "内容 tab→git.content.list");
                    } else {
                        bool codeVisible = false;
                        for (int i = 0; i < 8; ++i) {
                            const core::dsl::Element* code = ui->find(
                                "git.content.list.slot." + std::to_string(i) + ".code");
                            if (code != nullptr &&
                                code->text.find("第二版") != std::string::npos) {
                                codeVisible = true;
                            }
                        }
                        expect(codeVisible, "内容视图行文本「第二版」在场");
                        shot("09_content");
                        clickEl(ui->find("git.right.head.tab.diff.hit"));
                        pumpFor(10);
                    }
                }

                // ⑬ 左栏折叠/展开（D4-A：折叠=宽 0 隐藏，顶栏展开钮条件出现）
                const core::dsl::Element* collapseBtn = ui->find("git.list.btn.collapse.hit");
                if (!clickUntil(collapseBtn,
                                [&] { return ui->find("git.list.bg") == nullptr; })) {
                    expect(false, "左栏折叠（git.list.bg 消失）");
                } else {
                    expect(ui->find("git.top.expand.hit") != nullptr,
                           "折叠态顶栏展开钮条件出现");
                    shot("10_left_collapsed");
                    if (!clickUntil(ui->find("git.top.expand.hit"),
                                    [&] { return ui->find("git.list.bg") != nullptr; })) {
                        expect(false, "顶栏展开钮恢复左栏");
                    }
                }

                // ⑭ 失效仓行点击=尝试重开 → 目录不存在 → N1 错误块（§6.10②）
                clickUntil(ui->find("git.top.switch.hit"),
                           [&] { return ui->find("git.repo.dd") != nullptr; });
                {
                    const core::dsl::Element* cName = nullptr;
                    for (int i = 0; i < 8; ++i) {
                        const core::dsl::Element* e = ui->find(
                            "git.repo.dd.row." + std::to_string(i) + ".name");
                        if (e != nullptr && e->text == "假失效仓库") cName = e;
                    }
                    if (!clickUntil(cName, [&] { return ui->find("git.empty.error") != nullptr; })) {
                        expect(false, "失效仓行点击重开失败→空态页错误块");
                    } else {
                        shot("13_stale_repo_error");
                    }
                }
            } else {
                // ---- 窄窗档短跑（--size 1200x720）：两栏构图不破版 ----
                shot("14_narrow_1200");
                const core::dsl::Element* flatBtn = ui->find("git.list.btn.flat.hit");
                clickUntil(flatBtn, [&] { return findRow("deep").name == nullptr; });
                shot("15_narrow_flat");
            }
        }
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
