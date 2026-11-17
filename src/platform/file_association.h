// src/platform/file_association.h
// 文件关联平台抽象（F-18 记事本多扩展关联开关，取代 F-06 单 .md 版）：头平台
// 无关——配置注入缝 + 三态纯逻辑 inline 在头里，实现按 *_win/_macos 拆（与
// autostart / file_open 同一模式，业务代码零平台宏）。
// 语义基准 = 方案卡 §十（need263 批 4 棒 B）+ F-07 判定规则
//（docs/research/2026-11-10-f07-win-userchoice-poc.md，clue10）：
//   · 「已生效」唯一判据 = 查询回读（QueryCurrentDefault）归本应用；
//     写入成功 ≠ 双击行为生效（Win10 22H2 实测：注册表写入层生效、行为面不接管）。
//   · Unknown = 查询失败 ≠ Others（按需修复档：触发一次重写 + 未生效呈现，
//     不与 Others 混判）。
#pragma once

#include <string>
#include <vector>

#include "core/notepad_file_types.h"  // 扩展清单单一真源（core 层共享）
#include "core/result.h"

namespace platform::file_association {

// 三态归属：Ours=查询回读 ProgId==本应用；Others=回读他者 ProgId；
// Unknown=查询失败（≠Others，语义见头注释）
enum class AssocState { Ours, Others, Unknown };

// 登记参数（注入缝）：ProgId 名与目标扩展名清单可替换。生产 = DevDesk.notepad +
// core::notepad 全清单（26 项）；单测必须用 testConfig()（DevDeskTest.notepad +
// .mdtest/.notepadtest）——用例禁触开发机真实关联登记（测试红线：防用例劫持
// 真实用户级关联）。
struct AssocConfig {
    std::string progId;  // 判属锚：回读比对 ProgId 串而非 exe 路径（exe 挪位不影响判属）
    std::vector<std::string> extensions;  // 含点扩展名清单（F-18 多扩展化）
};

// 生产配置：ProgId = DevDesk.notepad；扩展清单 = core::notepad::notepadExtensions()
// 全量（单一真源，改清单只改 core 头，判据同步生效）。
inline AssocConfig productionConfig() {
    AssocConfig cfg;
    cfg.progId = "DevDesk.notepad";
    cfg.extensions.reserve(core::notepad::notepadExtensions().size());
    for (const std::string_view ext : core::notepad::notepadExtensions()) {
        cfg.extensions.emplace_back(ext);
    }
    return cfg;
}

// 测试配置：双扩展隔离（per-extension 循环/清理/聚合逻辑可测），扩展名均为
// 现实世界不存在的测试键（禁触真实关联）。
inline AssocConfig testConfig() {
    return {"DevDeskTest.notepad", {".mdtest", ".notepadtest"}};
}

// ProgId (Default) 描述串（登记内容组成部分，单测比对锚）
inline constexpr const char* kProgIdDescription = "DevDesk Notepad Document";

// 旧版单 .md 方案的 ProgId（F-06 时代）：仅用于①开启时检测归己的旧注册迁移
//（重写为新 ProgId 并清理旧 ProgId 树，幂等）②unregisterAll 的归己判定
//（旧注册同样视为「归本应用」可清）。新登记不再写它。
inline constexpr const char* kLegacyMdProgId = "DevDesk.md";

// ── 纯逻辑（inline，跨平台直测）──

// shell\open\command 值："<exe>" "%1"——exe 与 %1 各自带引号（路径/文件名含空格不拆参）
inline std::string buildOpenCommandValue(const std::string& exePath) {
    return "\"" + exePath + "\" \"%1\"";
}

// DefaultIcon 值："<exe>",0（首图标；引号规则同上）
inline std::string buildDefaultIconValue(const std::string& exePath) {
    return "\"" + exePath + "\",0";
}

// 三态判定（纯函数）：查询失败（queryOk=false）= Unknown；回读==本应用 ProgId =
// Ours；否则 Others
inline AssocState classifyState(bool queryOk, const std::string& currentProgId,
                                const std::string& ourProgId) {
    if (!queryOk) return AssocState::Unknown;
    return currentProgId == ourProgId ? AssocState::Ours : AssocState::Others;
}

// 多扩展归属聚合（纯函数，F-18「任一扩展未生效即整体未生效」）：全 Ours = Ours；
// 含 Unknown = Unknown（查询失败优先于 Others——按需修复档的成因不是他者占用）；
// 否则（含 Others）= Others。空表 = Unknown（无数据 ≠ 已生效）。
inline AssocState aggregateAssocStates(const std::vector<AssocState>& states) {
    if (states.empty()) return AssocState::Unknown;
    bool anyUnknown = false;
    bool anyOthers = false;
    for (const AssocState s : states) {
        if (s == AssocState::Ours) continue;
        if (s == AssocState::Unknown) {
            anyUnknown = true;
        } else {
            anyOthers = true;
        }
    }
    if (anyUnknown) return AssocState::Unknown;
    return anyOthers ? AssocState::Others : AssocState::Ours;
}

// ── 系统触达（Windows 实现在 file_association_win.cpp；macOS 在 *_macos.mm）──

// 查询单个扩展名生效归属：QueryCurrentDefault(<ext>, AT_FILEEXTENSION,
// AL_EFFECTIVE) 回读 ProgId 串 → 三态。查询失败一律落 Unknown（Result 仍 ok——
// 「查询通路失败」是 Unknown 态的成因之一，不是独立错误面）。
core::Result<AssocState> isDefaultFor(
    const std::string& extension,
    const AssocConfig& config = productionConfig());

// 全清单聚合归属：逐扩展查询 + aggregateAssocStates（Windows 全扩展逐查；
// macOS 跳过与系统 UTI 冲突不可登记的扩展——见 *_macos.mm 头注）。
core::Result<AssocState> overallState(
    const AssocConfig& config = productionConfig());

// 登记全部扩展名（F-18 开启路径）：Windows = HKCU\Software\Classes\<progId>
// ProgId 树一次 + 逐扩展默认值指向；写完必调 SHChangeNotify(SHCNE_ASSOCCHANGED)
//（官方 must-call，注入缝 spy 断言整批恰一次）。macOS = 逐扩展 LS/NSWorkspace
// 写入（12+ 批量 async，防逐项确认框挂 UI）。幂等：重复执行写同一目标值。
// 旧注册迁移（Windows）：.md 当前归旧 kLegacyMdProgId 时重写为新 ProgId 并清理
// 旧 ProgId 树（检测归己才动，幂等）。
// 返回 bool = 写入后聚合回查归本应用（Ours）。bool=true 只代表登记面回读归本，
// ≠ 双击行为生效（见头注释 F-07 判定规则）；写入/回查失败如实 fail。
core::Result<bool> setAsDefaultFor(const AssocConfig& config = productionConfig());

// 撤销全部已注册关联（F-18 新语义「关闭=撤销」，老板 2026-11-17 裁，取代旧
// 「生产永不注销」D-4）：逐扩展仅当回读归己（新 ProgId 或旧 kLegacyMdProgId）
// 才清该扩展登记——不误删用户后选的他者；清完发 SHChangeNotify（有清除时）。
// 幂等：已注销/未登记态均 ok。macOS 无注销公开 API = 如实 NotImplemented。
core::Result<void> unregisterAll(const AssocConfig& config = productionConfig());

// 拉起系统关联设置 UI（F-18 未生效态引导，Windows 10+ = ms-settings:defaultapps，
// api_map 映射锚见 file_association_api_map.h）：路径唯一权威，调用点 = 设置页
// 引导按钮。macOS 无按文件类型默认页直达 = NotImplemented（文字指引兜底）。
core::Result<void> launchAdvancedAssociationUI(const std::string& extension = {});

// ── 注入缝（单测专用；生产代码不触碰）──

// SHChangeNotify spy 缝：生产缺省 = 真调用（win SHChangeNotify / mac
// CFPreferencesAppSynchronize）；单测注入 spy 计数断言「写完必调」。返回旧 sink
//（用例负责恢复，防跨用例泄漏）。
using ShellNotifySink = void();
ShellNotifySink* setShellNotifySink(ShellNotifySink* sink);

// 测试清理缝：删登记键——<progId> 整树 + 逐扩展默认值（扩展名键仅当清空后才
// 整键删，不毁他者既有内容）。仅注入缝用例「写→断言→清理」使用；已注销态幂等 ok。
core::Result<void> removeRegistrationForTest(const AssocConfig& config);

} // namespace platform::file_association
