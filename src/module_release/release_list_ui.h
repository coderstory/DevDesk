// 发布任务列表页（池 55 重设计）：单列 64px 行卡 + 模式筛选 Tab + 首屏/触底分页。
//
// 契约：
//   - 列表数据是装配层组装好的卡片视图（ReleaseTaskCard）。本文件不查库：
//     进度 / 展示状态 / 下一步指引 / 日期短标一律由装配层按页快照推导填本结构。
//   - 行卡定高 kReleaseTaskRowH=64（主行 24 + 副行 20 + 上下内边距 10×2，8px
//     网格；设计 §5.1），单列白卡内纵排、行间 1px 二级分割线；内容总高 O(1)。
//   - 分页口径：首屏 kReleaseTaskPageSize 条；滚动到底按需向数据层再取一页
//     （offset = 已加载数），数据耗尽（已加载数 >= countTasks()）后继续触底
//     不再追加，也不再显示「加载中…」。加载中只允许一个请求在飞（重复触底
//     不得重复追加）；加载失败保留已加载内容并给出可点击重试的提示。
//   - Tab（全部/发布中/准备中/已归档）是状态分组筛选（设计 §四）：在内存对
//     已加载卡过滤，store 分页契约（offset/limit 全量序）不变；Tab 右端组计数
//     走 store::countTasksByStatuses（库值口径，含 voided）。
//   - ReleaseTaskPager 是「卡片已全部预载」场景（骨架页/无头测试）的内存分页模型；
//     真实列表页由 store 重载直接按页读库，不经过它。
//   - 回调由调用方注入；行点击回调携带任务 id。
#pragma once

#include "core/result.h"
#include "release_types.h"

#include <eui/dsl.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace module::release {

class ReleaseStore;
struct ReleaseNewTaskDialogState;

// 行卡定高（主 24 + 副 20 + 内边距 10×2；设计 §3.2 高度档）。
inline constexpr float kReleaseTaskRowH = 64.0f;
inline constexpr std::size_t kReleaseTaskPageSize = 10;

// 状态 chip 文案（五态唯一映射点；列表与详情共用同一口径，词表 = 池 53 §4.4：
// 草稿/待执行/执行中/已完成/已作废。旧词「配置中」已废除）。
std::string releaseTaskStatusText(TaskStatus status);

// 计划日期工具（纯逻辑，无 IO）：isoDate 必须严格为 YYYY-MM-DD 且真实存在。
// 非法日期一律返回 false / -1 / 空串，不猜「最近的有效日」。
bool releaseIsIsoDate(const std::string& isoDate);
int releaseWeekdayIndex(const std::string& isoDate);  // 0=周日 … 6=周六；非法 = -1
std::string releaseWeekdayLabel(const std::string& isoDate);  // "周二"；非法 = ""
bool releaseIsRegularReleaseDay(const std::string& isoDate);  // 周二 / 周四

// 模式筛选 Tab（设计 §四）：后三档即池 53 模式徽标词的状态分组。
enum class ReleaseListTab { All = 0, Publishing = 1, Preparing = 2, Archived = 3 };
inline constexpr int kReleaseListTabCount = 4;

// Tab 标题词（全部 / 发布中 / 准备中 / 已归档）；越界 tab 返回空串。
const char* releaseListTabLabel(int tab);

// 任务展示状态 → 所属分组：发布中 = Pending|Running、准备中 = Draft、
// 已归档 = Done|Voided。纯函数，供内存过滤与测试。
ReleaseListTab releaseListGroupOf(TaskStatus status);

// 分页模型（无 UI 依赖，可无头单测）。reset 后可见数 = min(pageSize, total)。
class ReleaseTaskPager {
public:
    explicit ReleaseTaskPager(std::size_t pageSize = kReleaseTaskPageSize);

    // 数据集变化才重置可见数；同量调用保持当前已加载量（滚动位置不跳）。
    void reset(std::size_t total);
    void setTotal(std::size_t total);

    std::size_t visible() const { return visible_; }
    std::size_t total() const { return total_; }
    std::size_t pageSize() const { return pageSize_; }
    bool hasMore() const { return visible_ < total_; }

