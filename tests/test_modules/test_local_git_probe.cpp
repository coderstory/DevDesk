// tests/test_modules/test_local_git_probe.cpp
// 本地 git 可用性探测：只管**平台无关的排空决策规则**（pipeDrainStep）。
//
// 为什么不测真路径：macOS 实现是 fork/exec `/usr/bin/xcode-select -p` + 非阻塞读，
// 没有可注入缝（改造成注入 readFn/nowFn 的代价远超这条 info 项收益）。所以判据钉的是
// 「优先级规则」；循环「每圈都过规则、且只杀活着的子进程」由实现保证，风险点写在
// `local_git_probe_macos.cpp` 的注释里，改那里必须回看本文件的 ①。
//
// 原始缺陷（T4 审计登记）：deadline 检查写在「读到数据 → continue」之后 —— 子进程
// 持续输出（永不 EOF）时循环永不查表，探测线程卡死、设置卡永久「检测中」（3s 上界
// 名存实亡）。①正是那种病态输入。
#include "doctest.h"

#include "module_settings/local_git_probe.h"

using module::settings::pipeDrainStep;
using module::settings::PipeDrainStep;

REGISTER_TEST(local_git_probe_drain_priority,
              "local_git_probe: 排空决策优先级（超时优先于「还有数据」/ 已退出读完不杀）", {
    // ① 病态输入 = 旧 bug 现场：子进程还活着、未 EOF、已超时 → 必须 Timeout
    DOCTEST_CHECK(pipeDrainStep(false, false, true) == PipeDrainStep::Timeout);
    // ② 已退出 + 已 EOF → Done。哪怕同一圈刚好过点：东西已收全，杀一个死进程无意义。
    DOCTEST_CHECK(pipeDrainStep(true, true, true) == PipeDrainStep::Done);
    DOCTEST_CHECK(pipeDrainStep(true, true, false) == PipeDrainStep::Done);
    // ③ 已退出但管道没读完 → 继续读（不杀：写端已死，管道内容有界、很快读完）
    DOCTEST_CHECK(pipeDrainStep(true, false, true) == PipeDrainStep::ReadMore);
    DOCTEST_CHECK(pipeDrainStep(true, false, false) == PipeDrainStep::ReadMore);
    // ④ 正常在途 → 继续读
    DOCTEST_CHECK(pipeDrainStep(false, false, false) == PipeDrainStep::ReadMore);
    DOCTEST_CHECK(pipeDrainStep(false, true, false) == PipeDrainStep::ReadMore);
    // ⑤ 非恒真自证：三个分支两两不同 —— 上面的断言不是「只认一个值」就过
    DOCTEST_CHECK(PipeDrainStep::ReadMore != PipeDrainStep::Timeout);
    DOCTEST_CHECK(PipeDrainStep::Done != PipeDrainStep::Timeout);
    DOCTEST_CHECK(PipeDrainStep::ReadMore != PipeDrainStep::Done);
    return true;
})
