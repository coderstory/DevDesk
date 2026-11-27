// notepad_session.h — 一窗一 session 的会话状态（注册表静态持有，need:273 批 1）。
// 字段集 = 技术方案卡 §二；生命周期不变量照抄 md_viewer_window 先例：
// session 在 onWindowDestroyed（销毁前回调，覆盖全部销毁路径）中注销 →
// compose 闭包捕获的裸指针与窗口同寿，无悬垂。
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"  // Result<void>（另存为落地单点返回面）
#include "notepad_encoding.h"
#include "ui_kit/text_find.h"  // FindHit / kFindNoHit（查找态字段集）

// eui::Ui / Screen / KeyEvent 在 EUI-NEO 里是 core::dsl 的**类型别名**
//（eui/dsl.h:9-10、types.h:36），不可在本头前向声明 class/struct——那会撞成
// 「definition conflicts with type alias」。本头被 notepad_module.cpp /
// notepad_window.cpp 等消费、需要完整定义，故直接引 EUI 头。
#include <eui/dsl.h>
#include <eui/types.h>
#include <components/markdown.h>  // MarkdownStyle（need345 预览选区样式快照字段）

namespace module::notepad {

using ::ui_kit::text_find::FindHit;
using ::ui_kit::text_find::kFindNoHit;

// 模块内共享（notepad_module.cpp / notepad_session.cpp 同模块内部使用，非公开 API）：
// 注册表本体 + pageId 全局递增序号（pageId = notepad.<seq>，跨清空单调递增）。
struct NotepadSession;  // 前向声明须在本命名空间 —— 写在 detail 内会被解析成
                        // detail::NotepadSession，与真类型不同域（曾编译不过）
namespace detail {
std::vector<std::unique_ptr<NotepadSession>>& sessionRegistry();
std::uint64_t nextPageIdSeq();
}  // namespace detail

struct NotepadSession {
    // ── 身份 ──
    std::string path;      // 用户可见原始路径（toast / 日志）
    std::string normKey;   // 规范路径键（同文件查重唯一依据，weakly_canonical 退化 lexically_normal）
    std::string title;     // 窗口标题 = 文件名（中段省略已处理；脏态「• 」前缀运行时拼）
    std::string pageId;    // notepad.<全局递增序号>（窗口内部唯一标识，v5 口径）
    void* handle = nullptr;  // onWindowCreated 记句柄（聚焦 / setWindowTitle / 注销）

    // ── 载入态 ──
    std::string loadError;   // 非空 = 错误占位文案（窗照开照关，PRD A2 异常分支）
    std::string rawBytes;    // 载入原始字节（干净缓冲保存直通 = 「打开不擅自转换」强保证）
    std::string loadText;    // 载入时 LF 规范缓冲（dirty 判定基线）
    std::string editText;    // 当前编辑缓冲（LF 域镜像；文本本体托管在 InputState）
    std::filesystem::file_time_type loadMtime{};
    bool hasMtime = false;   // mtime 取到才启用保存前拦截
    bool seeded = false;     // InputState 首帧播种旗标（每窗一次）
    int editorFontSize = 14; // 编辑字号（开窗时从 notepad_prefs 取，批 3 接 A-/A+）
    std::string fontFamily;  // 编辑字体族（空 = 系统默认；F-09，开窗从 prefs 取）
    bool wrapOn = true;      // 自动换行偏好（F-19；渲染级 = EUI InputBuilder.softWrap
                             // 旋钮，notepad.btnWrap 钮接管；大文件模式载入强制关）
    bool previewOn = false;  // F-13 md 预览整页切换（.md/.markdown 会话；切换不动
                             // 编辑 InputState → 切回光标/滚动保留）

    // ── md 预览文本选区（need345，语义平移自退役查看器 eebed0b9/池 #264）──
    // 锚/焦 = 整篇显示文本码点域（MarkdownBuilder::displayTextOf 扁平域，含块
    // 间 '\n'）——与旧查看器逐块 (block,cp) 域不同：notepad 预览 = components::
    // markdown 整篇单元素渲染，无逐块几何表，跨块选区天然由扁平域承担。
    // selPreviewStyle/Width = compose 帧快照：指针回调在帧间派发，反查
    // displayCpForPoint 须与渲染同源样式/宽度（bug259 onHover 同纪律——回调
    // 只捕 session 成员指针，禁捕 compose 帧栈上局部）。
    size_t selAnchorCp = 0;
    size_t selFocusCp = 0;
    bool selDragging = false;  // 按下→抬起之间（拖选进行中）
    bool selHasText = false;   // 非空选区（拖选中实时高亮 + 抬起后保留）
    components::MarkdownStyle selPreviewStyle{};  // compose 帧样式快照（见上注）
    float selPreviewWidth = 0.0f;                 // 同上：渲染宽快照