    // 立即追加一页；返回是否真的追加（数据耗尽 = false，不改变状态）。
    bool loadMore();

    // 触底请求：进入「加载中」；加载中或数据耗尽时返回 false（去重，防重复追加）。
    bool requestLoad();
    bool loading() const { return loading_; }
    // 帧泵推进一次；返回本次是否完成追加（完成后 loading=false）。
    // 反重构警告：必须由每帧 compose 驱动，不能改成 onChange 内同步追加——
    // 那会让「加载中」占位在同一帧被新数据顶掉，占位永远不可见。
    bool advanceLoad();

private:
    std::size_t pageSize_ = kReleaseTaskPageSize;
    std::size_t total_ = 0;
    std::size_t visible_ = 0;
    bool loading_ = false;
    int framesLeft_ = 0;
};

// 任务卡视图（装配层填）：
//   id 供点击回调；envNames 是未移除环境名（顺序即展示顺序）；
//   dateLabel 为日期短标「M/D」（跨年「YYYY/M/D」，dateWide=true 时日期列
//   72→104 加宽）；nextStepLabel 为「下一步 · {步名}」的步名（Pending/Running
//   装配，空 = 主行弹性区留白）；progressDone/Total 来自任务进度；
//   status 是展示态：装配时按 release_logic::deriveStatus 时间推导，库值仅作
//   「是否已提交」的初值，推导结果不写库。
struct ReleaseTaskCard {
    std::int64_t id = 0;
    std::string planDate;
    std::string dateLabel;
    bool dateWide = false;
    TaskStatus status = TaskStatus::Draft;
    std::vector<std::string> envNames;
    int progressDone = 0;
    int progressTotal = 0;
    std::string nextStepLabel;
};

struct ReleaseListActions {
    std::function<void(std::int64_t taskId)> openTask;  // 点行 → 执行页
    std::function<void()> openNewTask;                  // 页头/空态「新建任务」→ 宿主打开弹层
};

// 装配辅助：从数据层取「一页」任务及其快照，生成卡片视图（页内顺序由 store 保证）。
// offset/limit 直接下推给 ReleaseStore::listTasksPage，不在内存切片。
// 进度口径复用 release_logic 的 taskProgress（固定步骤 + 未跳过配置数），不自算二套；
// nextStepLabel 复用 release_logic 的 nextStepLabel（与详情页「下一步」同源），
// 仅 Pending/Running 装配，Draft/Done/Voided 为空。
// 任一查询失败按 Result 原样上抛，不吞错、不回落空列表冒充「无任务」。
core::Result<std::vector<ReleaseTaskCard>> releaseLoadTaskCards(ReleaseStore& store,
                                                                int offset, int limit);

// 页面组合：页头（标题 + 新建任务按钮）+ Tab 行（模式筛选 + 组计数）+ 单列行卡。
// 滚动到底触发追加；加载中在内容底部画「加载中…」占位，失败画可重试提示。
void composeReleaseListPage(eui::Ui& ui, const eui::Screen& screen,
                            const std::vector<ReleaseTaskCard>& cards,
                            const ReleaseListActions& actions);

// 装配便捷版（页面 + 新建弹层一次画完）：按需分页读库→行卡列表→弹层。
// 首屏 offset=0/limit=kReleaseTaskPageSize；触底按 offset=已加载数再取一页。
// 读库失败保留已加载内容并显示可点击重试的提示；首屏失败且无内容时不冒充
// 「暂无任务」（空态只用于「确实 0 条」）。
void composeReleaseListPage(eui::Ui& ui, const eui::Screen& screen,
                            ReleaseStore& store,
                            ReleaseNewTaskDialogState& newTaskState,
                            const ReleaseListActions& actions);

// 骨架调用点兼容：装配层尚未注入数据时的空态页（节点 id/文案与骨架期一致）。
// 装配层改为调用上面的数据版后本重载即闲置，但保留它可避免并行开发期编译断裂。
void composeReleaseListPage(eui::Ui& ui, const eui::Screen& screen);

} // namespace module::release
