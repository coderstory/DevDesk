// src/module_settings/settings_ui_cards.h
// R6（spec 2026-09-27-refactor-incremental-design §5.2）：settings 模块 8 张卡的
// 绘制单元 —— 逐字自 settings_module.cpp 搬出（原 compose 内的 9 个私有
// draw*Card；2026-09-28 about 页设计删去 drawAboutCard，关于信息迁往独立
// module_about）。全部是自由函数：吃只读入参（SettingsCardsIn）与回调
//（SettingsCardActions），不捕获 this、不写模块成员。
//
// 回调约定（R4 同款，spec §4.2.3）：卡片在元素回调里**按值捕获**用到的
// std::function（`[f = a.xxx]`），不捕获本文件内的临时对象——元素树比一帧活得
// 久，捕引用会悬垂。需要写回模块状态的一律经 SettingsCardActions。
//
// 零行为变化：节点 id、文案、字号、颜色、几何、控制流逐字保留。
#pragma once

#include "settings_config.h"
#include "settings_ui_logic.h"

#include <core/claude_proxy_service.h>
#include <core/theme.h>
#include <eui/dsl.h>

#include <functional>
#include <string>
#include <vector>

namespace module::settings {

// 卡片的只读入参（compose 每帧装配；引用指向模块的存活成员，只在绘制期读）
struct SettingsCardsIn {
    const SettingsConfig& cfg;                 // 配置（模块成员 cfg_）
    const std::vector<std::string>& petSkins;  // 皮肤清单快照（THS-01）
    const std::string& petSkinName;            // 当前皮肤名快照（THS-01）
    const std::string& proxyPortDraft;         // 端口草稿（空 = 未编辑）
    const std::vector<std::string>& logTail;   // 日志 tail 快照（H2）
    unsigned logTailVersion = 0;               // tail 版本号（进查看卡 contentKey）
    float logViewScrollOffset = 0.0f;          // 日志查看卡内滚动位置
    bool takeoverManaged = false;              // 接管状态快照（THS-01）
    bool notesDirExists = false;               // 笔记目录存在性快照（THS-01）
    int openDropdown = -1;                     // 下拉唯一持有者（THS-05）
    ::core::IClaudeProxyService* proxy = nullptr;  // 代理运行面（null = 未装配）
};

// 卡片回写模块的回调（compose 每帧装配；lambda 只捕获 this → SBO 无堆分配）
struct SettingsCardActions {
    std::function<void(int)> applyThemeMode;                 // 主题模式三档
    std::function<void(int)> applyFontPreset;                // 字号缩放档
    std::function<void(int)> applyWindowEffect;              // 窗口效果三档
    std::function<void(const std::string&, bool)> applyModuleEnabled;  // 模块插拔
    std::function<void(Dropdown, bool)> setDropdownOpen;      // 下拉唯一持有者（THS-05）
    std::function<void(int)> changePetSkinAt;                 // 桌宠换肤（按下标）
    std::function<void()> resetPetPosition;                   // 桌宠位置重置
    std::function<void()> showPetWindow;                      // 显示桌宠（PAB-13）
    std::function<void()> openNotesDirectory;                 // 打开笔记目录
    std::function<void(bool)> applyProxyAutoStart;            // 随应用启动代理
    std::function<void()> applyProxyPort;                     // 应用代理端口
    std::function<void(const std::string&)> setProxyPortDraft;  // 端口输入（含清错误态）
    std::function<void()> discardProxyPortDraft;              // 端口草稿 Esc 丢弃
    std::function<void(int)> applyRetentionPreset;            // 日志保留档
    std::function<void(int)> applyLogLevelPreset;             // 日志级别档
    std::function<void(int)> applyEditorFontPreset;           // 编辑器字号档
    std::function<void()> requestLogTailReload;               // 日志查看卡刷新
    std::function<void(float)> logViewScrolled;               // 日志查看卡内滚动
};

// 单卡绘制入参（注册表按序调用；9 张卡签名统一，未用到的字段忽略）
struct CardDrawArgs {
    eui::Ui& ui;
    std::string id;               // 卡根 id（注册表给的字面量）
    float w = 0.0f;               // 卡宽（= scrollView 内容宽）
    float h = 0.0f;               // 卡高（注册表按卡高种类解析）
    const core::theme::Tokens& t;
    float pageY = 0.0f;           // 卡顶页面绝对 y（仅 openUp 几何卡读）
    float screenH = 0.0f;         // 页面逻辑高（同上）
    const SettingsCardsIn& in;
    const SettingsCardActions& a;
};

// ---- 8 张卡（签名统一，注册表按 tab 分派；绘制顺序 = 注册表顺序）----
void drawAppearanceCard(const CardDrawArgs& args);   // 通用：外观（主题/字号/窗口效果）
void drawEditorFontCard(const CardDrawArgs& args);   // 通用：编辑器字号
void drawModulesCard(const CardDrawArgs& args);      // 模块：插拔开关
void drawPetCard(const CardDrawArgs& args);          // 模块：桌宠（开关门控）
void drawDirectoryCard(const CardDrawArgs& args);    // 模块：笔记目录（开关门控）
void drawProxyCard(const CardDrawArgs& args);        // 模块：Claude 代理（开关门控）
void drawLogCard(const CardDrawArgs& args);          // 日志：保留天数 + 级别
void drawLogViewCard(const CardDrawArgs& args);      // 日志：tail 查看

} // namespace module::settings