    // bug399 round5 / debt398：预览整篇估高缓存（键 = 文本修订 + 渲染宽 + 样式指纹）。
    // 整篇 estimateHeight 是 O(blocks)，不缓存时每格滚轮重排都全文档重算 = 主线程冻结；
    // wrap 高与 scrollView contentHeightProvider 共用同一缓存值（旧查看器几何表同架构）。
    struct PreviewDocHCache {
        unsigned long long rev = 0;
        float width = -1.0f;
        std::string styleKey;
        float height = 0.0f;
    };
    // 多档宽（滚动条扣减前后两档常态，900/876 等）各一条；容量 4 轮转替换。
    std::array<PreviewDocHCache, 4> previewDocHs{};
    int previewDocHNext = 0;

    // debt398：分段测高段表（预览总高异步化）。键口径同 previewDocHs（修订 +
    // 0.5px 宽 + 样式指纹）；段 = 连续块区间（EUI blockSegmentBoundaries，不切
    // 表格行组）。首帧只出近似总高 + 可见段由 markdown 组件自行精测，屏外段每
    // 帧预算补测 → 总高/滚动条渐进收敛；全部测完写回 previewDocHs 精确缓存。
    struct PreviewSegTable {
        unsigned long long rev = 0;
        float width = -1.0f;
        std::string styleKey;
        std::vector<std::size_t> bounds;    // 段边界（含首 0 与末 n）
        std::vector<float> heights;         // 段高（measured=1 精确 / 0 近似）
        std::vector<unsigned char> measured;
        float total = 0.0f;                 // Σ段高 + 段间 gap（= 整篇估高口径）
        int pending = 0;                    // 未测段数
        bool converged = false;             // 已写回精确总高缓存（只写一次）
        bool usable = false;
    };
    std::array<PreviewSegTable, 4> previewSegTables{};  // 多档宽轮转（同 previewDocHs）
    int previewSegNext = 0;
    PreviewSegTable* previewSegActive = nullptr;  // 本帧 provider 命中的段表（hint 用）
    float previewSegLastDocH = 0.0f;  // 本帧 provider 最后返回的总高（wrap 高同源）
    int previewSegPendings = 0;                   // 活动段表未测段数（测试观察缝）

    // need400：预览查找显示文本缓存（MarkdownBuilder::displayTextOf 结果；
    // 以 InputState.textRevision 失效——预览禁编辑，查找条开着时逐帧复用）
    std::string findPreviewDisplay;
    unsigned long long findPreviewRev = ~0ull;

    // ── 编码 / 行尾（notepad_encoding 探测结果）──
    NotepadEncodingProbe enc{};
    NotepadEol eolMode = NotepadEol::Lf;
    bool mixedEol = false;   // 载入时混合行尾（保存策略见 notepad_file_io）
    bool hasBom = false;     // UTF-8 BOM 保留（保存回写；状态栏点切 F-15）
    bool loadHasBom = false; // 载入基线 BOM（点切回翻 = 回到干净态的脏判定基线）
    NotepadEol loadEol = NotepadEol::Lf;  // 载入基线行尾（同上）
    bool largeFile = false;  // >1MB：关高亮+关 wrap（高亮/wrap 开关批 3 消费），可编辑
    bool hugeFile = false;   // >10MB：只读降级

    // ── 只读降级（非 UTF-8 / UTF-16 / 二进制 / 超大）──
    bool readonly = false;
    std::string readonlyReason;  // 提示条文案（只读时非空）
    bool binarySuspect = false;  // F-14 二进制拒编分支（三选「仍要打开」批 4）

    bool dirty = false;

    // ── 未保存保护链（批 4 棒 A，F-05；纯决策面在 notepad_protect.{h,cpp}）──
    bool skipCloseConfirm = false;   // 「不保存」一次性豁免：置位后下一次关窗
                                     // 请求直通放行并消费；再编辑（setDirty(true)）作废
    bool protectOpen = false;        // 未保存三选弹窗待决（该窗 Ui 树内渲染）
    bool protectQuitContext = false; // 弹窗语境：false=关窗 veto，true=退出 guard
                                     // （决定三选回调走 requestWindowClose 还是
                                     // requestExit 再入队，clue #269）

