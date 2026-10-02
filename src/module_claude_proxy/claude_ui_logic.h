// src/module_claude_proxy/claude_ui_logic.h
// R4（P-B1）纯逻辑单元：claude_module.cpp 匿名命名空间里「不碰 Ui、不碰 DB」的
// 4 个自由函数 + 调用记录弹窗常量，提升到本头/实现后即可无头单测
//（spec 2026-09-27-god-class-refactor-design §3.2 / §5.2 第 4 条：
// 「能进 logic 头的就进」）。
//
// 零行为变化：函数体与常量取值逐字搬运（含注释），只改归属与链接性
//（匿名 ns 的 internal linkage → 本模块内可见）；调用点全部按名重定位，
// 不动任何取值。
#pragma once

#include "claude_service.h"

#include <core/platform/platform.h>   // core::platform::requestUiUpdate

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <string>

namespace module::claude {

// 批次 D 评审收尾（2026-10-02）：后台任务**结果就绪 → 唤醒主循环**的唯一出口。
//
// 为什么要有这个函数：UI 主循环在无动画/无输入时阻塞在 glfwWaitEvents
//（EUI-NEO core/app/glfw_app_main.cpp 帧末），后台跑完不唤醒 → 结果没人 recompose，
// 要等用户下一次动鼠标才上屏（本批次主诉「加载特别慢」的根因）。
//
// 次序不能反（评审定的 major）：std::async 在 callable **返回之后**才发布值，
// 若在任务里先唤醒、后 set_value，UI 会抢跑——compose 收割时 wait_for(0) 看到
// 「未就绪」→ 唤醒位已被消费、脏标记已清 → 结果落在内存里没人再上屏，原症状
// 间歇复现。框架惯例：EUI-NEO core/async::finishTask（先落状态再 postReadyEvent）、
// core/render/image_source.cpp、core/platform/network.cpp。
//
// 把「探就绪」和「唤醒」绑在同一个出口里，是为了让次序可判定：seam 在**唤醒那一刻**
// 真读一次共享状态（不是另找一处写死 true）。次序写反 / 绕过本函数直接唤醒 →
// seam 停在 false → 无头用例判红（见 test_claude_calls_latency.cpp）。
template <typename T>
void wakeAfterPublish(const std::shared_future<T>& published,
                      std::atomic<bool>& sawReadySeam) {
    sawReadySeam.store(published.wait_for(std::chrono::seconds(0)) ==
                           std::future_status::ready,
                       std::memory_order_relaxed);
    // 不唤醒 = 结果要等用户下一次输入；glfwPostEmptyEvent 文档标注
    // 「may be called from any thread」，后台线程调用合法。
    // 用 core 层而不是 app::requestUpdate()：后者的定义只编进 app main TU
    //（glfw_app_main.cpp），probe/无头链接报 Undefined symbols。
    core::platform::requestUiUpdate();
}

// 格式化毫秒数为 "1234ms" / "1.2s"
std::string formatLatency(int64_t ms);

// 按字节截断 UTF-8 串（审计 [11]：错误行尾附 error 摘要，过长会挤爆行宽）。
// 截点回退到字符边界，不把多字节字符切成半个（乱码方块）
std::string truncateUtf8(const std::string& s, size_t maxBytes);

// 模型映射摘要（§6.2 详情面板一行式）：「默认 <model> · 槽位已配 N/M」
// 与分母 M 都取自槽位表（审计 CPX-08：不再手写槽位清单与硬编码 /6）
std::string modelsSummary(const Profile& p);

// 「导入当前配置」名称建议：BASE_URL host（剥 scheme/路径/端口），
// 提取不到（无 BASE_URL）→「导入 <日期>」；用户可改
std::string suggestImportName(const std::string& baseUrl);

// ---- 共享配置片段（批 2 建立；advanced-json-editor §8.2 拆除 frozenKeys
// ---- 家族：片段不再携带 env 键，sharedFrozenKeys/normalizedEnvKey/keyIn
// ---- 随 UI 冻结视觉一同移除，findSharedById 保留）----

// 片段按 id 查找（快照定位；未绑定/不存在 → nullptr）
const SharedConfig* findSharedById(const std::vector<SharedConfig>& configs,
                                   int64_t id);

// 「复制配置」序列化（2026-09-30 batch-fix C §4.2② 建立；advanced-json-editor
// §6 升级为**完整 settings.json 形态**，已拍板②；纯函数，无 IO）：
// env 域复用 buildDirectEnv 纯函数（BASE_URL 去尾斜杠 / 密钥键三态 / 槽位 /
// extra_env 别名归一——与「运行」临时 settings 同一条管线）；非 env 域追加
// effectiveExtraSettingsJson(p.extraSettingsJson, fragmentJson) 并入顶层
//（片段叠加 + 片段赢，advanced-json-editor §4.2）。fragmentJson = 所绑片段的
// extra_settings_json 原文（JSON 域，自 cachedSharedConfigs_ 快照取；空串 =
// 未绑定）。序列化 = stringify(pretty)——完整形态 = 用户可直接粘贴进
// settings.json 的文本（键序为字典序，D-2 同一条通道）。
// keyPlaceholder = 凭据键的占位值："sk-ant-***<尾4>"（与详情页 apiKeyHint 同一
// 脱敏口径，已裁决 §5②）；**不导明文 key**（剪贴板是跨应用共享面，§5① 裁决）；
// 传空串 → buildDirectEnv 空值不写凭据键，导出即无 token 键（Keychain 读不到
// 的兜底形态）。序列化失败（理论不可达）→ 返回空串，调用方按失败处理。
std::string buildProfileExportJson(const Profile& p,
                                   const std::string& fragmentJson,
                                   const std::string& keyPlaceholder);

// 「复制配置」toast 副文案（advanced-json-editor §6：完整形态升级后同步；
// 常量落 logic 层供无头断言，模块层直出）
inline constexpr const char* kCopyConfigToastSubtitle =
    "已把该 Profile 的完整 settings.json 配置（env + 高级属性）导出到剪贴板"
    "（API Key 为占位符，不含明文）";

// ---- 调用记录弹窗常量（审计 [11]）----
//
// 调用记录弹窗状态码过滤（审计 [11]，spec §8.3 第 6 条「支持过滤、搜索」）：
// 下拉索引 → ClaudeService::CallLogQuery::statusClass 映射
inline constexpr int kCallsStatusClasses[] = {0, 2, 3, 4, 5, 9};
inline constexpr const char* kCallsStatusLabels[] = {
    "全部状态", "2xx 成功", "3xx 重定向", "4xx 客户端错误",
    "5xx 服务端错误", "错误（超时等）"};
inline constexpr int kCallsStatusCount = 6;
// 日志弹窗每页条数（审计 [11]；总设计只承诺「过滤、搜索」，条数由 UI 自定）
inline constexpr int kCallsPageSize = 20;
// 搜索框防抖窗口（审计 CPX-06）：每次按键顺延窗口，停手满一个窗口才派发一次
// 后台查询（窗口内多次按键合并成一次）；250ms = 常见搜索防抖档
inline constexpr int kCallsSearchDebounceMs = 250;

} // namespace module::claude
