// src/ui_kit/logo.h
// DevDesk 品牌几何 logo（2026-10-20 C 方案「终端窗 >_」）的共享矢量绘制 helper。
// 归属 ui_kit：需 EUI-NEO dsl 原语（stack/rect/polygon），三处消费方（侧边栏 brand 行 /
// 自绘标题栏 / 启动 splash）共用；core / platform 不得反向依赖（本层铁律）。
//
// 与 assets/icon.ico 的关系：同一组归一化设计坐标（240 单位瓦面），bitmap 版由
// scripts/build_icon_assets.py 生成（几何契约详见其 docstring）；改设计时矢量/位图
// 两处同步动。颜色由调用方传 theme 令牌（禁裸 hex，D-03/§8 #11）：侧边栏/标题栏用
// wechat.primary + cardBg，splash 用 primaryPressed（AA 对比档）+ cardBg。
//
// 圆帽斜线说明：EUI-NEO dsl 无独立「stroke line」原语，本 helper 复用 lineChart 的
// 成熟胶囊拼法（components/linechart.h drawCapsuleSegment 同源）：线段 = 18 点
// polygon（两端各 8 段圆弧补圆帽），渲染路径已被 lineChart 长期验证，无需上游新原语。
#pragma once

#include <eui/dsl.h>
#include <eui/types.h>

#include <string>

namespace ui_kit {

// 绘制 DevDesk 品牌 logo：绿圆角瓦 + 白圆角窗 + 绿色 >_ 字形（两条圆帽斜线段 + 短横）。
//
// 几何：(x, y) = 瓦面左上角（logo 的可见包围盒），size = 瓦边长；窗口与 >_ 按设计
// 比例从 size 归一化缩放。颜色：tileColor 染瓦与 >_ 笔画（设计同色），windowColor
// 染白窗。opacity 供 splash 整体淡出复用（默认 1.0）。
//
// 元素契约（单测按此定位）：根 = stack(id)（box 恰为瓦面），子元素
//   id + ".tile"（Rect，圆角瓦）/ ".win"（Rect，白窗）/
//   ".gt1" ".gt2"（Polygon，> 两段）/ ".us"（Polygon，_ 短横）。
void drawDevDeskLogo(eui::Ui& ui, const std::string& id,
                     float x, float y, float size,
                     const eui::Color& tileColor, const eui::Color& windowColor,
                     float opacity = 1.0f);

} // namespace ui_kit