    // ── 查找替换态（F-06/F-07/F-08，tech 卡 §二 search；会话内隔离，多窗互不影响）──
    // 重开留词：query 在查找条关闭后仍保留（F-06 会话内保留 = session 字段）。
    struct NotepadFindState {
        bool visible = false;             // 查找条开（M+F 开 / Esc 关）
        std::string query;                // 输入框实时词（关条不清）
        std::string appliedQuery;         // 已应用词（输入即重算门控；关条清空 → 重开强制重算）
        bool caseSensitive = false;       // 大小写开关（批 2 默认关；UI 切换批 3 收敛）
        bool wholeWord = false;           // 全词开关（同上）
        bool replaceOpen = false;         // 替换行展开
        std::string replaceWith;          // 替换输入框实时词
        std::vector<FindHit> hits;        // 命中集（appliedQuery ∘ textRevision 域）
        size_t cur = kFindNoHit;          // 当前命中下标（kFindNoHit = 无）
        unsigned long long hitsRevision = 0;  // hits 对应的 InputState.textRevision
        bool styleDirty = false;          // 开关条后命中层缓存失效待消费（compose bump styleRevision）
        bool hitsPreview = false;         // 命中集所属文本域：true = 预览显示文本
                                          // （need400；预览↔编辑切换强制重算）
        bool pendingPreviewScroll = false;  // 预览态待滚动定位（跳转消费后置位，
                                          // drawPreviewSlot 换算矩形 y → scroll）
        bool pendingSeed = false;         // 开条后首帧播种输入框（键路由置位，compose 消费）
        int pendingJump = 0;              // 待跳转 +1/-1（键路由置位，compose 消费——
                                          // 键事件处理器无 Ui 访问权，InputState 直写单点在 compose）
        bool pendingFocusEditor = false;  // 关条焦点回收（P2-3）：开条有播种聚焦，
                                          // 关条对称回归编辑器（notepadFindClose 置位，
                                          // compose 编辑分支消费——键处理器无 Ui 访问权）
    };
    NotepadFindState find;

    // ── 语法高亮语言（notepad_language 映射；compose 懒求值一次，空 = None 纯文本）──
    std::string language;
    // M+Shift+F 格式化待消费（键路由置位——键事件处理器无 Ui 访问权，
    // InputState 直写单点在 compose 消费，find.pendingJump 同模式）
    bool pendingFormatJson = false;
    // 字体弹层 Esc 收起请求（P2-4）：Ui 树 FontPopupState 键路由不可达 →
    // session 请求位，compose drawFontPopup 消费（pendingFormatJson 同模式）
    bool pendingPopupDismiss = false;

    // ── 会话内轻提示（自管 1.5s 窗口；不用 ui_kit 单例 toast 态，多窗互不覆盖）──
    std::string toastMessage;
    std::chrono::steady_clock::time_point toastUntil{};

