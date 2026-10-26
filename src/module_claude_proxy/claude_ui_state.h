// Claude UI 瞬时状态：按「可视区域」收成的纯值 struct（无方法、无虚接口）。
// ProfileListState = 左栏列表；DetailState = 详情页签；FormState = 表单 + 弹窗
#pragma once

#include "claude_service.h"
#include "ccswitch_import.h"
#include "provider_config.h"

#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <utility>
#include <vector>

namespace module::claude {

// 数据刷新：工作线程产物（不触碰 this，结果经 future 值回渲染线程再落成员）。
struct DataRefresh {
    bool wantProfiles = false;
    std::vector<Profile> profiles;
    std::string profilesError;
    Profile active;
    bool hasActive = false;
};

// 左栏：Profile 列表 + header/引导条 + 数据缓存
struct ProfileListState {
    // compose 数据缓存（刷新语义见 refreshDataCache）：初值脏 = 首帧必查
    std::vector<Profile> cachedProfiles_;
    std::string cachedProfilesError_;
    Profile cachedActive_;
    bool cachedHasActive_ = false;
    bool dataDirty_ = true;
    bool dataLoaded_ = false;   // 快照是否装载过（冷启动兜底判据，见 refreshDataCache）
    // shared_future = 结果共享状态（worker 发布 → 唤醒 → 渲染线程收割）；async future 留作 join 句柄。在途判据 = refreshFuture_.valid()
    std::shared_future<DataRefresh> refreshFuture_;
    std::future<void> refreshWorker_;
    std::chrono::steady_clock::time_point lastRefreshPost_{};  // 节流窗口起点
    static constexpr int64_t kRefreshIntervalMs = 200;

    // 选中态（0 = 跟随激活 profile）
    int64_t selectedId_ = 0;
    // Profile 列表滚动位置
    float profilesScrollOffset_ = 0.0f;
    // 引导条一次性标记（会话内代理从未启动过才显示；内存态）
    bool proxyEverSeen_ = false;
    // S3.4 引导条手动关闭（用户点 × 收起）
    bool guideDismissed_ = false;
    // 切换 Profile 二次确认弹窗（UAT6）：点非选中卡先弹窗，确定后才切 selectedId_
    bool switchConfirmOpen_ = false;
    int64_t switchTargetId_ = 0;
    std::string switchTargetName_;
    bool switchBypassConfirm_ = false;  // 测试缝：true 时直接切不弹窗（全量测试回归门控）
    // 列表右键菜单：目标 profile id（0 = 无）+ 锚点（页面坐标）+ 开合标志
    int64_t ctxProfileId_ = 0;
    float ctxMenuX_ = 0.0f;
    float ctxMenuY_ = 0.0f;
    bool ctxMenuOpen_ = false;
};

struct DetailState {
    // 0 基础信息 / 1 模型配置 / 2 环境变量
    int detailTab_ = 0;
};

// 新建/编辑表单 + 删除确认弹窗
struct FormState {
    bool formOpen_ = false;
    int64_t formEditId_ = 0;   // 0 = 新建，非 0 = 编辑该 id
    int formTab_ = 0;          // 0 基础信息 / 1 模型配置 / 2 环境变量
    std::string formName_;
    std::string formUrl_;
    std::string formKey_;
    std::string formKeyHint_;  // 编辑时显示现有 key 脱敏提示
    std::string formError_;
    AuthField formAuthField_ = AuthField::Both;  // 认证字段（鉴权头三态）
    bool formAuthFieldOpen_ = false;             // 下拉展开态
    ModelsConfig formModels_;  // 槽位表全行（默认模型 = def 槽位）
    std::vector<std::pair<std::string, std::string>> formEnvRows_;  // env 键值行

    bool deleteConfirmOpen_ = false;
    int64_t deleteTargetId_ = 0;
    std::string deleteTargetName_;
};

// cc-switch 导入弹窗（独立状态，不塞进 FormState：复用会污染表单状态）。必须接入 anyDialogOpen 门控与 closeTopmostDialog Esc 序链（最顶层）
struct CcSwitchImportDialogState {
    bool open_ = false;
    bool loaded_ = false;             // rows 已装载（false = 首次打开需触发读库）
    // 候选行 + 每行映射结果 + 是否勾选；三个向量同长度、按下标对齐
    std::vector<CcSwitchImportRow> rows_;
    std::vector<CcSwitchImportMapping> mappings_;
    std::vector<bool> selected_;
    // 读库失败的中文原因（空 = 成功 / 未尝试）。非空时列表区不画，改画错误态。
    std::string loadError_;
    // 导入结果报告（每条「name: 成功 / 跳过(原因) / 失败(原因)」一行；非空 = 已跑过导入）
    std::vector<std::string> resultLines_;
    int resultSuccess_ = 0;
    int resultSkipped_ = 0;
    int resultFailed_ = 0;
};

struct ClaudeUiState {
    ProfileListState list;
    DetailState detail;
    FormState form;
    CcSwitchImportDialogState ccswitchImport;  // 从 cc-switch 导入弹窗
};

// 数据缓存的后台刷新：compose 每帧只做「搬已就绪结果进快照 + 发下一次刷新意图」两件无 IO 的事；SQLite 查询全在后台（同步查会与代理 worker 争锁）。失败保持脏、按节流窗口重试。

// 唯一跑在后台线程的 DB 入口（static 语义：不碰模块成员）
DataRefresh runDataRefresh(ClaudeService* svc, bool wantProfiles);

// 落快照（只在渲染线程调：onStart 首次装载 + compose 收割/兜底）
void applyDataRefresh(ProfileListState& st, DataRefresh r);

// 收割：后台结果就绪才搬（wait_for(0) 不阻塞；未就绪快照沿用上次值）
void harvestDataRefresh(ProfileListState& st);

// 节流：失败重试 / 连续写操作标脏时按窗口合并
bool refreshDue(const ProfileListState& st);

// compose 每帧调：只发意图 + 搬快照，零 IO（脏 = 进页/写操作后重查）
void refreshDataCache(ProfileListState& st, ClaudeService* svc);

} // namespace module::claude
