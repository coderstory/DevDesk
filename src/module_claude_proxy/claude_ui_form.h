// src/module_claude_proxy/claude_ui_form.h
// R4（P-B5）表单单元：新建/编辑 Profile 表单弹窗（tabs 三页）+ 删除确认弹窗，
// 以及表单状态机（打开 / 预填 / 导入解析与应用 / 序列化 / 提交）。
//
// 自由函数（spec §3.2）：`serializeFormData` 随之搬迁；表单状态机的其余成员
// （openCreateForm / openEditForm / doParseImport / doApplyImport /
// doImportCurrentConfig / submitForm）同样只读改写 FormState / ImportState，
// 故一并落在本单元 —— 它们不含 compose 渲染路径的隐藏状态，搬出后
// claude_module.cpp 只留「接线 + 服务编排」。
//
// 提交后要通知模块的两件事（标脏数据缓存 / 选中新 Profile）与 toast 经
// FormSubmitHooks 显式传入，避免表单单元反向依赖列表状态。
//
// 零行为变化：文案、几何、控制流、toast 标题与文案逐字保留。
#pragma once

#include "claude_service.h"
#include "claude_ui_state.h"

#include <components/theme.h>
#include <core/theme.h>
#include <eui/dsl.h>

#include <cstdint>
#include <functional>
#include <string>

namespace module::claude {

// 表单提交 / 导入的回调（compose 每帧装配；toast 与模块状态留在模块侧）
struct FormSubmitHooks {
    std::function<void(const std::string& title, const std::string& msg,
                       bool isError)> toast;
    std::function<void()> markDataDirty;         // 写操作标脏（审计 perf [13]）
    std::function<void(int64_t id)> selectProfile;  // 新建成功后的选中态
};

// 共享配置片段提交的回调（批 2，设计 §2.10；toast / 标脏与模块侧同款）
struct SharedConfigSubmitHooks {
    std::function<void(const std::string& title, const std::string& msg,
                       bool isError)> toast;
    std::function<void()> markDataDirty;
};

// ---- 表单状态机 ----
void openCreateForm(FormState& f, ImportState& imp);
void openEditForm(FormState& f, ImportState& imp, const Profile& p,
                  const std::vector<SharedConfig>& sharedConfigs);
void resetImportState(ImportState& imp);
void doParseImport(ImportState& imp);
void doApplyImport(FormState& f, ImportState& imp);
// 表单 → models_config / extra_env JSON（全空 = 空串，保持「未配置」语义）；
// 失败写 f.formError_ 并返回 false
bool serializeFormData(FormState& f, std::string* modelsJson, std::string* envJson);
// 「导入当前配置」：读 settings.json → 打开新建表单并预填（失败 toast，不弹窗）
void startImportCurrentConfig(FormState& f, ImportState& imp,
                              const FormSubmitHooks& hooks);
// sharedConfigs：共享配置片段快照（悬空绑定防御 + 新增同键拦截的片段名文案）
void submitForm(FormState& f, const FormSubmitHooks& hooks,
                const std::vector<SharedConfig>& sharedConfigs);

// 全屏 JSON「放弃修改」（batch-C C-2）：编辑缓冲恢复为**上次落库值**
//（f.formExtraSettingsJsonBaseline_——新建/导入 = 预填值、编辑 = DB 值、保存
// 成功后就地刷新）+ 清 JSON 行内红字 + 关全屏（表单仍开）。三条入口同调本函数：
// 底部「放弃修改」按钮 / Esc / 点遮罩。当前文本 == 落库值时等价于关闭，不报错。
void discardJsonFullscreenEdits(FormState& f);

// ---- 共享配置片段弹窗状态机（批 2）----
void openSharedCreate(SharedConfigDialogState& s);
void openSharedEdit(SharedConfigDialogState& s, const SharedConfig& c);
void submitSharedForm(SharedConfigDialogState& s,
                      const SharedConfigSubmitHooks& hooks);

// ---- 绘制 ----
// 新建/编辑表单弹窗（tabs 三页，§6.1）。sharedConfigs：片段快照（绑定下拉 +
// 片段赢预览的数据源）；profiles：profile 快照（批 3 备用 Profile 下拉的数据源，
// 编辑态排除自身）
void drawProfileFormDialog(eui::Ui& ui, const eui::Screen& screen,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& themeTokens,
                           FormState& f, ImportState& imp,
                           const std::vector<SharedConfig>& sharedConfigs,
                           const std::vector<Profile>& profiles,
                           const FormSubmitHooks& hooks);

// 高级设置 JSON 全屏编辑覆盖层（2026-09-30 json-fullscreen）：窗口级覆盖层，
// 由 compose 在全部弹窗之后调用（z=1050：盖表单弹窗默认 1000、让位 toast
// 1100）。可见性派生自 formOpen_ && formJsonFullscreen_（应用成功关表单 →
// 全屏同帧消失）；文本/红字/落库全部复用表单既有字段与 submitForm 路径——
// 同一编辑缓冲的大画布，不出现第二份状态
void drawJsonFullscreenOverlay(eui::Ui& ui, const eui::Screen& screen,
                               const core::theme::Tokens& t,
                               const components::theme::ThemeColorTokens& themeTokens,
                               FormState& f, const FormSubmitHooks& hooks,
                               const std::vector<SharedConfig>& sharedConfigs);

// 删除确认弹窗（确认后回调模块执行真删除）
void drawDeleteConfirmDialog(eui::Ui& ui, const eui::Screen& screen,
                             const core::theme::Tokens& t,
                             const components::theme::ThemeColorTokens& themeTokens,
                             FormState& f,
                             const std::function<void(int64_t id)>& onConfirmDelete);

// 共享配置片段编辑弹窗（批 2）：name 行 + 键值编辑器（kMaxVisibleRows 同款）。
// 编辑态底部多一枚「删除」键（开删除确认，删除 = 同事务解绑）
void drawSharedConfigDialog(eui::Ui& ui, const eui::Screen& screen,
                            const core::theme::Tokens& t,
                            const components::theme::ThemeColorTokens& themeTokens,
                            SharedConfigDialogState& s,
                            const SharedConfigSubmitHooks& hooks);

// 片段删除确认弹窗（确认后回调模块执行真删除；文案含「N 个 Profile 已解绑」）
void drawSharedConfigDeleteConfirmDialog(
    eui::Ui& ui, const eui::Screen& screen, const core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& themeTokens,
    SharedConfigDialogState& s,
    const std::function<void(int64_t id, const std::string& name)>&
        onConfirmDelete);

} // namespace module::claude