    // ── 尺寸记忆回写源：compose 每帧回填内容尺寸；关窗时连同位置落库（应用级一份）──
    int lastW = 0;
    int lastH = 0;
    bool sizeSeen = false;
};

// 规范路径键：fs::weakly_canonical（文件存在）失败退 fs::absolute().lexically_normal()
//（不存在文件也要能查重——错误占位窗同样一文件一窗）。主线程单线程调用。
std::string notepadNormKeyFor(const std::string& path);

// 窗口标题 = 文件名含扩展名（路径永不出现在标题）；超长中段省略号截断
//（保头尾 2/3+1/3，截断点回退 UTF-8 码点边界——查看器窗同规则）。
std::string notepadWindowTitleFor(const std::string& path, size_t maxBytes = 96);

// 保存单点（mtime 拦截 + 干净直通/脏重编码；实现单点在 notepad_file_io.cpp，
// 三选「覆盖/另存为/取消」对话框批 4 接管）。
void notepadSaveSession(NotepadSession& s);

// F-04 另存为完整链（need289 修复棒 1；实现单点在 notepad_file_io.cpp）：OS
// 保存对话框（默认名 = 当前文件名，覆盖确认由对话框承担，PRD F-04）→ 取消
// 零副作用（保持编辑态）→ notepadSaveSessionAsTo 落地。错误占位/只读/二进制
// 窗无另存为语义（钮禁用 + M+Shift+S 同口径拦截）。
void notepadSaveSessionAs(NotepadSession& s);

// 另存为落地单点（对话框取消之后的写盘 + session 全量迁移步骤；独立成函数 =
// 无头单测直调，对话框面不可无头化）。写失败 → 返回 fail 且 session 未落
// 任何态（零副作用）；成功 = path/normKey/title/rawBytes/基线
//（loadText/loadHasBom/loadEol/enc）/mtime/脏态/窗口标题全量迁移（设计稿
// §4.2「本窗转为新文件」）。
core::Result<void> notepadSaveSessionAsTo(NotepadSession& s,
                                          const std::string& newPath);

// 同文件查重（openNotepadWindow 专用）。注册表命中 = 窗活着（销毁即注销）。
NotepadSession* findSessionByNormKey(const std::string& normKey);

// onWindowDestroyed 唯一注销点：从注册表 erase + 窗口尺寸/位置记忆落库。
void notepadSessionWindowDestroyed(NotepadSession* s);

// 会话内轻提示（1.5s 自动消失；compose 每帧检查渲染）。
void sessionToast(NotepadSession& s, const std::string& message);

// 脏态切换单点：更新 dirty + 窗口标题「• 」前缀联动（setWindowTitle）。
void sessionSetDirty(NotepadSession& s, bool dirty);

// ── 批 3 棒 B（F-11/F-13）按扩展名的按钮矩阵 + 工具条联动（设计稿 §4.2/§4.6；
// 纯函数可直测）────────────────────────────────────────────────────────────

// 会话扩展名判据：normKey 尾缀匹配（ext 传小写字面量如 ".json"；normKey 保留
// 原 casing，ASCII 大小写折叠后比对——core::notepad notepadAsciiLower 同源折叠）。
bool notepadSessionHasExtension(const NotepadSession& s, std::string_view lowerExt);

// 工具条联动矩阵（compose 消费 + 测试直测）。editable 口径 = compose 同款
//（!loadError && !readonly && !binarySuspect），由本函数内部推导，调用方不再传。
struct NotepadToolbarMatrix {
    bool showFormat = false;         // .json / .sql 扩展（F-11 + need380 SQL）
    bool showPreview = false;        // 仅 .md/.markdown 扩展（F-13）
    bool formatEnabled = false;      // 可编辑且非预览态（readonly/binary/预览禁用）
    bool previewEnabled = false;     // 大文件禁用（1MB 级渲染成本未证，PRD F-16 降级）
    bool findToggleEnabled = true;   // need400：错误态外恒可用（预览查找走显示域）
    bool fontControlsEnabled = true; // 预览态字体下拉/A± 禁用（§4.6）
};
NotepadToolbarMatrix notepadToolbarMatrixFor(const NotepadSession& s);

// ── 窗口装配面（notepad_window.cpp 实现）────────────────────────────
// compose（工具条 44 / 提示条 32 条件 / 编辑区 flex / 状态栏 28，设计稿 §1.2）。
void notepadComposeSession(NotepadSession& s, eui::Ui& ui, const eui::Screen& screen);
// 窗口级键路由（M+O 打开 / M+S 保存 / M+Shift+S 另存为 / M+F 查找 / M+H 替换
// 直开 / M+Shift+F 格式化 / M+P·M+Shift+V 预览 / M+W 关窗 / M±= 字号；不进主
// 窗 Router；shift 组合单独判定——shortcut() 显式排除 shift）。
void notepadHandleKeySession(NotepadSession& s, const eui::KeyEvent& key);
// F-02 窗内打开：OS 打开对话框（过滤器=扩展名清单+所有文件）→ 逐路径
// openNotepadWindow（已开路径由查重聚焦收口）。
void notepadOpenViaDialog(NotepadSession& s);

// need345：预览选区将复制的文本（整篇显示文本码点域切片，无 markdown 标记
// 符；空选区 = 空串）。实现单点在 notepad_window.cpp——拖选抬起自动复制与
// Ctrl+C 兼容通道同一条构建通路（测试直驱本函数，session 字段可直接注入）。
std::string notepadPreviewSelText(const NotepadSession& s);

// debt398 测试缝：同步把活动段表（provider 命中的宽度档）全部段精测到底。
// O/P 段腿前预热用——渐进中间值不得污染既有「同档 docH 漂移 ≤1px」断言。
// 返回本次实际测量段数（0 = 无活动段表）。
int notepadPreviewWarmForTest(NotepadSession& s);

}  // namespace module::notepad
