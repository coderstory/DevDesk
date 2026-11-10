// tests/test_platform/test_single_instance.cpp
// 单实例单测：互斥量语义 + F-01 路径送达协议（编解码 / ack 专名配对 / 整包校验 /
// 发送互斥串行化 / 进程内全链路）。跨进程双开行为留给主会话真机验收——本文件仅覆盖
// 可单测的部分。
//
// acquireInstanceLock() 有进程级副作用（Windows：启动监听线程 + 持有命名 mutex），
// 仅 si_chain_full_inprocess 用例经「测试根名」调用（隔离开发机真实实例通道，先
// setSingleInstanceRootForTest 再 acquire）；其余用例全部绕开进程级单实例通道。
// macOS / POSIX 实现恒 true，无独立可测逻辑（编解码两条平台无关用例除外）。
#include "doctest.h"

#include "app_assembly.h"
#include "platform/single_instance.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

REGISTER_TEST(si_payload_codec_roundtrip,
              "single_instance: 载荷编解码往返（0/1/多路径/空串/中文空格/任意字节/上界）", {
    using platform::single_instance::PathPayload;
    using platform::single_instance::decodePathPayload;
    using platform::single_instance::encodePathPayload;
    using platform::single_instance::kMaxPayloadBytes;

    // 0 路径 + 空 ack 名（最小编码）
    {
        PathPayload out;
        DOCTEST_CHECK(decodePathPayload(encodePathPayload(PathPayload{"", {}}), out));
        DOCTEST_CHECK(out.ackName.empty());
        DOCTEST_CHECK(out.paths.empty());
    }
    // 单路径：中文 + 空格
    {
        const PathPayload in{"Local\\DevDesk.SingleInstance.Ack.1",
                             {"D:\\目录 带空格\\笔记.md"}};
        PathPayload out;
        DOCTEST_CHECK(decodePathPayload(encodePathPayload(in), out));
        DOCTEST_CHECK_EQ(out.ackName, in.ackName);
        DOCTEST_CHECK_EQ(out.paths.size(), in.paths.size());
        DOCTEST_CHECK_EQ(out.paths[0], in.paths[0]);
    }
    // 多路径保序 + 空串路径 + 非法 UTF-8 字节原样搬运（传输层不校验，上层 B7 兜底）
    {
        const PathPayload in{"ack-x", {"a.md", "", "b.md", "\xFF\xFE 坏字节.md", "c.md"}};
        PathPayload out;
        DOCTEST_CHECK(decodePathPayload(encodePathPayload(in), out));
        DOCTEST_CHECK_EQ(out.paths.size(), in.paths.size());
        bool allEqual = true;
        for (size_t i = 0; i < in.paths.size(); ++i) {
            if (out.paths[i] != in.paths[i]) allEqual = false;
        }
        DOCTEST_CHECK(allEqual);
    }
    // 上界贴线：整包恰 32KB 能编能解；超 1 字节拒编
    {
        const std::string big(kMaxPayloadBytes - 17, 'x');  // 17 = 魔数4+ackLen4+ack1+count4+len4
        const PathPayload in{"a", {big}};
        PathPayload out;
        DOCTEST_CHECK(decodePathPayload(encodePathPayload(in), out));
        DOCTEST_CHECK_EQ(out.paths[0].size(), big.size());
        const PathPayload over{"a", {big + "x"}};
        DOCTEST_CHECK(encodePathPayload(over).empty());
    }
    return true;
})

