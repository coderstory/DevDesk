// src/module_git/git_tree_model.h
// 左栏目录树行模型（need:7 批次二；技术方案 §1 / UI 方案 A §5.2）。
// 两模式：AllFiles = std::filesystem 单层 walk + ignore 过滤（懒加载，按需展开）；
//         ChangesOnly = status 快照驱动平铺（不 walk）。
// 渲染纪律（clue#22）：rows() 是（模型状态，）的纯函数——compose measure pass 每帧
// 两跑必须幂等；行盒与命中矩形由视图按（行, 宽度）现算，模型不缓存任何矩形。
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "platform/git/git_repo.h"

namespace module::git {

// 行类别。NoAccess = 无权限目录降级行（无展开符、单击无反应）；
// Error = 懒加载失败行内错误行（附原因 + 重试）；EmptyDir = 展开后空目录提示行。
enum class TreeRowKind {
    Directory,
    File,
    Subrepo,    // 子仓库：禁展开降级（F-12）
    NoAccess,   // 无权限目录
    EmptyDir,   // 展开后空目录提示（派生行，不进缓存）
    Error,      // 懒加载失败行内错误（派生行，不进缓存）
};

struct TreeRow {
    std::string name;     // 显示名（路径末段，UTF-8）
    std::string relPath;  // 相对 workdir 的 POSIX 路径（目录不带尾 '/'，UTF-8）
    int depth = 0;
    TreeRowKind kind = TreeRowKind::File;
    // 变更着色（四态之一；无变更 = nullopt）。重命名行 name = 新路径。
    std::optional<platform::git::StatusKind> status;
    bool expanded = false;       // Directory 有效
    bool loading = false;        // Directory：懒加载进行中（展开符位换旋转圈）
    bool childrenLoaded = false; // Directory：子层已加载
    std::string oldPath;         // 重命名副行（← 旧名）；其余行空
    std::string errorText;       // Error 行原因
};

// 单层枚举结果（worker → model 通道）：权限失败单列（目录行降级 NoAccess），
// 其余失败带原因（行内错误行 + 重试）。
struct WalkResult {
    std::vector<TreeRow> entries;  // ok 时有效
    bool ok = false;
    bool permission = false;
    std::string message;
};

class GitTreeModel {
public:
    enum class Mode { AllFiles, ChangesOnly };

    // 切仓/清空：模式保持，数据面全清
    void reset();
    void setMode(Mode mode);
    Mode mode() const { return mode_; }

    // 「全部文件」模式：worker walk 结果入库。dirRel == "" 表示根层。
    void applyChildren(const std::string& dirRel, std::vector<TreeRow> entries);
    // 懒加载三态：发起（展开符换旋转圈）/ 权限失败（目录行降级 NoAccess）/
    // 其它失败（行内 Error 行 + 原因，保留展开态可重试）
    void markLoading(const std::string& dirRel);
    void markNoAccess(const std::string& dirRel);
    void markLoadFailed(const std::string& dirRel, const std::string& message);
    void clearLoadFailed(const std::string& dirRel);

    // 展开/折叠状态机（连点防抖由视图同值判幂等；这里只做集合置位）
    void setExpanded(const std::string& dirRel, bool expanded);
    bool isExpanded(const std::string& dirRel) const;
    // 根层是否已完成首次 walk（左栏骨架态判据）
    bool rootLoaded() const { return children_.count(std::string()) > 0; }

    // status 快照注入：AllFiles 模式 = 着色叠加；ChangesOnly 模式 = 行数据源（重建平铺）
    void applyStatus(const platform::git::StatusSnapshot& snapshot);

    // 可见行序（纯函数；排序 = 目录类在前、按 name 字节序）
    std::vector<TreeRow> rows() const;

    // 已展开但子层未加载且未在加载中的目录（controller 据此投 worker）
    std::vector<std::string> pendingDirs() const;

    // 重试入口：清掉目录上的错误标记并回到待加载态（视图「重试」按钮调）
    void requestRetry(const std::string& dirRel);

    // 单层目录枚举（worker 侧跑，Repo 提供 isIgnored；repo == nullptr 时不过滤）。
    // 结果按展示序排好（目录类在前）。.git 跳过；ignored 过滤；symlink 目录不跟随
    //（按叶节点 File 行展示）；子仓库目录（含 .git 文件/目录）→ Subrepo。
    static WalkResult walkChildren(platform::git::Repo* repo,
                                   const std::filesystem::path& workdir,
                                   const std::string& dirRelUtf8);

private:
    void rebuildChangesRows();

    Mode mode_ = Mode::AllFiles;
    // 目录 → 子层缓存（"" = 根层）；TreeRow 复用为节点记录（depth 由 rows() 现算）
    std::map<std::string, std::vector<TreeRow>> children_;
    std::set<std::string> expanded_;
    std::set<std::string> loading_;
    std::map<std::string, std::string> loadErrors_;  // dirRel → 失败原因
    std::set<std::string> noAccess_;
    // path → 变更类别（相对 workdir POSIX 路径，已按路径去重合并双侧）
    std::map<std::string, platform::git::StatusKind> statusByPath_;
    std::map<std::string, std::string> renameOld_;   // 新路径 → 旧路径
    std::vector<TreeRow> changesRows_;               // ChangesOnly 模式平铺行
};

// 徽标字母（中栏 16×16 badge / 树行着色共用）：New→A Modified→M Deleted→D
// Renamed→R Untracked→U Ignored→I（Ignored 不进 status，防御位）。
char statusBadgeLetter(platform::git::StatusKind kind);

} // namespace module::git
