// tests/test_platform/test_terminal_vterm_mouse.cpp
// platform::VtermCore 鼠标上报（DECSET 1000/1002/1003 跟踪 + libvterm SGR 编码）。
// 用例面：点击/拖动/自由移动/滚轮/关闭模式/多参合并形/跨 chunk/备屏 getter。
// 字节锁值 = libvterm mouse.c 编码（SGR）= `ESC [ < code ; col+1 ; row+1 M|m`，
// 坐标 0 基进、1 基出（列在前）；motion 码 32+ 为拖动、35 为无键移动、64/65 为滚轮。
#include "doctest.h"

#include <cstdint>
#include <string>

#include "platform/terminal/terminal_vterm_core.h"

namespace {

void feed(platform::VtermCore& core, const std::string& s) {
    core.feed(reinterpret_cast<const uint8_t*>(s.data()), int32_t(s.size()), nullptr);
}

bool has(const std::string& reply, const char* needle) {
    return reply.find(needle) != std::string::npos;
}

} // namespace

REGISTER_TEST(vtermMouseClickSgr, "vterm core: 1000 点击 → SGR 上报（列在前，0 基进 1 基出）", {
    platform::VtermCore core(10, 20);
    DOCTEST_CHECK_FALSE(core.mouseReporting());
    feed(core, "\x1b[?1000h\x1b[?1006h");
    DOCTEST_CHECK(core.mouseReporting());

    std::string reply;
    core.mouseButton(3, 5, 0, true, &reply);  // 左上键按下 @ row=3 col=5（0 基）
    DOCTEST_CHECK(has(reply, "\x1b[<0;6;4M"));

    reply.clear();
    core.mouseButton(3, 5, 0, false, &reply);  // 松开：同码 + 'm'
    DOCTEST_CHECK(has(reply, "\x1b[<0;6;4m"));
    return true;
})

REGISTER_TEST(vtermMouseSilentWithoutMode, "vterm core: 未开鼠标模式零产出（含 4 参页面调用面）", {
    platform::VtermCore core(10, 20);
    std::string reply;
    core.mouseButton(1, 1, 0, true, &reply);
    DOCTEST_CHECK(reply.empty());
    core.mouseMove(2, 2, true, &reply);
    DOCTEST_CHECK(reply.empty());
    // 页面契约面（4 参、默认丢弃）：不得崩溃、不得产出可观测状态
    core.mouseButton(0, 0, 0, true);
    core.mouseMove(0, 0, false);
    DOCTEST_CHECK_FALSE(core.mouseReporting());
    return true;
})

REGISTER_TEST(vtermMouseDrag1002, "vterm core: 1002 按住拖动 → 32+x 报告，无键移动不报", {
    platform::VtermCore core(10, 20);
    feed(core, "\x1b[?1002h\x1b[?1006h");
    DOCTEST_CHECK(core.mouseReporting());

    std::string reply;
    core.mouseButton(3, 5, 0, true, &reply);  // 按下
    DOCTEST_CHECK(has(reply, "\x1b[<0;6;4M"));
    reply.clear();
    core.mouseMove(4, 6, true, &reply);  // 按住拖到 row=4 col=6
    DOCTEST_CHECK(has(reply, "\x1b[<32;7;5M"));
    reply.clear();
    core.mouseButton(4, 6, 0, false, &reply);  // 松开
    DOCTEST_CHECK(has(reply, "\x1b[<0;7;5m"));
    reply.clear();
    core.mouseMove(5, 7, false, &reply);  // 1002 只报拖动，无键移动零产出
    DOCTEST_CHECK(reply.empty());
    return true;
})

REGISTER_TEST(vtermMouseMove1003, "vterm core: 1003 自由移动 → 35 报告", {
    platform::VtermCore core(10, 20);
    feed(core, "\x1b[?1003h\x1b[?1006h");
    DOCTEST_CHECK(core.mouseReporting());
    std::string reply;
    core.mouseMove(1, 2, false, &reply);
    DOCTEST_CHECK(has(reply, "\x1b[<35;3;2M"));
    return true;
})

REGISTER_TEST(vtermMouseWheel, "vterm core: 滚轮 4/5 → SGR 64/65", {
    platform::VtermCore core(10, 20);
    feed(core, "\x1b[?1000h\x1b[?1006h");
    std::string reply;
    core.mouseButton(3, 5, 4, true, &reply);  // 上滚按下
    DOCTEST_CHECK(has(reply, "\x1b[<64;6;4M"));
    reply.clear();
    core.mouseButton(3, 5, 4, false, &reply);  // 上滚松开
    DOCTEST_CHECK(has(reply, "\x1b[<64;6;4m"));
    reply.clear();
    core.mouseButton(3, 5, 5, true, &reply);  // 下滚按下
    DOCTEST_CHECK(has(reply, "\x1b[<65;6;4M"));
    return true;
})

REGISTER_TEST(vtermMouseModeTracking,
             "vterm core: 1000l 关闭 + 多参合并形 + 跨 chunk + 备屏 getter", {
    platform::VtermCore core(10, 20);
    feed(core, "\x1b[?1000h");
    DOCTEST_CHECK(core.mouseReporting());
    feed(core, "\x1b[?1000l");
    DOCTEST_CHECK_FALSE(core.mouseReporting());
    std::string reply;
    core.mouseButton(0, 0, 0, true, &reply);
    DOCTEST_CHECK(reply.empty());

    // 多参合并形（部分 TUI 如此发送）：`?1000;1006h` 只认 1000 一项
    feed(core, "\x1b[?1000;1006h");
    DOCTEST_CHECK(core.mouseReporting());
    feed(core, "\x1b[?1000;1006l");
    DOCTEST_CHECK_FALSE(core.mouseReporting());

    // 终结符跨 chunk：候选残留在 tail 里等下一批
    feed(core, "\x1b[?10");
    feed(core, "03h");
    DOCTEST_CHECK(core.mouseReporting());

    // 备屏 getter（VTERM_PROP_ALTSCREEN 镜像）
    DOCTEST_CHECK_FALSE(core.altscreen());
    feed(core, "\x1b[?1049h");
    DOCTEST_CHECK(core.altscreen());
    feed(core, "\x1b[?1049l");
    DOCTEST_CHECK_FALSE(core.altscreen());
    return true;
})
