// src/platform/file_association_api_map.h
// F-09 macOS 设默认「版本→API 路径」映射纯逻辑（方案卡 §二.F-09，预置 A=D-6）：
// 平台无关小头——win 可编译可测（本单元唯一本机验证面），唯一消费者 =
// file_association_macos.mm。版本源 = mac 侧运行期 kern.osproductversion
// 主版本号（本头不含，保持零平台依赖）。
#pragma once

namespace platform::file_association::mac_api {

// macOS 12.0 = LSSetDefaultRoleHandlerForContentType 弃用、NSWorkspace
// setDefaultApplicationAtURL:toOpenURL: 引入（应用下限 11.0 → 双路径分界，
// clue5；真机 11–15 逐版本定性 = UAT 项，分界先按预置 A 落）
inline constexpr int kWorkspaceSetDefaultSinceMajor = 12;

// 写入路径二选一：LS role handler（11）/ NSWorkspace 设默认（12+）
enum class WriteApi { LsSetRoleHandler, NsWorkspaceSetDefault };

// 查询无强制分支：卡 F-09「12+ 新 API 均可（同一结果语义）」——统一走
// LSCopyDefaultApplicationURLForURL（10.10+，12 上不弃用，与 NSWorkspace
// URLForApplicationToOpenURL 同结果语义），映射表因此只承载写入路径。
inline WriteApi writeApiFor(int majorVersion) {
    return majorVersion >= kWorkspaceSetDefaultSinceMajor
               ? WriteApi::NsWorkspaceSetDefault
               : WriteApi::LsSetRoleHandler;
}

// F-18 拉系统关联设置 UI 的路径映射锚（launchAdvancedAssociationUI 唯一权威）：
// Windows 10+ = ms-settings:defaultapps（系统设置「默认应用」页，ShellExecuteW
// URI；Win8+ 禁静默抢默认后官方正路，clue #248）。macOS 无按文件类型默认页
// 程序化直达（PRD D-5）→ 该平台如实 NotImplemented + 设置页文字指引兜底。
inline constexpr const char* kAdvancedAssociationUiUri = "ms-settings:defaultapps";

} // namespace platform::file_association::mac_api
