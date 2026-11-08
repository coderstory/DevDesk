// layout_guard_probe.cpp — 布局病守卫 kit：自检 + 首批接入（池 need #221）
//
// 结构（全部只读断言，不改产品代码；发现的病入池报告）：
//   §0 kit 自检：五类断言纯几何红绿表（含 occludedPure 纯核）
//   §1 settings 域：模块卡 / 提醒卡 / 同步卡 × ①字号溢出(100%+140%) ②双端撑开
//       ③发丝条 ⑤遮挡扫描
//   §2 release 域：任务列表行卡 ②③、新建任务弹窗 ①(100%+140%)④、详情页 ③⑤
//   §3 人为违规演示：真实页面盒做扰动副本 → 断言红 → 还原 → 绿
//
// 退出码：0 全绿；1 有违例（known-issue 表内条目只告警不红）。

#include "probes/probe_harness.h"
#include "probes/layout_guard_kit.h"

#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（链接需要）
#include "test_support.h"
#include "app_registry.h"
#include "core/module_interface.h"
#include "module_release/release_store.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

// app:: 层实现随 dsl_app_impl.h 自给（同 settings_dropdown_z_probe：探针不建
// 应用级 UI，compose 空实现仅满足链接）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_layout_guard_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

/// 已知未修病登记（rule|subject 前缀）：门禁保持绿、病在池里追。空 = 无已知病。
const std::set<std::string>& knownIssues() {
    static const std::set<std::string> kKnown = {
        // "字号溢出|settings.card.xxx",  // 示例：池 #NNN
    };
    return kKnown;
}

struct Guard {
    bool passed = true;
    int checks = 0;

    bool isKnown(const layout_guard::Violation& v) const {
        const std::string key = v.rule + "|" + v.subject;
        for (const std::string& k : knownIssues()) {
            if (key.compare(0, k.size(), k) == 0) return true;
        }
        return false;
    }

    /// ok=断言通过（无病）。v 非空 = 病发详情。
    void expect(bool ok, const layout_guard::Violation* v,
                const std::string& what) {
        ++checks;
        if (ok) return;
        if (v != nullptr) {
            if (isKnown(*v)) {
                std::cout << "[KNOWN] " << what << " : " << v->rule << " "
                          << v->subject << " " << v->detail << "\n";
                return;
            }
            std::cerr << "layout_guard_probe: FAIL——" << what << " : "
                      << v->rule << " " << v->subject << " " << v->detail << "\n";
        } else {
            std::cerr << "layout_guard_probe: FAIL——" << what << "\n";
        }
        passed = false;
    }

    /// 演示专用：期望断言红（ok==false 才是演示成功）。
    void expectRed(bool ok, const std::string& what) {
        ++checks;
        if (!ok) {
            std::cout << "【演示-红】" << what << " → 断言拦截 ✓\n";
            return;
        }
        std::cerr << "layout_guard_probe: FAIL——演示失败（应红未红）：" << what << "\n";
        passed = false;
    }
};

using layout_guard::Box;
using layout_guard::Violation;

/// 最底「内容」叶：只认有文本的叶子。容器 / 轨道类包装结构与卡底齐平属常态
/// （run1 实测：modules 卡 362.0 vs 卡底 360.0 全是包装叶齐平，非溢出），
/// 字号溢出病（#187/#215）的用户可见形态是文本坠出，断言对象随之收窄。
Box deepestContentLeaf(const std::vector<layout_guard::NamedBox>& leaves) {
    Box out;
    for (const auto& nb : leaves) {
        if (nb.box.h <= 0.0f || nb.text.empty()) continue;
        if (out.h == 0.0f || nb.box.bottom() > out.bottom()) out = nb.box;
    }
    return out;  // 无文本叶 → 全零（调用方跳过）
}

