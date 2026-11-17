// notepad_session.h — 一窗一 session 的会话状态（注册表静态持有，need:273 批 1）。
// 字段集 = 技术方案卡 §二；生命周期不变量照抄 md_viewer_window 先例：
// session 在 onWindowDestroyed（销毁前回调，覆盖全部销毁路径）中注销 →
// compose 闭包捕获的裸指针与窗口同寿，无悬垂。
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "notepad_encoding.h"
#include "notepad_find.h"  // FindHit / kFindNoHit（查找态字段集）

// eui::Ui / Screen / KeyEvent 在 EUI-NEO 里是 core::dsl 的**类型别名**
//（eui/dsl.h:9-10、types.h:36），不可在本头前向声明 class/struct——那会撞成
// 「definition conflicts with type alias」。本头被 notepad_module.cpp /
// notepad_window.cpp 等消费、需要完整定义，故直接引 EUI 头。
#include <eui/dsl.h>
#include <eui/types.h>

namespace module::notepad {

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
        bool pendingSeed = false;         // 开条后首帧播种输入框（键路由置位，compose 消费）
        int pendingJump = 0;              // 待跳转 +1/-1（键路由置位，compose 消费——
                                          // 键事件处理器无 Ui 访问权，InputState 直写单点在 compose）
    };
    NotepadFindState find;

    // ── 语法高亮语言（notepad_language 映射；compose 懒求值一次，空 = None 纯文本）──
    std::string language;

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
    bool showFormat = false;         // 仅 .json 扩展（F-11）
    bool showPreview = false;        // 仅 .md/.markdown 扩展（F-13）
    bool formatEnabled = false;      // 可编辑且非预览态（readonly/binary/预览禁用）
    bool previewEnabled = false;     // 大文件禁用（1MB 级渲染成本未证，PRD F-16 降级）
    bool findToggleEnabled = true;   // 预览态 M+F 禁用（§4.6 查找禁用）
    bool fontControlsEnabled = true; // 预览态字体下拉/A± 禁用（§4.6）
};
NotepadToolbarMatrix notepadToolbarMatrixFor(const NotepadSession& s);

// ── 窗口装配面（notepad_window.cpp 实现）────────────────────────────
// compose（工具条 44 / 提示条 32 条件 / 编辑区 flex / 状态栏 28，设计稿 §1.2）。
void notepadComposeSession(NotepadSession& s, eui::Ui& ui, const eui::Screen& screen);
// 窗口级键路由（M+O 打开 / M+S 保存 / M+W 关窗；不进主窗 Router）。
void notepadHandleKeySession(NotepadSession& s, const eui::KeyEvent& key);
// F-02 窗内打开：OS 打开对话框（过滤器=扩展名清单+所有文件）→ 逐路径
// openNotepadWindow（已开路径由查重聚焦收口）。
void notepadOpenViaDialog(NotepadSession& s);

}  // namespace module::notepad
