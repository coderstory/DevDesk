// src/module_settings/notepad_assoc_card.h
// 「记事本文件关联」开关行卡（设置页「模块」tab，F-18，need263 批 4 棒 B；
// 接管退役的「关联 .md 文件」卡 md_assoc_card.*，UI 结构照抄其成熟样式：
// 开关 + 状态字四态 + 引导带分平台）。
// 状态字四态推导与卡高契约单一来源 = notepad_assoc_logic.h；引导行（未生效态）
// 分平台：Windows = 一键按钮直达系统默认应用设置（launchAdvancedAssociationUI）
// / macOS = 文字指引——分流经 platform::open_settings::oneClickAvailable()，
// 业务代码零平台宏。
#pragma once

#include "settings_ui_cards.h"

namespace module::settings {

// 注册表入口（平台分流：Win 一键按钮 / mac 文字指引）。
void drawNotepadAssocCard(const CardDrawArgs& args);

// 测试缝（承接 md 卡 P2-1 缝：mac 文字指引分支在 Windows 不可达——
// oneClickAvailable() 恒 true——本机探针/截图须强制文字档验证盒高派生；
// 生产零消费，注册表持有的函数指针仍绑单参重载，签名不变）。
void drawNotepadAssocCard(const CardDrawArgs& args, bool forceTextGuide);

// mac 引导文案实测行数（卡高派生输入，承接 md 卡 review P2-1）：11px 档量整串
// 实宽 ÷ 引导文案可用宽向上取整，下限 2（设计 §2.3 两行格律基线）。卡宽传配对
// 列宽（与 drawNotepadAssocCard 的 args.w 同源）；设置模块 compose 算卡高与本卡
// 绘制共用本函数，保证盒高与卡高契约同源不漂。文案随未生效摘要运行期变化 →
// 以调用方组装好的全文为参（notepadAssocMacGuideText 组装，见 logic.h）。
int notepadAssocMacGuideLineCount(float cardW, const std::string& guideText);

} // namespace module::settings
