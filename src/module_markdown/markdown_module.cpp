// Markdown 笔记本模块：三栏 UI（目录树 / 编辑器 / 预览），四态可切换。
// 所有 I/O 经 MarkdownService，失败走 core::Result；键盘流纯函数在 md_keyboard.*，
// 语法高亮在 md_highlight.*。类定义与共享助手在 markdown_module_internal.h，
// 成员函数按 文件树/编辑器/预览/菜单轮询 四块分驻 md_module_*.cpp（拆分第一步）。

#include "markdown_module_internal.h"

#include "core/mcp_tool.h"
#include "notes_mcp_tools.h"

#include <utility>

namespace module::markdown {
namespace {

// ServiceLocator 提供方适配器（UAT6.2 H31）：MarkdownService → IMarkdownNotesSource。
// 生命周期由 MarkdownModule 成员 notesSource_ 持有；service_ 早于其创建、晚于其析构
//（成员声明序保证），适配器只存引用。
class NotesSourceAdapter final : public ::core::IMarkdownNotesSource {
public:
    explicit NotesSourceAdapter(MarkdownService& service) : service_(service) {}

    ::core::Result<std::vector<::core::NoteRef>> listNotes() override {
        auto all = service_.listAll();
        if (all.isFail()) {
            return ::core::Result<std::vector<::core::NoteRef>>::fail(
                all.errorCode(), all.errorMessage());
        }
        std::vector<::core::NoteRef> out;
        out.reserve(all.value().size());
        for (const auto& m : all.value()) {
            out.push_back(
                {MarkdownService::toPosix(m.relativePath), m.title});
        }
        return ::core::Result<std::vector<::core::NoteRef>>::ok(std::move(out));
    }

