// src/module_settings/md_assoc_card.h
// 「关联 .md 文件」开关行卡（设置页「通用」tab，F-10）。
// 状态字四态推导与卡高契约单一来源 = md_assoc_logic.h；引导行（未生效态）
// 分平台：Windows = 一键按钮直达系统默认应用设置 / macOS = 文字指引——
// 分流经 platform::open_settings::oneClickAvailable()，业务代码零平台宏。
#pragma once

#include "settings_ui_cards.h"

namespace module::settings {

// 注册表入口（平台分流：Win 一键按钮 / mac 文字指引）。
void drawMdAssocCard(const CardDrawArgs& args);

// 测试缝（review P2-1：mac 文字指引分支在 Windows 不可达——oneClickAvailable()
// 恒 true——本机探针/截图须强制文字档验证盒高派生；生产零消费，注册表持有的
// 函数指针仍绑单参重载，签名不变）。
void drawMdAssocCard(const CardDrawArgs& args, bool forceTextGuide);

// mac 引导文案实测行数（卡高派生输入，review P2-1）：11px 档量整串实宽 ÷
// 引导文案可用宽向上取整，下限 2（设计 §2.3 两行格律基线）。卡宽传配对列宽
// （与 drawMdAssocCard 的 args.w 同源）；设置模块 compose 算卡高与本卡绘制
// 共用本函数，保证盒高与卡高契约同源不漂。用卡宽而非可用宽作参：可用宽的
// 推导（左轨/图标带）是本文件私有几何。
int mdAssocMacGuideLineCount(float cardW);

} // namespace module::settings
