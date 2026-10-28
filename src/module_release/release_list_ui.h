// 任务列表（票据 05）：多列自适应瀑布流 + 首屏/触底分页 + 卡片绘制。
//
// 契约：
//   - 列表数据是装配层组装好的卡片视图（ReleaseTaskCard）。本文件不查库：
//     数据层未提供「每个任务的未移除环境 / 进度」汇总查询，硬猜字段名会与
//     数据层并行落地版本漂移。装配层用 store 查询结果填本结构。
//   - 卡片高度随内容自然变化（环境行、摘要行有/无各差一档），不强行等高；
//     分列按「当前最矮列」分配，列宽 = (可用宽 − (列数−1)×列间距) / 列数，
//     列起点 x = 列号 × (列宽 + 列间距)（首列贴内容区左缘，最右列不溢出）。
//   - 分页口径：首屏 kReleaseTaskPageSize 条；滚动到底按需向数据层再取一页
//     （offset = 已加载数），数据耗尽（已加载数 >= countTasks()）后继续触底
//     不再追加，也不再显示「加载中…」。加载中只允许一个请求在飞（重复触底
//     不得重复追加）；加载失败保留已加载内容并给出可点击重试的提示。
//   - ReleaseTaskPager 是「卡片已全部预载」场景（骨架页/无头测试）的内存分页模型；
//     真实列表页由 store 重载直接按页读库，不经过它。
//   - 回调由调用方注入；卡片点击回调携带任务 id。
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

// 瀑布流几何（视觉规范：最小卡宽 320、列间距 16、垂直间距 16）
inline constexpr float kReleaseTaskCardMinWidth = 320.0f;
inline constexpr float kReleaseTaskCardGap = 16.0f;
inline constexpr std::size_t kReleaseTaskPageSize = 10;

// 状态 chip 文案（五态唯一映射点；卡片与详情共用同一口径）。
std::string releaseTaskStatusText(TaskStatus status);

// 计划日期工具（纯逻辑，无 IO）：isoDate 必须严格为 YYYY-MM-DD 且真实存在。
// 非法日期一律返回 false / -1 / 空串，不猜「最近的有效日」。
bool releaseIsIsoDate(const std::string& isoDate);
int releaseWeekdayIndex(const std::string& isoDate);  // 0=周日 … 6=周六；非法 = -1
std::string releaseWeekdayLabel(const std::string& isoDate);  // "周二"；非法 = ""
bool releaseIsRegularReleaseDay(const std::string& isoDate);  // 周二 / 周四

// 列数：floor((可用宽 + 间距) / (最小卡宽 + 间距))，至少 1 列。
// 计算口径与卡片宽度共用，保证 1600 窗 4 列 × 320、1000 窗 2 列 × 356 不溢出。
int releaseMasonryColumns(float availableWidth,
                          float minCardWidth = kReleaseTaskCardMinWidth,
                          float gap = kReleaseTaskCardGap);

// 瀑布流落位：逐卡放进当前最矮列（同高取列号小者）。y 为该卡在内容列中的顶边。
struct ReleaseMasonrySlot {
    int column = 0;
    float y = 0.0f;
};
std::vector<ReleaseMasonrySlot> releaseMasonryLayout(
    const std::vector<float>& cardHeights, int columns, float gapY);
// 内容总高 = 所有卡片底边的最大值（不含末尾间距；空集 = 0）。
float releaseMasonryTotalHeight(const std::vector<float>& cardHeights,
                                const std::vector<ReleaseMasonrySlot>& slots,
                                float gapY);

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

    // 矮窗填满：首屏不足一屏时持续追加，直到内容高 >= viewportHeight 或数据耗尽。
    // 返回是否发生追加。列数与卡片高度口径必须与绘制时一致。
    bool fillViewport(float viewportHeight, const std::vector<float>& cardHeights,
                      int columns, float gapY);

private:
    std::size_t pageSize_ = kReleaseTaskPageSize;
    std::size_t total_ = 0;
    std::size_t visible_ = 0;
    bool loading_ = false;
    int framesLeft_ = 0;
};

// 任务卡视图（装配层填）：id 供点击回调，envNames 是未移除环境名（顺序即展示顺序），
// progressDone/Total 来自任务进度，summary 为一行摘要（空串 = 不画摘要行）。
// status 是展示态：装配时按 release_logic::deriveStatus 时间推导，库值仅作
// 「是否已提交」的初值，推导结果不写库。
struct ReleaseTaskCard {
    std::int64_t id = 0;
    std::string planDate;
    TaskStatus status = TaskStatus::Draft;
    std::vector<std::string> envNames;
    int progressDone = 0;
    int progressTotal = 0;
    std::string summary;
};

// 卡片自然高度（与 drawTaskCard 的纵向排布同源）；环境/摘要为空时少一行。
float releaseTaskCardHeight(const ReleaseTaskCard& card, float cardWidth);

struct ReleaseListActions {
    std::function<void(std::int64_t taskId)> openTask;  // 点卡片 → 执行页
    std::function<void()> openNewTask;                  // 页头/空态「新建任务」→ 宿主打开弹层
};

// 装配辅助：从数据层取「一页」任务及其快照，生成卡片视图（页内顺序由 store 保证）。
// offset/limit 直接下推给 ReleaseStore::listTasksPage，不在内存切片。
// 进度口径复用 release_logic 的 taskProgress（固定步骤 + 未跳过配置数），不自算二套。
// 任一查询失败按 Result 原样上抛，不吞错、不回落空列表冒充「无任务」。
core::Result<std::vector<ReleaseTaskCard>> releaseLoadTaskCards(ReleaseStore& store,
                                                                int offset, int limit);

// 页面组合：页头（标题 40 高 + 新建任务按钮）、瀑布流或空态。
// 滚动到底触发追加；加载中在内容底部画「加载中…」骨架占位。
void composeReleaseListPage(eui::Ui& ui, const eui::Screen& screen,
                            const std::vector<ReleaseTaskCard>& cards,
                            const ReleaseListActions& actions);

// 装配便捷版（页面 + 新建弹层一次画完）：按需分页读库→瀑布流→弹层。
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