// 基础配置页（服务目录 / 环境定义）。
//
// 分层契约：只依赖 core / platform / ui_kit；ReleaseStore 是本页唯一 DB 入口。
// 渲染帧不得查库：dirty 时一次读齐两份列表，写成功后置 dirty 下一帧重读
//（每帧查库会让滚动与输入卡顿，且与 store 尚未就绪时的错误路径互相踩踏）。
//
// 时间窗一律存当天分钟数 0–1439；end <= start 视为次日（相等 = 24 小时窗）。
// 表单显示/输入 HH:MM，显示层在跨零点时追加 "(次日)"。
#pragma once

#include "core/result.h"
#include "release_store.h"
#include "release_types.h"

#include <eui/dsl.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace module::release {

// 服务类型 → 固定配置项标签（顺序即配置执行顺序）。
// 后端固定 5 项；两类前端 0 项（由类型决定，不逐服务勾选——不要给前端补配置项）。
const std::vector<std::string>& configItemLabels(ServiceKind kind);

// 类型徽标文案：后端 / PC 前端 / 移动前端。
const char* serviceKindLabel(ServiceKind kind);

// 当天分钟数 → "HH:MM"（越界值按 0..1439 夹取，仅供显示）。
std::string formatMinuteOfDay(int minute);

// end <= start 视为次日；相等也成立（09:00–09:00 = 24 小时窗）。
bool isCrossMidnight(int startMinute, int endMinute);

// "HH:MM" → 0..1439；接受首尾空白与 1–2 位小时/分钟，其余一律 false。
// 失败时不改 *minute；调用方负责给用户错误文案（本函数不抛异常）。
bool parseTimeText(const std::string& text, int* minute);

// 时间窗显示："HH:MM–HH:MM"，跨零点追加 "(次日)"。
std::string formatTimeWindow(int startMinute, int endMinute);

// 页内 UI 状态（宿主持有、跨帧复用；不要在渲染里临时构造，否则输入框与 Tab 丢状态）。
struct ReleaseConfigUiState {
    enum class Tab { Services, Envs };
    enum class Form { None, Service, Env };

    Tab tab = Tab::Services;
    Form form = Form::None;

    // 列表数据快照。composeReleaseConfigPage 在 dirty 时从 store 读入。
    std::vector<ServiceDef> services;
    std::vector<EnvDef> envs;
    bool dirty = true;       // true = 下一帧读库
    std::string loadError;   // 读库失败的人类可读原因

    // id 哨兵约定：0 = 新增/无选择（store 按 0 分配新 id；DB 自增 id 永不为 0）。
    // 删除二次确认：confirmDelete 为 true 前不得调用 store 的 delete*。
    bool confirmDelete = false;
    bool deleteIsService = true;
    std::int64_t deleteId = 0;  // 待确认删除的 id（确认前列表数据不变）
    std::string deleteError;

    // 服务表单（form == Service 时有效）。editingService 明确标记编辑态，不要用 id 推断。
    std::int64_t editServiceId = 0;
    bool editingService = false;
    std::string serviceName;
    std::string serviceBranch;
    ServiceKind serviceKind = ServiceKind::Backend;
    bool serviceKindOpen = false;
    int serviceSort = 0;
    std::string serviceError;

    // 环境表单（form == Env 时有效）。editingEnv 明确标记编辑态，不要用 id 推断。
    std::int64_t editEnvId = 0;
    bool editingEnv = false;
    std::string envName;
    std::vector<std::int64_t> envServiceIds;
    std::string envStartText = "09:00";
    std::string envEndText = "18:00";
    int envSort = 0;
    std::string envError;
};

// 写操作出口：UI 不直接碰 store；失败信息原样回显，不抛异常。
struct ReleaseConfigActions {
    std::function<core::Result<void>(const ServiceDef&)> upsertService;
    std::function<core::Result<void>(std::int64_t)> deleteService;
    std::function<core::Result<void>(const EnvDef&)> upsertEnv;
    std::function<core::Result<void>(std::int64_t)> deleteEnv;
};

// 页面入口（模块装配用）：dirty 时从 store 读一次，再渲染；写成功后由回调置 dirty。
// store 必须比本页元素树活得久（模块成员）。
void composeReleaseConfigPage(eui::Ui& ui, const eui::Screen& screen,
                              ReleaseStore& store, ReleaseConfigUiState& state);

// 纯渲染入口（无头测试 / 宿主已持有最新数据时）：只读 state.services/envs 并调用
// actions。不读库、不改列表——动作成功后由本函数置 state.dirty（宿主下一帧重读）。
void drawReleaseConfigPage(eui::Ui& ui, const eui::Screen& screen,
                           ReleaseConfigUiState& state,
                           const ReleaseConfigActions& actions);

} // namespace module::release