/// 只留几何上落在容器盒内的叶子（弹窗 backdrop 自身的全幅叶剔除）。
std::vector<layout_guard::NamedBox> leavesInside(
    const std::vector<layout_guard::NamedBox>& leaves, const Box& container) {
    std::vector<layout_guard::NamedBox> out;
    for (const auto& nb : leaves) {
        if (nb.box.x >= container.x - 0.5f && nb.box.right() <= container.right() + 0.5f &&
            nb.box.y >= container.y - 0.5f && nb.box.bottom() <= container.bottom() + 0.5f) {
            out.push_back(nb);
        }
    }
    return out;
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("layout_guard_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    // 数据底座：临时库 + 注册为 appDatabase（release 模块取已有连接）
    test_support::TestEnv testEnv("lgprobe_", "layoutguard", true);
    module::release::ReleaseStore store(testEnv.db->db());

    // 最小种子：2 服务 + 3 任务（列表有行卡可断言）
    {
        using namespace module::release;
        ServiceDef backend;
        backend.name = "后端服务";
        backend.kind = ServiceKind::Backend;
        backend.defaultBranch = "main";
        backend.sort = 1;
        store.upsertService(backend);
        const char* dates[3] = {"2026-11-03", "2026-11-04", "2026-11-05"};
        const TaskStatus statuses[3] = {TaskStatus::Running, TaskStatus::Pending,
                                        TaskStatus::Draft};
        for (int i = 0; i < 3; ++i) {
            Task task;
            task.planDate = dates[i];
            task.status = statuses[i];
            const auto created = store.createTask(task);
            if (created.isFail()) {
                std::cerr << "layout_guard_probe: 种子任务失败\n";
                return 2;
            }
            TaskEnv env;
            env.taskId = created.value();
            env.envName = "EM";
            env.startMinute = 9 * 60;
            env.endMinute = 18 * 60;
            TaskService svc;
            svc.serviceName = "layout-svc";
            svc.kind = ServiceKind::Backend;
            svc.defaultBranch = "main";
            svc.branch = "main";
            store.addTaskEnv(env, {svc});
        }
    }

    core::dsl::Runtime runtime;
    runtime.initialize(window);
    Guard G;
    core::dsl::Ui* ui = nullptr;
    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    auto composeFrame = [&](const std::string& pageId,
                            core::Module* mod) {
        probe::composeModuleFrame(runtime, window, pageId, w, h,
                                  [&](eui::Ui& u, const eui::Screen& screen) {
                                      ui = &u;
                                      mod->compose(u, screen);
                                  });
    };
    auto centerOf = [](const core::dsl::Element* e) {
        return std::pair<float, float>{e->frame.x + e->frame.width * 0.5f,
                                       e->frame.y + e->frame.height * 0.5f};
    };

    // ══ §0 kit 自检：五类断言红绿表（纯几何，无页面依赖） ══
    {
        std::cout << "── §0 kit 自检 ──\n";
        const Box card{0, 0, 300, 300};
        G.expect(layout_guard::contentWithinBottom(card, Box{10, 250, 100, 40}, 2.0f),
                 nullptr, "自检① 内容底 290 ≤ 298");
        Violation v1;
        G.expect(layout_guard::contentWithinBottom(card, Box{10, 298, 100, 4}, 2.0f, &v1) == false,
                 nullptr, "自检①（红例构造，验证返回 false）内容底 302 > 298");
        G.expect(!v1.rule.empty(), nullptr, "自检① 红例填充违例详情");

        const Box row{0, 0, 100, 40};
        G.expect(layout_guard::rowContentSpread(
                     row, {Box{0, 5, 35, 20}, Box{65, 5, 35, 20}}, 0.5f),
                 nullptr, "自检② 覆盖 70% ≥ 50%");
        Violation v2;
        G.expect(layout_guard::rowContentSpread(
                     row, {Box{0, 5, 10, 20}, Box{90, 5, 10, 20}}, 0.5f, &v2) == false,
                 nullptr, "自检②（红例）覆盖 20% < 50% → 中间空白病");
        G.expect(v2.rule == "双端撑开", nullptr, "自检② 病名=双端撑开");

        G.expect(layout_guard::noHairlineBar(Box{0, 0, 100, 12}, 100.0f), nullptr,
                 "自检③ 全宽条 h=12 ≥ 10");
        G.expect(layout_guard::noHairlineBar(Box{0, 0, 100, 1}, 100.0f), nullptr,
                 "自检③ h=1 功能分隔线豁免（R6）");
        Violation v3;
        G.expect(layout_guard::noHairlineBar(Box{0, 0, 100, 5}, 100.0f, &v3) == false,
                 nullptr, "自检③（红例）h=5 发丝装饰条");
        G.expect(layout_guard::noHairlineBar(Box{0, 0, 50, 5}, 100.0f), nullptr,
                 "自检③ 非全宽（50% < 90%）不判");

        const Box trigger{50, 50, 100, 100};
        G.expect(layout_guard::overlayAnchored(trigger, Box{100, 60, 40, 80}), nullptr,
                 "自检④ dx=+20/dy=+15 在 ±40/±60 内");
        Violation v4;
        G.expect(layout_guard::overlayAnchored(trigger, Box{100, 60, 40, 80}, 40.0f,
                                               60.0f, &v4) == true,
                 nullptr, "自检④ 同盒带出参仍绿");
        G.expect(layout_guard::overlayAnchored(trigger, Box{190, 60, 40, 80}, 40.0f,
                                               60.0f, &v4) == false,
                 nullptr, "自检④（红例）dx=+85 超上限");

        G.expect(layout_guard::occludedPure(Box{0, 0, 50, 20}, 0, Box{10, 5, 50, 20},
                                            10, false) == true,
                 nullptr, "自检⑤（红例）相交 + z 更高 → 遮挡");
        G.expect(layout_guard::occludedPure(Box{0, 0, 50, 20}, 0, Box{100, 5, 50, 20},
                                            10, false) == false,
                 nullptr, "自检⑤ 不相交 → 无遮挡");
        G.expect(layout_guard::occludedPure(Box{0, 0, 50, 20}, 0, Box{10, 5, 50, 20},
                                            10, true) == false,
                 nullptr, "自检⑤ 祖先关系（容器含文本）→ 不判");
    }

    // ══ §1 settings 域 ══
    std::unique_ptr<core::Module> settingsMod(create_settings_module());
    settingsMod->onStart();
    {
        std::cout << "── §1 settings 域 ──\n";
        composeFrame("settings", settingsMod.get());
        // 逐 tab 找三张卡：外观/编辑器在 tab0，模块/提醒/同步可能在别的 tab
        const char* kTargetCards[3] = {"settings.card.modules",
                                       "settings.card.reminder", ""};
        std::string syncCardId;
        for (int tab = 0; tab < 8; ++tab) {
            const std::string tabId = "settings.tabs." + std::to_string(tab) + ".hit";
            const core::dsl::Element* tabHit = ui->find(tabId);
            if (tabHit == nullptr) break;
            const auto [cx, cy] = centerOf(tabHit);
            probe::click(window, runtime, cx, cy);
            composeFrame("settings", settingsMod.get());
            composeFrame("settings", settingsMod.get());
            if (syncCardId.empty()) {
                for (const std::string& id : layout_guard::findIds(*ui, "settings.")) {
                    if (id.find("github") != std::string::npos ||
                        id.find("sync") != std::string::npos) {
                        // 取最短者 = 卡根（其余为其子元素）
                        if (syncCardId.empty() || id.size() < syncCardId.size()) {
                            syncCardId = id;
                        }
                    }
                }
            }
            for (const char* cardId : kTargetCards) {
                if (cardId[0] == '\0') continue;
                const core::dsl::Element* card = ui->find(cardId);
                if (card == nullptr) continue;
                const Box cardBox = layout_guard::boxOf(*card);
                std::cout << "  [" << cardId << "] 卡 " << cardBox.w << "x"
                          << cardBox.h << " @tab" << tab << "\n";
                const auto leaves = layout_guard::leafBoxes(*ui, cardId);
                // ① 100% 档内容底
                const Box deepest = deepestContentLeaf(leaves);
                if (deepest.h > 0.0f) {
                    Violation v;
                    G.expect(layout_guard::contentWithinBottom(cardBox, deepest, 2.0f, &v),
                             &v, std::string(cardId) + " 100% 档内容底");
                }
                // ② 卡级内容覆盖（阈值 0.35：卡有内边距 + 分组行，低于行级阈值）
                std::vector<Box> leafOnly;
                leafOnly.reserve(leaves.size());
                for (const auto& nb : leaves) leafOnly.push_back(nb.box);
                Violation v2;
                G.expect(layout_guard::rowContentSpread(cardBox, leafOnly, 0.35f, &v2),
                         &v2, std::string(cardId) + " 内容双端撑开");
                // ③ 发丝条扫描
                for (const auto& nb : leaves) {
                    Violation v3;
                    if (!layout_guard::noHairlineBar(nb.box, cardBox.w, &v3)) {
                        v3.subject = nb.id;
                        G.expect(false, &v3, std::string(cardId) + " 发丝条");
                    }
                }
                // ⑤ 静止态遮挡扫描
                for (const auto& v5 : layout_guard::scanOccluders(*ui, cardId)) {
                    G.expect(false, &v5, std::string(cardId) + " 遮挡扫描");
                }
            }
            if (ui->find("settings.card.modules") != nullptr &&
                ui->find("settings.card.reminder") != nullptr &&
                !syncCardId.empty()) {
                break;  // 三张卡都拿到即可
            }
        }
        // ① 140% 档：三张卡内容底（clue 125：改档后必须重新 compose+find）
        core::theme::setFontScale(1.4f);
        composeFrame("settings", settingsMod.get());
        composeFrame("settings", settingsMod.get());
        const char* scaleCards[3] = {"settings.card.modules",
                                     "settings.card.reminder",
                                     syncCardId.c_str()};
        for (const char* cardId : scaleCards) {
            if (cardId[0] == '\0') continue;
            const core::dsl::Element* card = ui->find(cardId);
            if (card == nullptr) continue;
            const auto leaves = layout_guard::leafBoxes(*ui, cardId);
            const Box deepest = deepestContentLeaf(leaves);
            if (deepest.h > 0.0f) {
                Violation v;
                G.expect(layout_guard::contentWithinBottom(
                             layout_guard::boxOf(*card), deepest, 2.0f, &v),
                         &v, std::string(cardId) + " 140% 档内容底");
            }
        }
        core::theme::setFontScale(1.0f);
        composeFrame("settings", settingsMod.get());
        if (syncCardId.empty()) {
            std::cout << "  [未验证] 同步卡 id 未发现（github/sync 关键字零命中）\n";
        }
        // §3 素材：模块卡真实盒（演示段用）
        // （演示段在 §3 重新 compose 后取，指针不跨帧持有）
        settingsMod->onStop();
    }

    // ══ §2 release 域 ══
    std::unique_ptr<core::Module> tasksMod(create_release_tasks_module());
    tasksMod->onStart();
    {
        std::cout << "── §2 release 域 ──\n";
        composeFrame("release", tasksMod.get());
        composeFrame("release", tasksMod.get());
        const std::string kCardPrefix = "release.tasks.card.";
        std::vector<std::string> cardIds;
        for (const std::string& id : layout_guard::findIds(*ui, kCardPrefix)) {
            // 前缀匹配会连子元素一起命中（card.N.prog.wrap 等，run1 实测 38 张
            // 假卡）；顶级行卡 = 前缀之后没有更多 '.' 的 id
            if (id.find('.', kCardPrefix.size()) == std::string::npos) {
                cardIds.push_back(id);
            }
        }
        std::cout << "  任务行卡 " << cardIds.size() << " 张\n";
        if (cardIds.empty()) {
            G.expect(false, nullptr, "release 任务行卡未发现（种子/渲染异常）");
        }
        for (const std::string& cardId : cardIds) {
            const core::dsl::Element* card = ui->find(cardId);
            if (card == nullptr) continue;
            const Box cardBox = layout_guard::boxOf(*card);
            const auto leaves = layout_guard::leafBoxes(*ui, cardId);
            std::vector<Box> leafOnly;
            leafOnly.reserve(leaves.size());
            for (const auto& nb : leaves) leafOnly.push_back(nb.box);
            Violation v2;
            G.expect(layout_guard::rowContentSpread(cardBox, leafOnly, 0.35f, &v2),
                     &v2, cardId + " 行内容双端撑开");
            for (const auto& nb : leaves) {
                Violation v3;
                if (!layout_guard::noHairlineBar(nb.box, cardBox.w, &v3)) {
                    v3.subject = nb.id;
                    G.expect(false, &v3, cardId + " 发丝条");
                }
            }
        }
        // 功能分隔线：release.tasks.divider.* 必须落在豁免口径（h ≤ 2）
        for (const std::string& id : layout_guard::findIds(*ui, "release.tasks.divider.")) {
            const core::dsl::Element* d = ui->find(id);
            if (d == nullptr) continue;
            const Box box = layout_guard::boxOf(*d);
            Violation v;
            G.expect(layout_guard::noHairlineBar(box, box.w, &v), &v,
                     id + " 分隔线豁免口径（h ≤ 2px）");
        }

        // ── 新建任务弹窗：①(100%+140%) + ④ datepicker 锚定 ──
        const core::dsl::Element* newBtn = ui->find("release.tasks.new");
        if (newBtn == nullptr) {
            G.expect(false, nullptr, "release.tasks.new 按钮未发现");
        } else {
            const auto [cx, cy] = centerOf(newBtn);
            probe::click(window, runtime, cx, cy);
            composeFrame("release", tasksMod.get());
            composeFrame("release", tasksMod.get());
            const core::dsl::Element* dialog = ui->find("release.newtask.dialog");
            if (dialog == nullptr) {
                G.expect(false, nullptr, "release.newtask.dialog 未弹出");
            } else {
                // release.newtask.dialog 是全窗 backdrop（run1 实测 y=0 h=1000）；
                // 真面板 = 子元素中面积最大且高度 < 90% 窗高的那个
                auto resolvePanel = [&](const core::dsl::Element* dlg)
                    -> const core::dsl::Element* {
                    const core::dsl::Element* panel = nullptr;
                    float bestArea = 0.0f;
                    for (const auto& c : dlg->children) {
                        const float area = c->frame.width * c->frame.height;
                        if (c->frame.height < h * 0.9f && area > bestArea) {
                            bestArea = area;
                            panel = c.get();
                        }
                    }
                    return panel;
                };
                const core::dsl::Element* panel = resolvePanel(dialog);
                if (panel == nullptr) {
                    std::cout << "  [未验证] 弹窗面板子元素未定位\n";
                } else {
                    const Box panelBox = layout_guard::boxOf(*panel);
                    const Box deepest = deepestContentLeaf(leavesInside(
                        layout_guard::leafBoxes(*ui, "release.newtask.dialog"),
                        panelBox));
                    if (deepest.h > 0.0f) {
                        Violation v;
                        G.expect(layout_guard::contentWithinBottom(panelBox, deepest, 2.0f, &v),
                                 &v, "新建任务弹窗 100% 档内容底");
                    }
                }
                // 140% 档
                core::theme::setFontScale(1.4f);
                composeFrame("release", tasksMod.get());
                composeFrame("release", tasksMod.get());
                const core::dsl::Element* dialog140 = ui->find("release.newtask.dialog");
                if (dialog140 != nullptr) {
                    const core::dsl::Element* panel140 = resolvePanel(dialog140);
                    if (panel140 != nullptr) {
                        const Box panelBox140 = layout_guard::boxOf(*panel140);
                        const Box deepest140 = deepestContentLeaf(leavesInside(
                            layout_guard::leafBoxes(*ui, "release.newtask.dialog"),
                            panelBox140));
                        if (deepest140.h > 0.0f) {
                            Violation v;
                            G.expect(layout_guard::contentWithinBottom(
                                         panelBox140, deepest140, 2.0f, &v),
                                     &v, "新建任务弹窗 140% 档内容底");
                        }
                    }
                }
                core::theme::setFontScale(1.0f);
                composeFrame("release", tasksMod.get());
                composeFrame("release", tasksMod.get());
                // ④ datepicker 锚定（开日期选择后断言）
                const core::dsl::Element* dateHit = ui->find("release.newtask.date.hit");
                if (dateHit != nullptr) {
                    const auto [dx, dy] = centerOf(dateHit);
                    probe::click(window, runtime, dx, dy);
                    composeFrame("release", tasksMod.get());
                    composeFrame("release", tasksMod.get());
                    const core::dsl::Element* picker =
                        ui->find("release.newtask.datepicker");
                    const core::dsl::Element* hit2 = ui->find("release.newtask.date.hit");

                    if (picker != nullptr && hit2 != nullptr) {
                        // datepicker 根是全幅层（backdrop 撑满 1200×801 @0,0），真日历
                        // 面板是它的兄弟子元素。**按 id 直取**，不要用「面积最大的
                        // 子元素」启发式——backdrop 面积必然远大于面板（1200×801 vs
                        // 380×260），启发式会稳定选中 backdrop，于是 gap 算成
                        // 「0 − 触发器底」的负值假红（池 bug240 实测：真实面板
                        // y=270.5 对上 date.hit 底 259.5，gap=11px 合规）。
                        const core::dsl::Element* pickerPanel =
                            ui->find("release.newtask.datepicker.panel");
                        if (pickerPanel == nullptr) {
                            pickerPanel = ui->find("release.newtask.datepicker.panel.bg");
                        }
                        if (pickerPanel == nullptr) pickerPanel = picker;
                        Violation v4;
                        // 日历面板天然比触发器高得多（run1：中心 dy=172 被中心距
                        // 口径误报），附着族弹层用「贴上/下弹出」口径
                        G.expect(layout_guard::overlayAttached(
                                     layout_guard::boxOf(*hit2),
                                     layout_guard::boxOf(*pickerPanel), 40.0f, 0.5f, &v4),
                                 &v4, "datepicker 附着于 date.hit");
                    } else {
                        std::cout << "  [未验证] datepicker 未进树（picker="
                                  << (picker != nullptr) << "）\n";
                    }
                }
                // 关弹窗还原（过渡帧跑满再点行卡：收尾半透明 backdrop 仍会
                // 拦截命中，run2/run4 详情页点不进即此因）
                const core::dsl::Element* cancel = ui->find("release.newtask.cancel");
                if (cancel != nullptr) {
                    const auto [cx2, cy2] = centerOf(cancel);
                    probe::click(window, runtime, cx2, cy2);
                }
                for (int i = 0; i < 24; ++i) composeFrame("release", tasksMod.get());
            }
        }

        // ── 详情页：③⑤（点首张行卡进入）──
        if (!cardIds.empty()) {
            const core::dsl::Element* first = ui->find(cardIds.front());
            // 行底命中区 = id + ".bg"（release_list_ui.cpp：整行命中 → openTask；
            // run2 实测点卡面/点 .hit 均无跳转）
            const core::dsl::Element* firstBg = ui->find(cardIds.front() + ".bg");
            const core::dsl::Element* clickTarget =
                firstBg != nullptr ? firstBg : first;
            if (clickTarget != nullptr) {
                const auto [cx, cy] = centerOf(clickTarget);
                probe::click(window, runtime, cx, cy);
                composeFrame("release", tasksMod.get());
                composeFrame("release", tasksMod.get());
                if (ui->find("release.detail.root") == nullptr) {
                    std::cout << "  [未验证] 详情页未进入（点行卡无跳转）\n";
                } else {
                    std::cout << "  详情页已进入\n";
                    for (const auto& v5 :
                         layout_guard::scanOccluders(*ui, "release.detail.root")) {
                        G.expect(false, &v5, "详情页遮挡扫描");
                    }
                    for (const auto& nb : layout_guard::leafBoxes(*ui, "release.detail.root")) {
                        Violation v3;
                        if (!layout_guard::noHairlineBar(nb.box, w * 0.6f, &v3)) {
                            v3.subject = nb.id;
                            G.expect(false, &v3, "详情页发丝条");
                        }
                    }
                }
            }
        }
        tasksMod->onStop();
    }

    // ══ §3 人为违规演示：真实盒扰动副本 → 红 → 还原 → 绿 ══
    {
        std::cout << "── §3 人为违规演示（红→还原→绿）──\n";
        std::unique_ptr<core::Module> mod(create_settings_module());
        mod->onStart();
        composeFrame("settings", mod.get());
        core::dsl::Element* card = nullptr;  // 模块卡不在 tab0（run1：@tab1），逐 tab 找
        for (int tab = 0; tab < 8 && card == nullptr; ++tab) {
            const core::dsl::Element* tabHit =
                ui->find("settings.tabs." + std::to_string(tab) + ".hit");
            if (tabHit == nullptr) break;
            probe::click(window, runtime, tabHit->frame.x + tabHit->frame.width * 0.5f,
                         tabHit->frame.y + tabHit->frame.height * 0.5f);
            composeFrame("settings", mod.get());
            composeFrame("settings", mod.get());
            card = ui->find("settings.card.modules");
        }
        if (card == nullptr) {
            std::cout << "  [未验证] 演示素材缺失（settings.card.modules）\n";
        } else {
            const Box cardBox = layout_guard::boxOf(*card);
            const auto leaves = layout_guard::leafBoxes(*ui, "settings.card.modules");
            const Box leaf = deepestContentLeaf(leaves);
            // 绿：原值
            G.expect(layout_guard::contentWithinBottom(cardBox, leaf, 2.0f), nullptr,
                     "演示① 原值（绿）");
            // 红：扰动副本（内容底挪到卡底+8 = 人为构造坠出；run2 的 +12 平移
            // 落在卡内余量里未越线，改用定量越过）
            Box bad = leaf;
            bad.y = cardBox.bottom() - bad.h + 8.0f;
            Violation vd;
            G.expectRed(layout_guard::contentWithinBottom(cardBox, bad, 2.0f, &vd),
                        "演示① 内容底越卡底+8 坠出");
            // 绿：还原
            G.expect(layout_guard::contentWithinBottom(cardBox, leaf, 2.0f), nullptr,
                     "演示① 还原（绿）");
            // 发丝条演示
            Box bar{cardBox.x, cardBox.y + 10.0f, cardBox.w, 5.0f};
            G.expectRed(layout_guard::noHairlineBar(bar, cardBox.w),
                        "演示③ 全宽 5px 发丝条");
            bar.h = 1.0f;
            G.expect(layout_guard::noHairlineBar(bar, cardBox.w), nullptr,
                     "演示③ 还原为 1px 功能分隔线（豁免，绿）");
        }
        mod->onStop();
    }

    settingsMod.reset();
    tasksMod.reset();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    std::cout << "layout_guard_probe: checks=" << G.checks << " passed=" << G.passed
              << "\n";
    if (!G.passed) std::cerr << "FAIL\n";
    return G.passed ? 0 : 1;
}