REGISTER_TEST(si_payload_codec_reject,
              "single_instance: 魔数错/截断/字段越界/路径数超界/超 32KB → 整包拒收", {
    using platform::single_instance::PathPayload;
    using platform::single_instance::decodePathPayload;
    using platform::single_instance::encodePathPayload;
    using platform::single_instance::kMaxPayloadBytes;

    const std::string good = encodePathPayload(PathPayload{"ack", {"a.md", "b.md"}});
    DOCTEST_CHECK_FALSE(good.empty());

    // 魔数错
    {
        std::string bad = good;
        bad[0] = static_cast<char>(bad[0] ^ 0xFF);
        PathPayload out;
        DOCTEST_CHECK_FALSE(decodePathPayload(bad, out));
    }
    // 截断（拦腰半包）
    {
        PathPayload out;
        DOCTEST_CHECK_FALSE(decodePathPayload(good.substr(0, good.size() / 2), out));
    }
    // ack 名长越界（声明长度超过剩余字节）
    {
        std::string bad = good;
        const unsigned char over[4] = {0xFF, 0xFF, 0xFF, 0x7F};
        std::memcpy(bad.data() + 4, over, 4);
        PathPayload out;
        DOCTEST_CHECK_FALSE(decodePathPayload(bad, out));
    }
    // 路径数超上界（257 > kMaxPayloadPaths；字段位于 offset 8，ack 名为空）
    {
        std::string p = encodePathPayload(PathPayload{"", {"x.md"}});
        p[8] = 0x01;
        p[9] = 0x01;
        PathPayload out;
        DOCTEST_CHECK_FALSE(decodePathPayload(p, out));
    }
    // 路径声明长度越界（65536 > 剩余；字段位于 offset 12）
    {
        std::string p = encodePathPayload(PathPayload{"", {"x.md"}});
        const unsigned char big[4] = {0x00, 0x00, 0x01, 0x00};
        std::memcpy(p.data() + 12, big, 4);
        PathPayload out;
        DOCTEST_CHECK_FALSE(decodePathPayload(p, out));
    }
    // 解码端：输入超 32KB / 空串
    {
        PathPayload out;
        DOCTEST_CHECK_FALSE(
            decodePathPayload(std::string(kMaxPayloadBytes + 1, '\0'), out));
        DOCTEST_CHECK_FALSE(decodePathPayload(std::string(), out));
    }
    return true;
})

#if defined(_WIN32)

namespace {

// 跨用例记录容器：static 存储期——用例 fail-fast 跳过收尾清理也不产生悬垂回调
std::vector<std::string>& siRecordingSink() {
    static std::vector<std::string> sink;
    return sink;
}

} // namespace

REGISTER_TEST(si_ack_timeout_short_seam,
              "single_instance: ack 超时走注入短缝（120ms 封顶即返 false，不等真 5s）", {
    platform::single_instance::setSingleInstanceRootForTest("DevDesk.Tests.SI.TO");
    // 手建唤醒事件（此根名无监听线程）：让发送事务推进到「等专名 ack」一步，
    // 否则发送方在打开唤醒事件的重试段就退出、根本走不到 ack 等待
    HANDLE wake = CreateEventW(nullptr, TRUE, FALSE,
                               L"Local\\DevDesk.Tests.SI.TO.SingleInstance.Event");
    DOCTEST_CHECK(wake != nullptr);
    if (!wake) return true;

    const auto t0 = std::chrono::steady_clock::now();
    const bool acked = platform::single_instance::notifyExistingInstance(
        {"D:\\无监听\\a.md"}, 120);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    DOCTEST_CHECK_FALSE(acked);   // 无首实例应答 → 未送达
    DOCTEST_CHECK_MESSAGE(ms < 2000,
                          "ack 等待未走注入短缝（耗时 " + std::to_string(ms) +
                              "ms，疑似写死 5000ms）");

    CloseHandle(wake);
    return true;
})

