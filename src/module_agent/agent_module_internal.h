// agent_module_internal.h —— AgentModule 拆分共享件（仅本模块内包含）。
// 拆分（04 号票）产物：agent_module.cpp 为门面（生命周期 / compose / 键事件 / 工厂），
// 调度（agent_module_schedule.cpp）/ 工具（agent_module_tools.cpp）/ UI
//（agent_module_dialogs.cpp）三块成员函数在各自 TU 出行定义；本头文件聚齐跨 TU
// 共用的助手（页内错误分类 + 模型槽位键表），并集中各 TU 所需 include。
// 对外仍只有 create_agent_module() 与 agent_module.h，本头文件不许被模块外包含
//（消费方零改动的边界）。
#pragma once

#include "agent_module.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <system_error>
#include <utility>

#include <components/button.h>
#include <components/dialog.h>
#include <components/dropdown.h>
#include <components/input.h>
#include <components/scrollview.h>
#include <components/segmented.h>
#include <components/switch.h>
#include <core/window/window_backend.h>
#include <core/platform/platform.h>
#include <eui/dsl.h>
#include <ui_kit/dialog_form.h>
#include <ui_kit/metrics.h>
#include <ui_kit/button.h>
#include <ui_kit/card.h>
#include <ui_kit/wswitch.h>
#include <ui_kit/toast.h>

#include "model_slots.h"

#include "core/component_theme.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/router.h"
#include "core/service_locator.h"
#include "core/theme.h"
#include "core/time.h"
#include "platform/notification.h"
#include "module_agent/agent_loop.h"
#include "module_agent/agent_time_text.h"
#include "module_agent/agent_builtin_tools.h"
#include "module_agent/app_notify_tool.h"
#include "module_agent/app_now_tool.h"
#include "module_agent/app_stats_tool.h"
#include "module_agent/cron.h"
#include "module_agent/hot_tool.h"
#include "module_agent/message_blocks.h"
#include "module_agent/net_fetch_tool.h"
#include "module_agent/task_draft_parse.h"
#include "module_agent/tool_info.h"
#include "module_agent/trigger_config.h"
#include "module_agent/agent_system_prompt.h"
#include "module_agent/agent_ui_shared.h"

namespace module::agent {

// needsClaudePageHint 的 inline 定义在 agent_module.h（04 拆分回归修正：非 inline
// 声明 + 这里的 inline 定义在 macOS Release 下 undefined symbol——本头含 agent_module.h，
// 消费方经它拿到同一个 inline 实体，无需重复提供）。

// 真源 = src/model_slots.h 的 X 宏表，新增槽位只改那里；键集进库（agent_tasks.model_slot），显示名只上屏
#define DEVDESK_MODEL_SLOT_KEY(name, jsonKey, label, formLabel, envKey, nameKey, oneM) jsonKey,
constexpr const char* kModelSlotKeys[] = {DEVDESK_MODEL_SLOT_LIST(DEVDESK_MODEL_SLOT_KEY)};
#undef DEVDESK_MODEL_SLOT_KEY
#define DEVDESK_MODEL_SLOT_LABEL(name, jsonKey, label, formLabel, envKey, nameKey, oneM) label,
constexpr const char* kModelSlotLabels[] = {
    DEVDESK_MODEL_SLOT_LIST(DEVDESK_MODEL_SLOT_LABEL)};
#undef DEVDESK_MODEL_SLOT_LABEL
constexpr int kModelSlotCount = static_cast<int>(
    sizeof(kModelSlotKeys) / sizeof(kModelSlotKeys[0]));

inline int slotIndexOf(const std::string& slotKey) {
    for (int i = 0; i < kModelSlotCount; ++i) {
        if (slotKey == kModelSlotKeys[i]) return i;
    }
    return 0;
}

} // namespace module::agent
