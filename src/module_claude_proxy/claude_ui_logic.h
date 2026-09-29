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

#include <cstddef>
#include <cstdint>
#include <string>

namespace module::claude {

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

// ---- 共享配置片段（批 2，设计 §2.10；纯函数，无 IO）----

// 片段 K 键集：extra_env 全部键经 applyEnvAliases 别名归一（与写进 live 的
// 键形、服务层 sharedEnvKeysFor 同口径；空串/非法 JSON → 空集，读路径容错）。
// 片段赢预览的置灰行 / 新增同键拦截的判定源
std::vector<std::string> sharedFrozenKeys(const SharedConfig& sc);

// 键名归一（同写侧口径）：别名 → 官方键名，未命中透传原名。
// 行键与 K 比对前先归一——「用户敲了别名、片段里是官方名」也算同键
std::string normalizedEnvKey(const std::string& k);

// 片段按 id 查找（快照定位；未绑定/不存在 → nullptr）
const SharedConfig* findSharedById(const std::vector<SharedConfig>& configs,
                                   int64_t id);

// 键集成员判定（小向量线性扫，片段键集 ≤64）
bool keyIn(const std::vector<std::string>& keys, const std::string& k);

// 「复制配置」序列化（2026-09-30 batch-fix C，设计 §4.2②；纯函数，无 IO）：
// profile → settings.json 形态 JSON（{"env": {...}}）。内部复用 buildDirectEnv
// 纯函数（BASE_URL 去尾斜杠 / 密钥键三态 / 槽位 / extra_env 别名归一 / 共享
// 片段叠加——与「运行」临时 settings 同一条管线）。
// keyPlaceholder = 凭据键的占位值："sk-ant-***<尾4>"（与详情页 apiKeyHint 同一
// 脱敏口径，已裁决 §5②）；**不导明文 key**（剪贴板是跨应用共享面，§5① 裁决）；
// 传空串 → buildDirectEnv 空值不写凭据键，导出即无 token 键（Keychain 读不到
// 的兜底形态）。序列化失败（理论不可达）→ 返回空串，调用方按失败处理。
std::string buildProfileExportJson(const Profile& p,
                                   const std::string& sharedExtraEnvJson,
                                   const std::string& keyPlaceholder);

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
