// src/toast_notice_bridge.cpp
// toast 通知桥实现缝。独立 TU：只依赖标准库，不链接 app 层——devdesk_tests /
// probe / live 目标编译本文件时不产生模块边；真实现由 main.cpp 装配段注入
//（转发 app::showAppNotice 应用级单槽 toast）。
#include "toast_notice_bridge.h"

namespace app::toast_notice {

namespace {

ShowHandler g_show = nullptr;

}  // namespace

void setHandler(ShowHandler show) { g_show = show; }

void show(const std::string& title, const std::string& body) {
    if (g_show == nullptr) {
        return;  // 默认桩：未装配（无头测试 / 未接线宿主）= 投递丢弃，零 UI 副作用
    }
    g_show(title, body);
}

}  // namespace app::toast_notice
