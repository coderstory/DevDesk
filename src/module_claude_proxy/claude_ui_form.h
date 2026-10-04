// src/module_claude_proxy/claude_ui_form.h
// 新建/编辑 Profile 表单弹窗 + 删除确认弹窗 + 表单状态机（打开 / 预填 / 导入解析
// 与应用 / 序列化 / 提交）；状态机只读改写 FormState / ImportState，无隐藏状态。
// 提交后的标脏 / 选中 / toast 经 FormSubmitHooks 显式传入，避免表单单元反向依赖列表状态。
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
    std::function<void()> markDataDirty;         // 写操作标脏
    std::function<void(int64_t id)> selectProfile;  // 新建成功后的选中态
};

// 共享配置片段提交的回调（toast / 标脏与模块侧同款）
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

// 全屏 JSON「放弃修改」：编辑缓冲恢复为上次落库值（新建/导入 = 预填值、编辑 =
// DB 值、保存成功后就地刷新）+ 清红字 + 关全屏（表单仍开）。三条入口同调本函数：
// 底部按钮 / Esc / 点遮罩；当前文本 == 落库值时等价于关闭。
void discardJsonFullscreenEdits(FormState& f);

// ---- 共享配置片段弹窗状态机 ----
void openSharedCreate(SharedConfigDialogState& s);
void openSharedEdit(SharedConfigDialogState& s, const SharedConfig& c);
void submitSharedForm(SharedConfigDialogState& s,
                      const SharedConfigSubmitHooks& hooks);

// ---- 绘制 ----
// 新建/编辑表单弹窗。sharedConfigs：片段快照（绑定下拉 + 片段赢预览数据源）；
// profiles：profile 快照（备用 Profile 下拉数据源，编辑态排除自身）
void drawProfileFormDialog(eui::Ui& ui, const eui::Screen& screen,
                           const core::theme::Tokens& t,
                           const components::theme::ThemeColorTokens& themeTokens,
                           FormState& f, ImportState& imp,
                           const std::vector<SharedConfig>& sharedConfigs,
                           const std::vector<Profile>& profiles,
                           const FormSubmitHooks& hooks);

// 高级设置 JSON 全屏编辑覆盖层：由 compose 在全部弹窗之后调用（z=1050：盖表单、
// 让 toast）。文本/红字/落库复用表单既有字段与 submitForm 路径（同一编辑缓冲）。
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

// 共享配置片段编辑弹窗：name 行 + 键值编辑器。
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
