# 网页终端（need#490）源清单。主 CMakeLists 只 include + target_sources；
# 新增 web/*.cpp 改这里，不改 module_terminal/CMakeLists.txt。
set(DEVDESK_WEB_TERMINAL_SOURCES
    web/web_auth.cpp
    web/web_bridge.cpp
    web/web_crypto_session.cpp
    web/web_dev_entry.cpp
    web/web_routes.cpp
    web/web_terminal_service.cpp
    # W4：网页网格帧编解码 + 会话桥（纯逻辑，无 HTTP 依赖）
    web/web_grid.cpp
    web/web_session_bridge.cpp
)
