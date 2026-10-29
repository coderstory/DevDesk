// src/toast_notice_bridge.h
// app 层 toast 通知桥：函数指针缝 + 默认桩 + static_assert 签名哨兵（模式同
// memory_mcp_bridge）。module_schedule 的 toast 渠道只经此桥投递应用级轻提示，
// 不 include app_sidebar.h；实现 TU 只依赖标准库，tests / probe / live 编译
// 它时不产生模块边。真实现由 main.cpp 装配段注入（转发 app::showAppNotice）。
#pragma once

#include <string>
#include <type_traits>

namespace app::toast_notice {

using ShowHandler = void (*)(const std::string& title, const std::string& body);

// 传 nullptr = 恢复默认桩（未装配 = 投递静默丢弃，零 UI 副作用）。
void setHandler(ShowHandler show);

void show(const std::string& title, const std::string& body);

}  // namespace app::toast_notice

static_assert(std::is_same_v<decltype(&app::toast_notice::show),
                             app::toast_notice::ShowHandler>,
              "toast_notice_bridge: show 签名漂移");
