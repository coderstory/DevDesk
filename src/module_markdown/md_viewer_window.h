// md_viewer_window.h — F-04 独立只读查看器窗口公开面（F-02 OS 漏斗专用入口）。
// session 注册表静态持有渲染与搜索/滚动状态；onWindowDestroyed 注销防悬垂
//（EUI-NEO 销毁回调覆盖全部销毁路径：requestWindowClose / Alt+F4 / 应用退出）。
// 只读构造性保证：窗口 compose 只建渲染节点 + scrollview——无编辑 input / 无光标 /
// 无 contextMenu 注册；键路由在窗口级 onKeyEvent，不进主窗 Router。
#pragma once

#include <cstddef>
#include <string>

namespace module::markdown {

// 查看器正文 scrollView id（跳转直写/回读断言/探针共用常量）。每个子窗有独立
// Ui 树，同 id 跨窗不冲突。
inline constexpr const char* kViewerScrollId = "md.viewer.scroll";
// 搜索条输入框 id（聚焦请求/状态换代释放/探针断言用）。
inline constexpr const char* kViewerSearchInputId = "md.viewer.search.input";

// F-02：OS 入口 .md/.markdown 专用——绕开笔记目录拷贝，为 path 开独立只读查看器窗
//（开原文件，只读不回写）。每次调用 = 新 session 新窗（同一文件再开 = 再开一窗，
// D-1）。读文件失败仍开窗，窗内错误占位（PRD 流程 B7）。仅主线程调用（OS 漏斗 /
// Apple Event 同域）。
void openViewerWindow(const std::string& path);

// 测试缝：session 存活数（路由单测断言「.md → 查看器分支」）。
size_t viewerSessionCountForTest();
// 测试缝：按开窗序取 session pageId（同名双开序号后缀断言；越界返回空串）。
std::string viewerSessionPageIdForTest(size_t index);

// 测试缝：清空 session 注册表（不触窗口销毁——测试进程无主循环；用例间隔离）。
void resetViewerSessionsForTest();

// 测试缝（探针直驱搜索态）：对首个匹配 path 的 session 应用 query——与输入框
// onChange 同一条 viewerSearchApplyQuery 通路，不绕状态机。无匹配 session 返回 false。
bool viewerSearchInjectForTest(const std::string& path, const std::string& query);

// 测试缝（need264 直驱选择态）：对首个匹配 path 的 session 注入拖选终态
//（anchor=(blockA,cpA) / focus=(blockB,cpB)，显示文本码点域）——与鼠标抬起
// 同一条 selHasText/viewerSelText 通路（不绕自动复制语义：复制文本由
// viewerSelectionTextForTest 读回）。无匹配 session 或块越界返回 false。
bool viewerSelectionInjectForTest(const std::string& path, size_t blockA,
                                  size_t cpA, size_t blockB, size_t cpB);

// 测试缝（need264）：当前选区将被复制进剪贴板的文本（空选区 = 空串）——
// 与 viewerSelRelease 实际写入剪贴板的字符串完全同一条构建函数。
std::string viewerSelectionTextForTest(const std::string& path);

} // namespace module::markdown