REGISTER_TEST(si_section_garbage_dropped,
              "single_instance: 悬垂/半包/坏魔数整包丢弃（不误 ack 不入队）", {
    platform::single_instance::setSingleInstanceRootForTest("DevDesk.Tests.SI.Garbage");
    platform::single_instance::setOpenPathsHandler(nullptr);  // 防其它用例残留回调

    using platform::single_instance::PathPayload;
    using platform::single_instance::consumeSectionSignalOnce;
    using platform::single_instance::encodePathPayload;
    using platform::single_instance::writeSectionForTest;

    siRecordingSink().clear();
    platform::single_instance::setOpenPathsHandler([](const std::vector<std::string>& paths) {
        siRecordingSink().insert(siRecordingSink().end(), paths.begin(), paths.end());
    });

    // 坏魔数（悬垂 section 常态：发送者超时遗留的任意脏字节）
    DOCTEST_CHECK(writeSectionForTest(std::string(64, 'Z')));
    DOCTEST_CHECK_FALSE(consumeSectionSignalOnce());
    // 半包：合法载荷拦腰截断（发送方被覆写到一半）
    const std::string good =
        encodePathPayload(PathPayload{"ack", {"a.md", "b.md"}});
    DOCTEST_CHECK(writeSectionForTest(good.substr(0, good.size() / 2)));
    DOCTEST_CHECK_FALSE(consumeSectionSignalOnce());
    // 悬垂覆写交错：旧整包与新半包混写（超时释放锁后下一发送者覆写到一半）
    const std::string other =
        encodePathPayload(PathPayload{"ack2", {"c.md", "d.md", "e.md"}});
    const std::string torn =
        other.substr(0, other.size() / 2) + good.substr(other.size() / 2);
    DOCTEST_CHECK(writeSectionForTest(torn));
    DOCTEST_CHECK_FALSE(consumeSectionSignalOnce());

    // 整包丢弃 = 路径零投递
    DOCTEST_CHECK(siRecordingSink().empty());
    platform::single_instance::setOpenPathsHandler(nullptr);
    return true;
})

REGISTER_TEST(si_ack_name_pairing,
              "single_instance: ack 专名配对——只按载荷内名字放行，异名不被误 ack", {
    platform::single_instance::setSingleInstanceRootForTest("DevDesk.Tests.SI.Pair");
    const wchar_t* kRight = L"Local\\DevDesk.Tests.Ack.PairRight";
    const wchar_t* kWrong = L"Local\\DevDesk.Tests.Ack.PairWrong";
    HANDLE right = CreateEventW(nullptr, TRUE, FALSE, kRight);
    HANDLE wrong = CreateEventW(nullptr, TRUE, FALSE, kWrong);
    DOCTEST_CHECK(right != nullptr);
    DOCTEST_CHECK(wrong != nullptr);
    if (!right || !wrong) {
        if (right) CloseHandle(right);
        if (wrong) CloseHandle(wrong);
        return true;
    }

    siRecordingSink().clear();
    platform::single_instance::setOpenPathsHandler([](const std::vector<std::string>& paths) {
        siRecordingSink().assign(paths.begin(), paths.end());
    });

    // 载荷头携带 ack 名 = PairRight：消费一次后 Right 有信号、Wrong 无信号、路径已投递
    const std::string bytes = platform::single_instance::encodePathPayload(
        platform::single_instance::PathPayload{"Local\\DevDesk.Tests.Ack.PairRight",
                                               {"D:\\p\\a.md", "D:\\p\\b.md"}});
    DOCTEST_CHECK(platform::single_instance::writeSectionForTest(bytes));
    DOCTEST_CHECK(platform::single_instance::consumeSectionSignalOnce());
    DOCTEST_CHECK_MESSAGE(WaitForSingleObject(right, 0) == WAIT_OBJECT_0,
                          "载荷内专名 ack 未被放行");
    DOCTEST_CHECK_MESSAGE(WaitForSingleObject(wrong, 0) == WAIT_TIMEOUT,
                          "载荷外异名 ack 被误放行（配对失效）");
    DOCTEST_CHECK_EQ(siRecordingSink().size(), 2u);

    // 异名等待者以 60ms 短窗复核：无人为它 SetEvent（错配防护；封顶即证非恒真）
    DOCTEST_CHECK(WaitForSingleObject(wrong, 60) == WAIT_TIMEOUT);

    ResetEvent(right);
    ResetEvent(wrong);
    platform::single_instance::setOpenPathsHandler(nullptr);
    CloseHandle(right);
    CloseHandle(wrong);
    return true;
})

