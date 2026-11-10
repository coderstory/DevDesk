// src/platform/file_association.h
// 文件关联平台抽象（F-06 .md 只读查看器登记）：头平台无关——配置注入缝 + 三态
// 纯逻辑 inline 在头里，实现按 *_win/_macos 拆（与 autostart / file_open 同一模式，
// 业务代码零平台宏）。macOS 侧实现归 F-09（U9），本头先行冻结接口。
// 语义基准 = 方案卡 §二.F-06 + F-07 判定规则
//（docs/research/2026-11-10-f07-win-userchoice-poc.md，clue10）：
//   · 「已生效」唯一判据 = 查询回读（QueryCurrentDefault）归本应用；
//     写入成功 ≠ 双击行为生效（Win10 22H2 实测：注册表写入层生效、行为面不接管）。
//   · Unknown = 查询失败 ≠ Others（按需修复档：触发一次重写 + 未生效呈现，
//     不与 Others 混判）。
#pragma once

#include <string>

#include "core/result.h"

namespace platform::file_association {

// 三态归属：Ours=查询回读 ProgId==本应用；Others=回读他者 ProgId；
// Unknown=查询失败（≠Others，语义见头注释）
enum class AssocState { Ours, Others, Unknown };

// 登记参数（注入缝）：ProgId 名与目标扩展名可替换。生产 = DevDesk.md + .md；
// 单测必须用 testConfig()（DevDeskTest.md + .mdtest）——用例禁触开发机真实 .md
// 登记（测试红线：防用例劫持真实用户级关联）。
struct AssocConfig {
    std::string progId;     // 判属锚：回读比对 ProgId 串而非 exe 路径（exe 挪位不影响判属）
    std::string extension;  // 含点扩展名
};

inline AssocConfig productionConfig() { return {"DevDesk.md", ".md"}; }
inline AssocConfig testConfig() { return {"DevDeskTest.md", ".mdtest"}; }

// ProgId (Default) 描述串（登记内容组成部分，单测比对锚）
inline constexpr const char* kProgIdDescription = "DevDesk Markdown Document";

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

// ── 系统触达（Windows 实现在 file_association_win.cpp；macOS 归 F-09）──

// 查询 .md 生效归属：QueryCurrentDefault(<ext>, AT_FILEEXTENSION, AL_EFFECTIVE)
// 回读 ProgId 串 → 三态。查询失败一律落 Unknown（Result 仍 ok——「查询通路失败」
// 是 Unknown 态的成因之一，不是独立错误面）。非 Windows 平台 = NotImplemented。
core::Result<AssocState> isDefaultForMd(
    const AssocConfig& config = productionConfig());

// 恢复登记：HKCU\Software\Classes\<progId>{(Default)=kProgIdDescription,
// DefaultIcon, shell\open\command="<自身exe>" "%1"} + HKCU\Software\Classes\<ext>
// 默认值指向 <progId>；写完必调 SHChangeNotify(SHCNE_ASSOCCHANGED)（官方
// must-call，刷 Shell 图标/关联缓存）。全程 HKCU 用户级，免管理员。
// 幂等：重复执行写同一目标值，无累积无残留。
// 返回 bool = 写入后回查归本应用（Ours）。注意：bool=true 只代表登记面回读归本，
// ≠ 双击行为生效（Win10 22H2 行为面不接管，依据见头注释 POC 报告）；写入/回查
// 失败如实 fail。非 Windows 平台 = NotImplemented。
core::Result<bool> setAsDefaultForMd(
    const AssocConfig& config = productionConfig());

// ── 注入缝（单测专用；生产代码不触碰）──

// SHChangeNotify spy 缝：生产缺省 = 真调用 SHChangeNotify(SHCNE_ASSOCCHANGED,
// SHCNF_IDLIST, NULL, NULL)；单测注入 spy 计数断言「写完必调」。返回旧 sink
//（用例负责恢复，防跨用例泄漏）。
using ShellNotifySink = void();
ShellNotifySink* setShellNotifySink(ShellNotifySink* sink);

// 测试清理缝：删登记键——<progId> 整树 + <ext> 默认值（扩展名键仅当清空后才
// 整键删，不毁他者既有内容）。生产永不注销（PRD D-4），仅注入缝用例
//「写→断言→清理」使用；已注销态幂等 ok。
core::Result<void> removeRegistrationForTest(const AssocConfig& config);

} // namespace platform::file_association
