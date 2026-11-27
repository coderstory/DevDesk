// 通知渠道管理弹窗（need527）：schedule_channel 的唯一生产写入入口。
//
// 为什么必须存在：此前全仓只有测试/探针调用 createChannel，UI 只能勾选「已存在
// 的渠道」，普通用户永远建不出渠道 → 「设了提醒但永远收不到」的 P0 根因。
//
// 分层：本头文件的纯逻辑（kind 表、account 派生、草稿校验）不碰 EUI；
// drawChannelManager 只做渲染与事件转发，落库与密钥写入经 ChannelManagerHost
// 由宿主模块（ScheduleListUi）注入。
//
// 凭据纪律：密钥只经 secrets_store::set(service, account) 写 app_secrets，
// 本层不落盘、不进日志、UI 只显示「已配置/未配置」。（§4.9）
#pragma once

#include "module_schedule/channel_registry.h"
#include "module_schedule/channels/wecom.h"  // kChannelWecom / kWecomSecretService
#include "module_schedule/schedule_store.h"

#include <core/theme.h>
#include <eui/dsl.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace module::schedule::ui {

// 渠道 kind 的一项候选（新建时的类型选择）
struct ChannelKindOption {
    std::string key;      // 落 schedule_channel.kind
    std::string label;    // 中文名
    bool needsKey = false;  // 是否需要 webhook 密钥（当前仅 wecom）
};

// 全量可选 kind（顺序即新建弹窗里的排列顺序）。
const std::vector<ChannelKindOption>& channelKindOptions();

// kind → 中文名（未知 kind 原样返回）。
std::string channelKindLabel(const std::string& kind);

// 渠道列表行（渲染用视图模型；hasKey 只表示 app_secrets 行存在，不含值）
struct ChannelRowVm {
    int64_t id = 0;
    std::string kind;
    std::string name;     // 空串 = 未命名，显示 kind 中文名
    bool enabled = true;
    bool needsKey = false;
    bool hasKey = false;
};

// 弹窗状态（纯值类型）。两种模式互斥：
//   creating=true  → 新建表单（名称 + 类型 + 密钥）
//   keyEditingId≠0 → 给既有行重设密钥（下方密钥输入行）
struct ChannelManagerState {
    bool open = false;
    bool creating = false;
    std::vector<ChannelRowVm> rows;
    // 新建草稿
    std::string nameDraft;
    std::string kindDraft = "wecom";
    std::string keyDraft;
    // 重设密钥草稿
    int64_t keyEditingId = 0;
    std::string keyEditDraft;
    std::string error;  // 落库/校验失败红字（任一编辑动作清除）
};

// 宿主注入：全部落库动作，返回空串 = 成功、非空 = 弹窗内嵌红字。
struct ChannelManagerHost {
    std::function<std::string()> reload;  // 重读 rows；返回错误串
    // 新建（密钥为空 = 不写 app_secrets，仅当 kind 需要密钥时由 UI 拦）
    std::function<std::string(const std::string& kind, const std::string& name,
                              const std::string& key)>
        create;
    std::function<std::string(int64_t id, bool enabled)> setEnabled;
    std::function<std::string(int64_t id, const std::string& key)> setKey;
    std::function<void()> close;
};

// 绘制（关闭态也常驻构建，EUI 内建收起过渡依赖节点存在）。
// 元素 id 以 "schedule.channels." 为前缀，是无头用例定位契约。
void drawChannelManager(eui::Ui& ui, const eui::Screen& screen,
                        const core::theme::Tokens& t, ChannelManagerState& st,
                        const ChannelManagerHost& host);

}  // namespace module::schedule::ui
