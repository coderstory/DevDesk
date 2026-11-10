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

} // namespace platform::file_association::mac_api