REGISTER_TEST(si_concurrent_senders_serialized,
              "single_instance: 并发双发送者——发送互斥串行化（后者整段事务晚于前者放锁）", {
    platform::single_instance::setSingleInstanceRootForTest("DevDesk.Tests.SI.Conc");
    // 双线程并发 notify（各注入 250ms ack 封顶；此根名无监听 → 双双超时 false）。
    // 串行化断言：两个发送者的事务完成时刻必须错开 ≥150ms——若互斥未生效，
    // 两者各自并行 250ms 超时、几乎同刻结束（gap < 50ms），本用例必红
    using Clock = std::chrono::steady_clock;
    Clock::time_point ends[2] = {Clock::now(), Clock::now()};
    bool acked[2] = {true, true};
    std::atomic<bool> go{false};
    auto sender = [&](int slot, const char* path) {
        while (!go.load(std::memory_order_acquire)) {
            Sleep(5);
        }
        acked[slot] = platform::single_instance::notifyExistingInstance(
            std::vector<std::string>{path}, 250);
        ends[slot] = Clock::now();
    };
    std::thread a(sender, 0, "D:\\conc\\a.md");
    std::thread b(sender, 1, "D:\\conc\\b.md");
    go.store(true, std::memory_order_release);
    a.join();
    b.join();

    DOCTEST_CHECK_FALSE(acked[0]);
    DOCTEST_CHECK_FALSE(acked[1]);
    const Clock::time_point& first = ends[0] < ends[1] ? ends[0] : ends[1];
    const Clock::time_point& second = ends[0] < ends[1] ? ends[1] : ends[0];
    const long long gapMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                second - first)
                                .count();
    DOCTEST_CHECK_MESSAGE(gapMs >= 150,
                          "两个并发发送者几乎同刻结束——发送互斥未串行化（gap=" +
                              std::to_string(gapMs) + "ms）");
    return true;
})

REGISTER_TEST(si_chain_full_inprocess,
              "single_instance: 进程内全链路——模拟第二实例发送→监听线程读→队列→drain", {
    platform::single_instance::setSingleInstanceRootForTest("DevDesk.Tests.SI.Chain");
    // 首实例形态（测试根名隔离）：拿锁启动监听线程 + 注册「回调 → app_assembly 队列」
    DOCTEST_CHECK_MESSAGE(platform::single_instance::acquireInstanceLock(),
                          "测试根名互斥被占（有并发 devdesk_tests 实例在跑？）");
    app::assembly::resetSecondInstanceFilesForTest();
    platform::single_instance::setOpenPathsHandler(
        [](const std::vector<std::string>& paths) {
            app::assembly::enqueueSecondInstancePaths(paths);
        });

    siRecordingSink().clear();
    app::assembly::setOsOpenFunnel([](const std::string& path) {
        siRecordingSink().push_back(path);
    });

    // drain 轮询辅助：监听线程异步消费，10ms 步进、3s 截止（不靠真等待定结果）
    auto drainUntil = [](size_t want) {
        for (int i = 0; i < 300; ++i) {
            app::assembly::drainPendingSecondInstanceFiles();
            if (siRecordingSink().size() >= want) return true;
            Sleep(10);
        }
        return siRecordingSink().size() >= want;
    };

    // 第一笔：两路径（监听线程 500ms 轮询，ack 最坏 ~0.6s；注入封顶 2.5s）
    const bool acked1 = platform::single_instance::notifyExistingInstance(
        {"D:\\si\\一.md", "D:\\si\\二.md"}, 2500);
    DOCTEST_CHECK_MESSAGE(acked1, "第二实例发送未在封顶内被自己的专名 ack 放行");
    DOCTEST_CHECK(drainUntil(2));
    DOCTEST_CHECK_EQ(siRecordingSink().size(), 2u);
    DOCTEST_CHECK_EQ(siRecordingSink()[0], std::string("D:\\si\\一.md"));
    DOCTEST_CHECK_EQ(siRecordingSink()[1], std::string("D:\\si\\二.md"));

    // 第二笔：顺序双发各被自己的专名 ack 放行（边界用例②的时序面，无串扰）
    siRecordingSink().clear();
    const bool acked2 = platform::single_instance::notifyExistingInstance(
        {"D:\\si\\三.md"}, 2500);
    DOCTEST_CHECK_MESSAGE(acked2, "第二笔发送未被放行");
    DOCTEST_CHECK(drainUntil(1));
    DOCTEST_CHECK_EQ(siRecordingSink()[0], std::string("D:\\si\\三.md"));

    app::assembly::resetSecondInstanceFilesForTest();
    platform::single_instance::setOpenPathsHandler(nullptr);
    return true;
})

#endif
