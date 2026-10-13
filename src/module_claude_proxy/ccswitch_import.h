// src/module_claude_proxy/ccswitch_import.h
// 从 cc-switch 导入 Profile：只读读侧 + 纯映射函数。
// 读侧恒只读（路径与打开 flags 复用 quota/ccswitch_source）；映射为纯函数（无 IO / 无 DB / 无日志）。
// apiKey 只作内存局部变量，写库一律走 ClaudeService::createProfile；本文件不写日志、不进错误文案。

#pragma once

#include <string>
#include <vector>

namespace module::claude {

// 读侧

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

// status != Ok 时 rows 必为空且 unavailableReason 非空（面向用户的中文原因）——
// 调用方**不得**把它当「没有可导入的供应商」渲染。
struct CcSwitchImportRows {
    std::vector<CcSwitchImportRow> rows;
    CcSwitchImportStatus status = CcSwitchImportStatus::Ok;
    std::string unavailableReason;
};

// 恒返回、不抛异常。行序 = providers 表扫描序。
CcSwitchImportRows listCcSwitchImportRows();

// 纯映射

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

// 纯函数：cc-switch 一行 → DevDesk Profile 字段；existingNames 非空时同名即跳过
//（精确相等，大小写敏感，与 claude_profiles.name 的 UNIQUE 语义一致）。
// 判定顺序：JSON 解析失败 → 名称为空 → 同名冲突 → 缺 base_url → 缺密钥；
// 任一项命中都产出 ok=false + skipReason，**不产出部分字段**。
CcSwitchImportMapping mapCcSwitchProviderToProfile(
    const std::string& ccName, const std::string& settingsConfigJson,
    const std::vector<std::string>& existingNames = {});

}  // namespace module::claude