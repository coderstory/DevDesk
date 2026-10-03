// src/module_claude_proxy/ccswitch_import.h
// 从 cc-switch 导入 Profile：**只读读侧** + **纯映射函数**（2026-10-03）
//
// 用户需求原话：「profile 新增从 cc-switch 导入功能」「需要导入全部的配置内容」。
//
// ── 为什么分成这两层 ───────────────────────────────────────────────────────
//   - 读侧（listCcSwitchImportRows）：碰 cc-switch 的 SQLite 库，**恒只读**
//     （SQLITE_OPEN_READONLY，不含 READWRITE / CREATE —— 打开不存在的路径
//     不许把它创建出来）。路径与打开 flags 直接复用 quota/ccswitch_source 的
//     已交付能力（ccSwitchDbPath / kCcSwitchOpenFlags 均为 public），**唯一
//     真相源只有一处**；新增的只有「多取 app_type=claude-desktop + 取回
//     settings_config 原文」两点。
//   - 映射（mapCcSwitchProviderToProfile）：**纯函数**（无 IO / 无 DB / 无
//     日志 / 不看全局状态），cc-switch 一行 → DevDesk Profile 字段，逐项对应
//     见下表。单测直接对它断言，无需起库。
//
// ── 字段映射表（主会话 2026-10-03 裁决，不自行改）────────────────────────
//   | DevDesk                | 来源                                                     |
//   |------------------------|----------------------------------------------------------|
//   | name                   | cc-switch `name`（**同名冲突 → 跳过**，见 existingNames）    |
//   | baseUrl                | `env.ANTHROPIC_BASE_URL`（缺 → 跳过，原因「缺 base_url」）  |
//   | apiKey                 | `env.ANTHROPIC_AUTH_TOKEN` 优先，缺则 `env.ANTHROPIC_API_KEY` |
//   | model                  | `env.ANTHROPIC_MODEL`（缺 → 空串）                         |
//   | authField              | 两键都有 → 'both' / 只有 TOKEN → 'bearer' /                |
//   |                        | 只有 API_KEY → 'api_key' / 都无 → 'both'（值域同既有）    |
//   | extraEnvJson           | `env` 里**除** BASE_URL / AUTH_TOKEN / API_KEY / MODEL 外   |
//   |                        | 全部键值（**JSON 原文**）                                  |
//   | extraSettingsJson      | settings_config **去掉 env 块后**的全部顶层属性（原文）     |
//   | dialectOverride        | 留空（既有 host 推断自动处理）                             |
//   | sharedConfigId /        | 0                                                            |
//   | fallbackProfileId      |                                                              |
//   | modelsConfigJson       | 空串                                                        |
// 「导入全部的配置内容」= 上面两个 JSON 字段**完整保留，不丢键**。
//
// ── 密钥去向 ─────────────────────────────────────────────────────────────
// apiKey 只在这里作为**内存局部变量**存在；写库一律走既有
// ClaudeService::createProfile（凭据经 platform::credential 分平台保护后入
// claude_profiles.api_key_encrypted，见 profile_repository.cpp:398）。本文件
// **不写日志、不产出错误文案、不进任何返回值文案** —— 返回结构里的 apiKey 是
// 导入流程的必要中间量，与 Profile.apiKeyPlain 同一性质（「用完即丢弃」）。
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
// 用户的中文原因（同名冲突 / 缺 base_url / JSON 解析失败 / 名称为空）。
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
// 判定顺序：settings_config 解析失败 → 名称为空 → 同名冲突 → 缺 base_url。
// 任一项命中都产出 ok=false + skipReason，**不产出部分字段**。
CcSwitchImportMapping mapCcSwitchProviderToProfile(
    const std::string& ccName, const std::string& settingsConfigJson,
    const std::vector<std::string>& existingNames = {});

}  // namespace module::claude