    bool noteExists(const std::string& relPath) override {
        // 磁盘层判定（fileMtime 失败 = 不存在），不信任调用方内存缓存
        auto mtime = service_.fileMtime(relPath);
        return mtime.isOk();
    }

private:
    MarkdownService& service_;
};

} // namespace

// 测试缝实现（声明与理由见 markdown_module_test_seam.h）。唯一消费方是
// tests/test_modules/test_markdown.cpp，入口是 create_markdown_module()，故
// static_cast 安全 —— 该缝不对外承诺接受任意 core::Module*。
void mdSetPollIntervalMsForTest(::core::Module* mod, int64_t ms) {
    if (mod == nullptr || ms <= 0) return;
    static_cast<MarkdownModule*>(mod)->pollIntervalMs_ = ms;
}



// 生命周期与 compose 编排（onStart/onStop/compose/键事件/模式与 toast）。
// 拆分第一步（抽编译单元）产物：成员函数在此出行定义，纯代码搬移。

void MarkdownModule::onStart()  {
    if (started_) return;
    started_ = true;
    // GitHub Sync 拉取会 DROP 重建整个库：订阅导入成功事件，导入线程同步回调里
    // 只置原子脏标记（不得摸 UI 线程成员），compose 收割重载。
    // 闸 = 存活守卫：onStop 先关闸再退订，挡住在途 handler（emit 已把 handler
    // 拷出后才调用，退订拦不住在途那一发）。
    dbImportHandlersEnabled_.store(true, std::memory_order_release);
    dbImportedSub_ = ::core::event::bus().subscribe(
        ::core::event::kDbImported, [this](const std::string&) {
            if (dbImportHandlersEnabled_.load(std::memory_order_acquire)) {
                dbImportedDirty_.store(true, std::memory_order_release);
            }
        });
    auto* db = ::core::appDatabase();
    if (db == nullptr) {
        LOG_WARN("Markdown 模块启动：appDatabase() 为空，跳过真实索引");
        return;
    }
    // 服务复用而非重建：agent run 线程可能正持 md.* 工具引用旧 service，
    // off→on 窗口重建 = 先析构旧对象 → UAF。前提：db 指针与 notesDir 运行期不变。
    if (!service_) {
        service_ = std::make_unique<MarkdownService>(*db,
                                                     ::core::paths::markdownDir());
    }
    markdownTools_ = std::make_unique<MarkdownTools>(*service_);
    markdownTools_->registerAll();
    auto r = service_->rescan();
    if (r.isFail()) {
        LOG_ERROR("Markdown rescan 失败: " + r.errorMessage());
        showToast("笔记目录加载失败", "重扫失败：" + r.errorMessage(), true);
    } else {
        LOG_INFO("已加载笔记目录：" + service_->notesDir().filename().string());
    }
    refreshList();
    loadViewState();  // 恢复上次的选中笔记 / 视图模式 / 收起目录

    // 跨模块提供方注册（UAT6.2 H31）：Todo 详情「关联文档」选择器经 ServiceLocator
    // 取笔记清单。onStart 幂等（started_ 门控），重复注册覆盖旧指针无害。
    if (!notesSource_) {
        notesSource_ = std::make_unique<NotesSourceAdapter>(*service_);
    }
    ::core::ServiceLocator::set<::core::IMarkdownNotesSource>(notesSource_.get());

    // 笔记目录 git 自动提交：后台启动（异步 init，不阻塞 onStart）
    if (!noteGit_) {
        noteGit_ = std::make_shared<NoteGitAutoCommit>(
            service_->notesDir());
    }
    noteGit_->start();

    // 需-7 笔记 MCP 三工具（notes_search / notes_read / notes_write）：注册进
    // 全局 MCP 注册表（onStart 注册 / onStop 注销，markdown_tools 同款生命周期；
    // markdown 模块关闭即随行注销）。失败只 LOG_ERROR 不阻断启动。
    if (auto reg = mcp::registerNotesMcpTools(core::mcp::globalMcpTools(),
                                              *service_, noteGit_);
        reg.isFail()) {
        LOG_ERROR("notes MCP 工具注册失败（不阻断启动）: " + reg.errorMessage());
    }

    LOG_INFO("Markdown 模块启动，共 " + std::to_string(notes_.size()) + " 篇笔记");
}

void MarkdownModule::onStop()  {
    if (!started_) return;
    // 先关原子闸再退订：闸挡在途 handler，退订挡后续 emit。
    dbImportHandlersEnabled_.store(false, std::memory_order_release);
    dbImportedSub_ = {};
    if (markdownTools_) {
        markdownTools_->unregisterAll();
        markdownTools_.reset();
    }
    // 需-7：MCP notes 三工具先注销再放 noteGit（write 工具 shared 持有 git 链，
    // 在途 execute 由注册表 + shared_ptr 保活；service_ 跨 stop/start 存续不悬垂）
    mcp::unregisterNotesMcpTools(core::mcp::globalMcpTools());
    // 退出前落盘：无退出拦截 API，只能同步有限重试 + 日志兜底。
    // 退出时冲突未决（toast 随页面消失、没有让用户选择的时机）：只能按「保留本地」
    // 落盘 —— 未保存的键入不可恢复，外部版本可从笔记 git 历史 / 远端找回，所以这个
    // 默认方向是安全侧；但不能静默，WARN 留痕（saveEditor 成功时会顺带 resolve）。
    if (editorDirty_ && conflictOpen_) {
        LOG_WARN("Markdown: 退出模块时存在未决冲突，按「保留本地」落盘（外部版本可在笔记 git 历史 / 远端找回）: " +
                 currentPath_);
    }
    for (int attempt = 0; attempt < 3 && editorDirty_; ++attempt) {
        saveEditor();
        if (!editorDirty_) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (editorDirty_ && !currentPath_.empty()) {
        LOG_ERROR("退出时保存笔记失败，未落盘的编辑已丢失: " + currentPath_ +
                  "（EUI-NEO 暂无退出拦截 API，无法阻断退出）");
    }
    // 笔记 git 自动提交：析构 join 后台线程（已在 onFileSaved 推进时间戳，
    // 本 onStop 也会经循环 saveEditor → onFileSaved 触发最后一次 commit）
    noteGit_.reset();
    // 先注销再析构适配器：ServiceLocator 只存指针，先析构 = 消费方悬垂
    ::core::ServiceLocator::set<::core::IMarkdownNotesSource>(nullptr);
    notesSource_.reset();
    started_ = false;
    LOG_INFO("Markdown 模块停止");
}

void MarkdownModule::compose(eui::Ui& ui, const eui::Screen& screen)  {
    harvestDbImport();

    flushEditorIfIdle();

    pollDiskChangesIfDue();

    if (outlineDirty_) {
        outline_ = scanHeadings(editorText_);
        outlineDirty_ = false;
    }

    // 消费跨模块跳转负载（一次性语义）
    consumeNavPayload();

    const auto& t = core::theme::current();
    // UAT3 T1（D-R3-1 米黄清除）：页面主体皮肤整体切 wechat（与 todo/home 同口径）。
    // 本变量只喂页面主体（树/编辑/实时/预览/大纲/状态栏链）；popup 作用域
    // （弹窗/右键菜单/toast）各自就地取 componentTokensWeChat，不经本变量。
    const auto componentTheme =
        core::theme::componentTokensWeChat(t);

    // 主题名变化 = 换色表 = bump styleRevision 触发全部可视行重高亮
    if (const std::string themeName = core::theme::currentThemeName();
        themeName != highlightThemeName_) {
        highlightThemeName_ = themeName;
        highlighter_.setColors(mdHighlightColors(t));
        ++ui.state<components::input_detail::InputModel::InputState>(
                  kEditorInputId)
             .styleRevision;
        liveHighlighter_.setColors(mdHighlightColors(t));
        ++ui.state<components::input_detail::InputModel::InputState>(
                  kLiveInputId)
             .styleRevision;
    }

    const float pad = t.sizes.spacingLarge * 2.0f;  // 32 —— S4.2 弹性内边距（spacingLarge=16×2=32）
    const float contentW = screen.width - pad * 2.0f;   // 宽 = 窗口宽 − 双侧内边距
    const float contentH = screen.height - pad;   // 高 = 窗口高 − 底内边距（顶距 0=标题顶格，底距 32）
    // UAT3 D-R3-3 c：面板宽每帧向目标 lerp（固定系数），距目标 <1px 即钉住；
    // treePanelCollapsed_（bool）仍为唯一持久化口径，本值启动时按折叠态直接落位。
    // sideW 取连续值 → midW/列宽随动画平滑过渡，窄窗下不再二值跳变。
    const float treeTargetW = treePanelCollapsed_ ? 0.0f : kTreePanelWidth;
    treePanelWidth_ += (treeTargetW - treePanelWidth_) * kTreePanelAnimK;
    if (std::fabs(treeTargetW - treePanelWidth_) < kTreePanelSnapEps) {
        treePanelWidth_ = treeTargetW;
    }
    const float sideW = treePanelWidth_;
    const float gap = t.sizes.spacingLarge;
    const float midW = (contentW - sideW - gap * 2.0f) / 2.0f;
    constexpr float kHeaderGap = 8.0f;   // 标题下间距（2026-10-25 拍板：16→8 收紧）
    // 池130：tab 带 32px 不再全局扣高（原树列跟着变矮）——只从 tab 带正下方那列
    //（编辑/实时/纯预览）顶部扣（tabBandInset），树列恒满高。
    const float bodyH = contentH - ui_kit::kHeaderH - kHeaderGap;

    // 页面底色 = 主窗口 clearColor，不自铺不透明底（半透/磨砂档才生效）

    ui.column("md.root")
        .size(screen.width, screen.height)
        .padding(pad, 0.0f, pad, pad)  // 上=0 标题顶格（2026-10-25 拍板）——EUI padding 序=(left,top,right,bottom)，底 32
        .gap(kHeaderGap)
        .content([&] {
            drawHeader(ui, contentW, contentH, bodyH, t);
            drawBody(ui, contentW, bodyH, sideW, midW, gap, t, componentTheme);
        })
        .build();

    // 池130：tab 带移出列流，按页坐标画在编辑列顶部——x = 树面板动画宽 + gap、
    // 宽 = 编辑列宽（双栏 midW / 单栏 fullW），随 sideW lerp 逐帧连续无跳变；
    // y = 页头下缘（root 顶距 0 → kHeaderH + kHeaderGap）。
    if (!openNotes_.empty()) {
        const float tabW = mode_ == ViewMode::Split
                               ? midW
                               : contentW - sideW - gap;
        drawNoteTabBar(ui, pad + sideW + gap,
                       ui_kit::kHeaderH + kHeaderGap, tabW, t, componentTheme);
    }

    // 方案 A 边界把手：一颗把手管两端，取代旧 drawTreeExpandFab + md.tree.collapse
    drawTreeHandle(ui, pad, bodyH, sideW, gap, t);

    // 帮助钮 tooltip 绑定（claude.title.help 同款现成 hint 机制）：hover 弹回
    // 快捷键说明（文案源 = onKeyEvent 的快捷键注释口径，防「问号钮看着可点
    // 却无解释」回归）。必须在**根作用域**构建——tooltip 组件是普通 in-flow
    // stack，画进 md.root 列闭包会占流内槽位把 body 下压 48px（bug149）。
    // 锚点静态推算（bug163 修正，旧算两处漂移）——①坐标空间：本 tooltip 挂
    // compose 根作用域，x/y 是窗口坐标，而 md.root 有左 pad，content 坐标须
    // +pad 才是窗口 x（tab 带/把手同口径）；②行内偏移：md.actions 行（宽
    // 304）justify END 实装 help 24 + gap 4(spacingSmall) + 大纲 112 + gap 4 +
    // 新建 136（旧算 124 已漂移）= 280，左余 24 → help 中心 = 行内 x 36。行
    // 起点窗口 x = pad + (contentW - 304)，故锚点 x = pad + contentW - 268。
    // bounds 取窗口口径：右限 = screen.width - pad - 560 - 12，气泡 560 宽
    // clamp 右贴后指针尖（anchorX - tooltipX）恰落按钮中心正下方；y 向
    // topLimit 44 把气泡钉页头正下。面板宽 560——本句 4 组快捷键（约 19 全角
    // + 37 ASCII，估宽 ≈505px）远长于 claude 的 15 字档（320），tooltip 默认
    // 86-112 是 barchart 短标签档必截断。
    components::tooltip(ui, "md.help.icon.tooltip")
        .theme(componentTheme)
        .source("md.help.icon.bg")
        .value("快捷键：Ctrl+Shift+P 切换视图、Ctrl+N 新建笔记、Ctrl+S 保存、Esc 关浮层")
        .anchor(pad + contentW - 304.0f + 36.0f, ui_kit::kHeaderH * 0.5f)
        .bounds(screen.width - pad, screen.height)
        .minWidth(560.0f)
        .maxWidth(560.0f)
        .build();

    // UAT5 真机收敛泵：lerp 只在 compose 里推进，而真机主循环是事件驱动——无输入
    // 即 glfwWaitEvents 睡眠，且 runtime 的 animating 位只认 DSL 管理的动画
    //（onFrame/过渡/GIF/滚动惯性），本手搓 treePanelWidth_ lerp 对它不可见。
    // 后果（真机两报同根因）：点折叠/展开 → 点击帧只推进一步（224→~184）即冻结，
    // 面板壳仍 ~224、内容空（<220 不画）、浮钮（>1px）不上屏；其后每次任意输入
    //（如反复开关大纲）各供一帧，面板再向目标爬一步 → 编辑列 x 随之逐轮漂移。
    // headless 直泵 compose 40 帧绕过主循环策略，故既有测试恒绿、真机分叉。
    // 修法 = carousel 同款模式：动画在途挂 1x1 空 onFrame 元素——runtime 每帧经
    // updateFrameCallback 回调它并置 animating_/composeRequested_/paintRequested_
    // → 主循环持续出帧推进 compose 直至钉住；收敛后本元素不再挂载即被剪除，
    // 循环回睡，零常驻功耗。条件与上方钉住判定互补（<eps 钉住 / >=eps 挂泵），
    // 不存在「在途无泵」或「收敛常驻泵」的缝隙。
    // W3.5 审计 B（F5）复现裁决：本泵曾在 UAT6（2fd90a3c）被移除，移除理由
    // 「headless 下 onFrame 经 ui.layout() move → bad_function_call」为误诊——
    // 实测（泵重挂 + 逐断言）真因是 test_markdown.cpp 树列收敛用例「零漂移锁」
    // 循环跨帧持 otoggle 悬垂指针（元素每帧重建，旧 onClick 调用 = UB）；泵
    // 元素改变堆布局使 UB 以 bad_function_call 显形。EUI onFrame 契约本身
    // 健全（updateElementTree→updateFrameCallback 链路无 move）。悬垂指针已修，
    // 泵恢复为 md 树折叠动画的**自持帧源**——不再依赖 app.fluid.anim.driver
    // 的旁路持续 compose（fluid 开关关闭时树折叠动画因此逐输入步进）。
    if (std::fabs(treeTargetW - treePanelWidth_) >= kTreePanelSnapEps) {
        ui.stack("md.tree.anim.driver")
            .size(1.0f, 1.0f)
            .onFrame([](float) {})
            .build();
    }

    drawOutlinePanel(ui, screen, t, componentTheme);

    drawNewNoteDialog(ui, screen, t, componentTheme);
    drawNewFolderDialog(ui, screen, t, componentTheme);
    // 右键菜单 zIndex 1050 盖过 dialog 默认 1000；菜单项点击先 dismiss 再开弹窗
    drawNoteContextMenu(ui, screen, t, componentTheme);
    drawFolderContextMenu(ui, screen, t, componentTheme);
    drawEditorContextMenu(ui, screen, t, componentTheme);
    drawDeleteDialog(ui, screen, t, componentTheme);
    drawRenameDialog(ui, screen, t, componentTheme);
    drawFolderDeleteDialog(ui, screen, t, componentTheme);
    drawFolderRenameDialog(ui, screen, t, componentTheme);
    drawToast(ui, screen, componentTheme);
}

bool MarkdownModule::closeTopmostPopup() {
    if (dirRenameOpen_) { dirRenameOpen_ = false; return true; }
    if (dirDeleteConfirmOpen_) { dirDeleteConfirmOpen_ = false; return true; }
    if (renameOpen_) { renameOpen_ = false; return true; }
    if (deleteConfirmOpen_) { deleteConfirmOpen_ = false; return true; }
    if (editorCtxOpen_) { editorCtxOpen_ = false; return true; }
    if (dirCtxOpen_) { dirCtxOpen_ = false; return true; }
    if (ctxMenuOpen_) { ctxMenuOpen_ = false; return true; }
    if (newFolderOpen_) {
        newFolderOpen_ = false;
        newFolderParentOpen_ = false;
        return true;
    }
    if (newNoteOpen_) { newNoteOpen_ = false; return true; }
    return false;
}

void MarkdownModule::drawHeader(eui::Ui& ui,
                float contentW,
                float contentH,
                float bodyH,
                const core::theme::Tokens& t) {
    (void)contentH;
    (void)bodyH;
    // UAT3 T1：页面头部控件（大纲开关）随页面主体切 wechat；开关主色 = 微信绿。
    const auto componentTheme =
        core::theme::componentTokensWeChat(t);
    // UAT5 全局顶部收紧：kHeaderH 64→40（共享常量），标题行 24
    ui.row("md.header")
        .size(contentW, ui_kit::kHeaderH)
        .alignItems(eui::Align::CENTER)
        .content([&] {
            const float actionsW = 304.0f;   // 右操作组控件定宽（按钮 112/136 字面）
            // B6/F-69-2：标题列与模式 Tab 段随字号派生；两者都按可用宽收口
            // （放大档三列相加可超内容宽 → 必须先夹，否则 Tab/按钮被推出页面右缘）。
            const float segW = std::min(core::theme::scaled(264.0f),
                                        std::max(216.0f, contentW - actionsW -
                                                             200.0f - 16.0f));
            // 2026-10-25 用户拍板：标题列按内容收紧（原 contentW−actionsW−segW 拉满，
            // 模式 Tab 被推到最右、中间 ~400px 空隙）；Tab 左聚跟标题，spacer 推 actions 靠右
            const float kTitleColW =
                std::min(core::theme::scaled(240.0f),
                         std::max(160.0f, contentW - segW - actionsW - 16.0f));
            ui.column("md.title")
                .size(kTitleColW, ui_kit::kHeaderH)
                .gap(2.0f)
                .content([&] {
                    // 行盒 32 = 全站页标题口径（about_module.cpp:202 kTitleH /
                    // agent_module.cpp:480 同款）：fontSizeTitle 24 的 CJK 包围盒
                    // ≈1.3×em，行盒 24 装不下（池138 下半被裁）。
                    ui.text("md.h1")
                        .size(kTitleColW, core::theme::scaled(32.0f))
                        .text("Markdown")
                        .fontSize(t.sizes.fontSizeTitle)
                        .lineHeight(core::theme::scaled(32.0f))
                        .fontWeight(700)
                        .maxWidth(kTitleColW)
                        .overflow(core::TextOverflow::Ellipsis)
                        .clip()
                        .color(t.colors.textPrimary)
                        .build();
                })
                .build();

            // S2.2 块面 Tab：高度 40、4px 间距（gaps=3，tabW 反算）；primarySoftBg+primary 字。
            // 节点 id = md.mode.0..3，hit/text/bg 按 wtab 约定。
            // 标签逐字「编辑/实时/双栏/预览」不变（modeOrderIndex 逆映射）。
            const float tabGap = 4.0f;
            const float tabW = (segW - tabGap * 3.0f) / 4.0f;
            ui.stack("md.mode")
                .size(segW, ui_kit::kWtabHeight)
                .content([&] {
                    for (int i = 0; i < 4; ++i) {
                        ui_kit::wtab(
                            ui,
                            std::string("md.mode.") + std::to_string(i),
                            std::vector<std::string>{"编辑", "实时", "双栏",
                                                     "预览"}[i],
                            modeOrderIndex(mode_) == i,
                            static_cast<float>(i) * (tabW + tabGap), 0.0f, tabW,
                            ui_kit::kWtabHeight, t,
                            [this, i] { setMode(modeFromOrderIndex(i)); });
                    }
                })
                .build();

            // spacer：模式 Tab 与右侧 actions 之间的弹性空隙
            ui.stack("md.header.spacer")
                .size(std::max(0.0f, contentW - kTitleColW - segW - actionsW), 1.0f)
                .build();

            ui.row("md.actions")
                .size(actionsW, 40.0f)
                .justifyContent(eui::Align::END)
                .alignItems(eui::Align::CENTER)
                .gap(t.sizes.spacingSmall)
                .content([&] {
                    // 快捷键帮助入口（§5.4）：UAT6 问号钮重设计——照 sweep 批
                    // 定标准（claude.title.help 同款五段规格）：24px 正圆、icon
                    // 居中、hover 三态（透明底→浅灰→按压灰）、flat，长文案经
                    // EUI tooltip 弹回快捷键说明（文案源 = onKeyEvent 的快捷键
                    // 注释口径，见下方 tooltip 绑定）。0xF059 = fa-circle-question。
                    constexpr float kHelpBtnD = 24.0f;
                    ui.stack("md.help.icon")
                        .size(kHelpBtnD, kHelpBtnD)
                        .visualStateFrom("md.help.icon.bg", 0.965f)
                        .content([&] {
                            ui.rect("md.help.icon.bg")
                                .size(kHelpBtnD, kHelpBtnD)
                                .states(eui::Color{0.0f, 0.0f, 0.0f, 0.0f},
                                        t.colors.primarySoftBg,
                                        t.colors.hover)
                                .radius(kHelpBtnD * 0.5f)
                                .build();
                            ui.text("md.help.icon.icon")
                                .size(kHelpBtnD, kHelpBtnD)
                                .icon(0xF059)
                                .fontSize(core::theme::scaled(14.0f))
                                .lineHeight(kHelpBtnD)
                                .horizontalAlign(eui::HorizontalAlign::Center)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .color(t.colors.textTertiary)
                                .build();
                        })
                        .build();

                    components::toggleSwitch(ui, "md.outline.toggle")
                        .size(112.0f, 36.0f)
                        .checked(outlineOpen_)
                        .text("大纲")
                        .fontSize(t.sizes.fontSizeSmall)
                        .theme(componentTheme)
                        .onChange([this](bool v) { outlineOpen_ = v; })
                        .build();

                    // 微信绿主按钮（§4.1 Primary）：wechat 令牌填充 + 白字。
                    // 用 componentTokensWeChat 给 button 组件绿底；row 布局定位不受 ui_kit::button position 干扰。
                    const auto wechatTheme = core::theme::componentTokensWeChat(t);
                    components::button(ui, "md.btn.new")
                        .size(136.0f, 40.0f)
                        .text("新建笔记")
                        .fontSize(t.sizes.fontSizeBody)
                        .theme(wechatTheme, true)
                        .textColor(t.colors.onPrimary)
                        .radius(t.sizes.radius)
                        .onClick([this] { openNewNoteDialog(); })
                        .build();
                })
                .build();
        })
        .build();

    // 帮助钮 tooltip 绑定已移出本函数：tooltip 组件是普通 in-flow stack（无
    // ignoreLayout），画在 md.root 列内容闭包里会占走一个 40px 流内槽位 +
    // 8px gap，把 md.body 整体下压 48px、列底溢出窗口（bug149，2026-11-07
    // 探针实测 md.help.icon.tooltip frame=(32,48) 560x40 幽灵占位）。现挂
    // compose 根作用域（claude.title.help 同款），见 compose 内 tooltip 绑定。
}

void MarkdownModule::drawBody(eui::Ui& ui,
              float contentW,
              float bodyH,
              float sideW,
              float midW,
              float gap,
              const core::theme::Tokens& t,
              const components::theme::ThemeColorTokens& componentTheme) {
    const float fullW = contentW - sideW - gap;
    ui.row("md.body")
        .size(contentW, bodyH)
        .gap(gap)
        .content([&] {
            drawTreeColumn(ui, bodyH, sideW, t, componentTheme);
            if (mode_ == ViewMode::Live) {
                drawLiveColumn(ui, bodyH, fullW, t, componentTheme);
                return;
            }
            if (mode_ != ViewMode::Preview) {
                drawEditorColumn(ui, bodyH, mode_ == ViewMode::Split ? midW : fullW,
                                 t, componentTheme);
            }
            if (mode_ != ViewMode::Edit) {
                drawPreviewColumn(ui, bodyH, mode_ == ViewMode::Split ? midW : fullW,
                                  t, componentTheme, gap);
            }
        })
        .build();
}

void MarkdownModule::drawToast(eui::Ui& ui, const eui::Screen& screen,
               const components::theme::ThemeColorTokens& themeTokens) {
    const auto wcTheme =
        core::theme::componentTokensWeChat(core::theme::current());
    if (!conflictOpen_) {
        ui_kit::drawToast(ui, screen, "md.toast", toast_, wcTheme);
        return;
    }
    components::toast(ui, "md.toast")
        .theme(wcTheme)
        .screen(screen.width, screen.height)
        .visible(true)  // 本分支 conflict 必真
        .size(360.0f, 128.0f)  // 带 action 按钮加高一档
        .duration(0.0f)
        .title("文件已被外部修改")
        .message("「" + currentPath_ + "」在磁盘上被其他程序修改，"
                 "与未保存的本地编辑冲突。\n✕ 关闭 = 保留本地编辑")
        .icon(0xF071)
        .action("重载磁盘版本", [this] { resolveConflict(true); })
        .onAutoDismiss([this] {
            if (conflictOpen_) {
                resolveConflict(false);
            } else {
                toast_.open = false;
            }
        })
        .onDismiss([this] {
            if (conflictOpen_) {
                resolveConflict(false);
            } else {
                toast_.open = false;
            }
        })
        .build();
}

void MarkdownModule::showToast(const std::string& title, const std::string& msg, bool isError) {
    toast_.show(title, msg, isError);
}

void MarkdownModule::enterLiveState(const char* reason) {
    liveCursorValid_ = false;
    liveFocusPending_ = true;
    liveScrollY_ = 0.0f;
    // 滚动跟随状态位群回初始值：scrollView 本帧重建（offset 取声明值 0）
    liveFollow_ = false;
    liveProgrammaticScroll_ = false;
    liveMaxOffset_ = 0.0f;   // 上一会话的 maxOffset 不属于新文档
    liveCursorSeen_ = -1;    // -1 ≠ 任何真实光标 → 首帧判为一次光标事件
    liveContentWidth_ = 0.0f;
    // 输入框状态也换代（下一帧 release md.live.input）：只复位锚不复位状态时，
    // 上一篇的块文本 / 在飞组字 / framework undo 栈会跟着稳定 id「复活」。
    liveInputStateFresh_ = true;
    // 撤销栈随之换代（下一帧按当前文档重建基线）
    liveUndoSeeded_ = false;
    LOG_DEBUG(std::string("Markdown 实时态状态复位: ") + reason);
}

void MarkdownModule::setMode(ViewMode m) {
    if (m == mode_) return;
    // 离开实时态前关掉撤销合并组：别让跨态两次键入并成同一次撤销
    if (mode_ == ViewMode::Live) liveUndo_.markBoundary();
    if (m == ViewMode::Live) enterLiveState("切换视图");
    mode_ = m;
    // 进预览态时对齐编辑器位置（延迟到 compose，需要列宽与样式）
    if (m == ViewMode::Preview) syncPreviewOnEnter_ = true;
    saveViewState();  // 四态视图跨会话记忆
    LOG_INFO("Markdown 视图切换: " + std::to_string(static_cast<int>(m)));
}

bool MarkdownModule::onKeyEvent(const eui::KeyEvent& key)  {
    // 弹窗输入框已挂 onCancel，聚焦时 Esc 被 input 收走，不重复
    if (key.isDown() && key.key == eui::InputKey::Escape) {
        return closeTopmostPopup();
    }
    if (key.isDown() && key.modifiers.shortcut() && key.modifiers.shift &&
        key.key == eui::InputKey::P) {
        setMode(nextMode(mode_));
        return true;
    }
    // Ctrl+S 显式落盘。四态共用 saveEditor 幂等链（无改动零操作）。
    if (key.isDown() && key.modifiers.shortcut() && !key.modifiers.shift &&
        !key.modifiers.alt && key.key == eui::InputKey::S) {
        saveEditor();
        LOG_INFO("Ctrl+S：手动保存 " + currentPath_);
        return true;
    }
    // Ctrl+N 与按钮/跨模块负载同一入口；任一弹窗开着就落空，
    // 避免清掉正在输入的草稿名。
    if (key.isDown() && key.modifiers.shortcut() && !key.modifiers.shift &&
        !key.modifiers.alt && key.key == eui::InputKey::N) {
        if (newNoteOpen_ || newFolderOpen_ || renameOpen_ ||
            deleteConfirmOpen_) {
            return false;
        }
        openNewNoteDialog();
        LOG_INFO("Ctrl+N：打开新建笔记弹窗");
        return true;
    }
    return false;
}

void MarkdownModule::consumeNavPayload() {
    auto* router = ::core::appRouter();
    if (router == nullptr) return;
    const std::string payload = router->consumePendingPayload(pageId());
    if (payload.empty()) return;

    if (payload == "action:new_note") {
        openNewNoteDialog();
        LOG_INFO("跨模块负载：打开新建笔记弹窗");
        return;
    }
    static constexpr char kOpenPrefix[] = "open:";
    constexpr size_t kOpenLen = sizeof(kOpenPrefix) - 1;
    if (payload.size() > kOpenLen &&
        payload.compare(0, kOpenLen, kOpenPrefix) == 0) {
        openNote(payload.substr(kOpenLen));
        return;
    }
    LOG_WARN("Markdown: 未识别的跳转负载: " + payload);
}

void MarkdownModule::harvestDbImport() {
    // 早退必须在 exchange 之前：DB 未就绪时不能把脏标记消费掉丢弃。
    if (!service_) return;
    if (!dbImportedDirty_.exchange(false, std::memory_order_acq_rel)) return;
    refreshList();
    checkCurrentNoteOnDisk();
    LOG_INFO("Markdown: 检测到库导入，已重载笔记列表");
}

} // namespace module::markdown
extern "C" ::core::Module* create_markdown_module() {
    return new module::markdown::MarkdownModule();
}
