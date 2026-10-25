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
    // Ghost/Text 级按钮（透明底 + 主题色字）的专用字色：primary 浅色 #36D399 on #FAFAF9
    // 仅 1.84:1 禁作字色，故与作图标/边框用的 primary 分槽；两主题均须对
    // background/primarySoftBg ≥ WCAG AA 4.5:1
    eui::Color accentText;
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
    // error 作文字色的专用 token：浅色 #EF4444 压 10% 红粉底（校验失败态/错误横幅）
    // 仅 3.18:1，低于 WCAG AA 4.5:1。浅色档 = 深红；深色档 = error 同值。
    // 错误文字用本 token；红底/边框/圆点/图标继续用 error
    eui::Color errorText;
    eui::Color info;
    eui::Color sidebarBg;       // #F0FDF7 侧栏淡绿底
    // 与现有功能色同族的去饱和强调色（区别于 blue 的品牌色档位，浅色档达图形 AA）
    eui::Color purple;
    eui::Color pink;
    // 腾讯 QQ 经典蓝（用户点名），识别度优先于家族去饱和纪律；
    // 对浅色底约 2.1:1（低于图形 AA 3.0），与 primary（2.96:1）同属品牌色档位——
    // 只作图标/标签/边框/装饰强调，禁止作小字正文色。
    // 同 primaryText 模式：将来需要文字用法另设 blueText 档，勿直接拿本值当文字色。
    eui::Color blue;
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
    float radiusLarge;         // 12.0f
    float radiusOverlay;       // 16.0f 首页大卡片圆角（S1 圆角 16px 档）
    float radiusPill;          // 999.0f
    float fontSizeSmall;       // 12.0f
    float fontSizeBody;        // 15.0f
    float fontSizeLarge;       // 18.0f
    float fontSizeTitle;       // 24.0f
    float fontSizePage;        // 24.0f
    float fontSizeDisplay;     // 48.0f
    float fontSizeStat;          // 28.0f  首页数据数字（S1.2 字号迁移）
    float spacingPage;            // 12.0f 页面外 padding（D-12 A3：16→12）
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

// 窗高驱动字号封顶已废止（2026-10-25 用户拍板，可读性优先）：windowScaleCap 恒不封顶，
// fontScale() = 用户手动档；setWindowHeight 保留信号通道（app::compose 每帧喂入）。
// 如需恢复 revert theme.cpp windowScaleCap
void setWindowHeight(float logicalHeight);

// v1.0 字体栈（S1.2 字体系统 / S5.2 平台回退）
// Inter 为基准无衬线，Windows 回退 YaHei，macOS 回退 PingFang SC
inline const char* fontStack() {
    return "Inter, \"Microsoft YaHei\", \"PingFang SC\", sans-serif";
}

// Markdown 编辑器字号（px）：「core 存值、app/settings 写、消费方读」全局通道
//（markdown 不可 include module_settings）。默认 15.0f，set 钳制 [10, 32]
void setEditorFontSize(float px);
float editorFontSize();

std::string currentThemeName();

enum class Accent { Clay, Purple, Pink, Blue };
void setAccent(Accent a);
Accent accent();

} // namespace core::theme