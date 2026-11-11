// tests/test_platform/test_file_association_api_map.cpp
// F-09 mac 设默认「版本→API 路径」映射纯逻辑（win 可编译运行；ObjC 调用形态
// 本机不可编译验证，真机接管定性 = UAT）。回退会红：改 api_map 分界常量或
// 分支方向，本用例即红。
#include "doctest.h"
#include "platform/file_association_api_map.h"

REGISTER_TEST(file_association_mac_api_map,
              "fileassoc: mac 版本→写入 API 路径映射（11=LS role handler / 12+=NSWorkspace，分界钉死）", {
    using namespace platform::file_association::mac_api;
    // 应用下限 11.0：11 = LS 旧 API（预置 A）；查询无分支（卡「同一结果语义」）
    DOCTEST_CHECK_EQ(writeApiFor(11), WriteApi::LsSetRoleHandler);
    DOCTEST_CHECK_EQ(writeApiFor(12), WriteApi::NsWorkspaceSetDefault);
    DOCTEST_CHECK_EQ(writeApiFor(13), WriteApi::NsWorkspaceSetDefault);
    DOCTEST_CHECK_EQ(writeApiFor(15), WriteApi::NsWorkspaceSetDefault);
    // 版本解析失败回退（macOSMajorVersion()=0）→ LS 保守路径
    DOCTEST_CHECK_EQ(writeApiFor(0), WriteApi::LsSetRoleHandler);
    DOCTEST_CHECK_EQ(writeApiFor(10), WriteApi::LsSetRoleHandler);
    // D-6 预置 A 分界钉死（若 UAT 回填改判，此处与实现同步改）
    DOCTEST_CHECK_EQ(kWorkspaceSetDefaultSinceMajor, 12);
    return true;
})
