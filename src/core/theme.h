// 设计 token（温暖专业配色）
#pragma once

#include <eui/types.h>

#include <functional>
#include <string>

namespace core::theme {

// 主题深浅切换回调：core 层不可调 EUI-NEO 运行时行为，标题栏联动由 main.cpp 启动时注入；
// setDark/setLight 每次切换都通知（幂等重放也通知）
using ChangeHandler = std::function<void(bool isDark)>;
void setChangeHandler(ChangeHandler handler);

// 颜色 token（Dark Mode）
struct ColorTokens {
    eui::Color background;
    eui::Color surface;
    eui::Color surfaceElevated;
    eui::Color hover;
    eui::Color active;
    eui::Color primary;
    eui::Color primaryHover;
    eui::Color primaryPressed;
    eui::Color primarySoftBg;
    // 主色作文字色的专用 token：#D97757 直接当 11-13px 小字在浅底上仅 2.96:1，低于 WCAG AA 4.5:1；
    // 深色档 = primary，浅色档 = 深一档 clay。链接/chip 文字用本 token；图标/边框/背景继续用 primary
    eui::Color primaryText;
    // 主色背景上的前景色（主按钮文字/ACTIVE 徽标；两主题均为白）
    eui::Color onPrimary;
    eui::Color textPrimary;
    eui::Color textSecondary;
    eui::Color textTertiary;
    eui::Color textDisabled;
    eui::Color borderDefault;
    eui::Color borderMuted;
    eui::Color success;
    eui::Color warning;
    eui::Color error;
    eui::Color info;
    // 与现有功能色同族的去饱和强调色（区别于 blue 的品牌色档位，浅色档达图形 AA）
    eui::Color purple;
    eui::Color pink;
    // 腾讯 QQ 经典蓝（用户点名），识别度优先于家族去饱和纪律；
    // 对浅色底约 2.1:1（低于图形 AA 3.0），与 primary（2.96:1）同属品牌色档位——
    // 只作图标/标签/边框/装饰强调，禁止作小字正文色。
    // 同 primaryText 模式：将来需要文字用法另设 blueText 档，勿直接拿本值当文字色。
    eui::Color blue;

    // —— 微信风格配色（D-12 整改新增；独立于既有暖色系，新旧并存，页面逐工单切换）——
    // 旧槽位（primary=橙 #D97757 等）完全不动；微信样式经 componentTokensWeChat() 显式传值。
    struct WeChatColors {
        eui::Color primary;            // #07C160 主色（按钮底/选中/高亮）
        eui::Color primaryHover;       // #06AE56 主色 hover（spec 未指定则用此派生值）
        eui::Color primaryPressed;     // #059A4C 主色 pressed（派生值）
        eui::Color onPrimary;          // #FFFFFF 主色上的前景（按钮文字）
        eui::Color warningRed;         // #FA5151 警示/危险/逾期
        eui::Color linkBlue;           // #576B95 链接/可复制文本
        eui::Color pageBg;             // #F7F7F7 页面底
        eui::Color cardBg;             // #FFFFFF 卡片底
        eui::Color dividerPrimary;     // #E5E5E5 一级分割线
        eui::Color dividerSecondary;   // #EDEDED 二级分割线 / hover
        eui::Color textPrimary;        // #1A1A1A 一级文字
        eui::Color textSecondary;     // #666666 二级文字
        eui::Color textTertiary;      // #999999 三级文字
        eui::Color selectedBg;        // #E5E5E5 选中态底
        eui::Color secondaryButtonBg; // #F2F2F2 次按钮底
        eui::Color sidebarBgTinted;   // #E8F0EC 侧栏微绿灰（D-12 E1）
    } wechat;
};

// 尺寸 token（唯一权威定义）
struct SizeTokens {
    float spacing;             // 8.0f
    float spacingSmall;        // 4.0f
    float spacingLarge;        // 16.0f
    float spacingXLarge;       // 24.0f
    float spacingXXLarge;      // 40.0f
    float radius;              // 10.0f
    float radiusSmall;         // 6.0f
    float radiusLarge;         // 14.0f
    float radiusPill;          // 999.0f
    float fontSizeSmall;       // 13.0f
    float fontSizeBody;        // 15.0f
    float fontSizeLarge;       // 18.0f
    float fontSizeTitle;       // 24.0f
    float fontSizePage;        // 32.0f
    float fontSizeDisplay;     // 48.0f
    float spacingPage;            // 12.0f 页面外 padding（D-12 A3：16→12）
    float radiusWeChat;           // 6.0f 微信风格统一圆角（D-12 E2：v1.0 三档 6/8/4→统一 6）
    float fontSizeWeChat;         // 16.0f 微信风格正文字号（D-12 §2.4：16/18 档，18 复用既有 fontSizeLarge）
};

struct Tokens {
    ColorTokens colors;
    SizeTokens sizes;
    bool isDark;
};

// 当前主题（默认深色）
const Tokens& current();

void setDark();
void setLight();

// 全局字号缩放（1.0 = 原始尺寸）：作用于 SizeTokens 全部 fontSize* 字段；硬编码字面量不在缩放范围
void setFontScale(float scale);
float fontScale();

// Markdown 编辑器字号（px）：「core 存值、app/settings 写、消费方读」全局通道
//（markdown 不可 include module_settings）。默认 15.0f，set 钳制 [10, 32]
void setEditorFontSize(float px);
float editorFontSize();

std::string currentThemeName();

enum class Accent { Clay, Purple, Pink, Blue };
void setAccent(Accent a);
Accent accent();

} // namespace core::theme