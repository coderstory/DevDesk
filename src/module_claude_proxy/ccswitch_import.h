// src/module_claude_proxy/ccswitch_import.h
// 从 cc-switch 导入 Profile：只读读侧 + 纯映射函数。
//
// 读侧（listCcSwitchImportRows）恒只读（SQLITE_OPEN_READONLY，不含 CREATE）；
// 路径与打开 flags 复用 quota/ccswitch_source 的 ccSwitchDbPath /
// kCcSwitchOpenFlags（唯一真相源一处）。映射（mapCcSwitchProviderToProfile）是
// 纯函数（无 IO / 无 DB / 无日志），单测直接断言，无需起库。
//
// 字段映射：
//   name        ← cc-switch name（同名冲突 → 跳过）
//   baseUrl     ← env.ANTHROPIC_BASE_URL（缺 → 跳过）
//   apiKey      ← ANTHROPIC_AUTH_TOKEN 优先，缺则 ANTHROPIC_API_KEY（都缺 → 跳过）
//   model       ← env.ANTHROPIC_MODEL（缺 → 空串）
//   authField   ← 两键都有 'both' / 只 TOKEN 'bearer' / 只 API_KEY 'api_key'
//   extraEnvJson      ← env 里除上述四键外的全部键值（键排序、只收字符串值、
//                       空串丢弃 —— 非逐字节原文）；双 key 并存时未用作 apiKey 的
//                       API_KEY 例外保留，不丢值
//   extraSettingsJson ← settings_config 去掉 env 块后的全部顶层属性
//   dialectOverride 留空；sharedConfigId / fallbackProfileId 置 0；modelsConfigJson 空串
//
// 密钥：apiKey 只作内存局部变量，写库一律走 ClaudeService::createProfile
// （分平台保护后入 api_key_encrypted）。本文件不写日志、不进错误文案。

#pragma once

#include <string>
#include <vector>

namespace module::claude {

// ===== 读侧 =====

// 读不到的分档（文案与排查方向各不相同；Ok 之外 rows 必为空）。
enum class CcSwitchImportStatus {
    Ok,          // 读到了（rows 可能为空——那是「cc-switch 里没配 Claude 供应商」）
    NotFound,    // 库文件不存在（没装 / 没跑过 / 找不到用户主目录）
    Unreadable,  // 存在但打不开：非 SQLite、权限不足、查询失败
    Schema,      // 打得开但 providers 表不是认识的形状（缺 id / app_type / name / settings_config）
};

// cc-switch `providers` 表的一行（app_type ∈ claude / claude-desktop）。
struct CcSwitchImportRow {
    std::string id;             // providers.id
    std::string appType;        // providers.app_type（claude / claude-desktop）
    std::string name;           // providers.name
    std::string settingsConfig; // providers.settings_config（**JSON 原文**，未解析）
};

// 一次读取的全部结果。status != Ok 时 rows 必为空且 unavailableReason 非空
//（面向用户的中文原因）——调用方**不得**把它当「没有可导入的供应商」渲染。
struct CcSwitchImportRows {
    std::vector<CcSwitchImportRow> rows;
    CcSwitchImportStatus status = CcSwitchImportStatus::Ok;
    std::string unavailableReason;
};

// 恒返回、不抛异常。行序 = providers 表扫描序。
CcSwitchImportRows listCcSwitchImportRows();

// ===== 纯映射 =====

// 映射结果。ok == true 时下面各字段有效；ok == false 时 skipReason 是面向
// 用户的中文原因（同名冲突 / 缺 base_url / 缺密钥 / JSON 解析失败 / 名称为空）。
struct CcSwitchImportMapping {
    bool ok = false;
    std::string skipReason;         //!< ok == false 时非空
    std::string name;
    std::string baseUrl;
    std::string apiKey;             //!< 仅内存；**不进任何日志 / 文案**
    std::string model;
    std::string authField;          //!< 'both' / 'bearer' / 'api_key'
    std::string extraEnvJson;       //!< 空串 = 无额外 env 键
    std::string extraSettingsJson;  //!< 空串 = 去掉 env 后无顶层属性
};

// 纯函数：cc-switch 一行 → DevDesk Profile 字段。
//   ccName             —— providers.name（映射表 name 那一行）
//   settingsConfigJson —— providers.settings_config 原文
//   existingNames      —— DevDesk 现有 profile 名；**非空集合内出现同名即跳过**
//                        （精确相等，与 claude_profiles.name 的 UNIQUE 语义
//                        一致；DB 那个 UNIQUE 是大小写敏感的，故此处亦然）。
//                        传空集合 = 不做冲突判定（纯映射单测用）。
//
// 判定顺序：settings_config 解析失败 → 名称为空 → 同名冲突 → 缺 base_url →
// 缺密钥（审计批 ①：无 AUTH_TOKEN 且无 API_KEY 与缺 base_url 同为「源缺必填
// 字段」的确定性跳过；都缺时先报缺 base_url）。
// 任一项命中都产出 ok=false + skipReason，**不产出部分字段**。
CcSwitchImportMapping mapCcSwitchProviderToProfile(
    const std::string& ccName, const std::string& settingsConfigJson,
    const std::vector<std::string>& existingNames = {});

}  // namespace module::claude