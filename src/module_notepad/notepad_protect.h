// notepad_protect.h — 未保存保护纯决策层（need:263 批 4 棒 A，F-05）。
// 三选（保存/不保存/取消）×两语境（关窗 veto / 退出 quit guard）的全部裁决
// 收敛为纯函数面，UI 回调与 quit guard 只做薄接线（doctest / probe 可无头直测）。
// 保护链只读 session.dirty（批 3 三元基线比对单点产出），本层不重复判定。
#pragma once

#include <cstddef>
#include <vector>

#include "notepad_session.h"

namespace module::notepad {

// 三选动作（对话框三钮 → 后续动作映射表的输入维度）。
enum class NotepadProtectChoice { Save, Discard, Cancel };

// 三选结果 → 后续动作（关窗语境与退出语境共用同一映射，单点防语义漂移）：
// - closeWindow：请求关窗（关窗语境）；退出语境恒 false（退出统一由
//   requestExit 再入队走 guard 放行，不逐窗 requestWindowClose）。
// - rearmExit：退出语境需 app::requestExit() 重新置位 → 下一帧再进 guard
//   （clue #269：请求标志 exchange 取走即清，模块处理完必须再入队）。
// - armExempt：置 skipCloseConfirm 一次性豁免旗标（「不保存」）。
struct NotepadProtectOutcome {
    bool closeWindow = false;
    bool rearmExit = false;
    bool armExempt = false;
};

// 映射表：
//   保存+成功：关窗语境=closeWindow；退出语境=rearmExit（该窗已干净，
//              下一帧 guard 跳过它继续处理下一窗）。
//   保存+失败（mtime 拦截/写失败，dirty 未清）：三动作全 false = 留窗 +
//              退出链中止（用户处理后需重新发起退出）。
//   不保存：置豁免；关窗语境=closeWindow，退出语境=rearmExit（guard 下一
//              帧按豁免跳过该窗继续下一窗）。
//   取消：全 false = 留窗；退出语境不 rearm = 中止整个退出（D-3 定案）。
NotepadProtectOutcome notepadProtectOutcomeFor(NotepadProtectChoice choice,
                                               bool saveSucceeded,
                                               bool quitContext);

// 关窗 veto 裁决（notepad_module.cpp onCloseRequested 唯一消费点）：
// 干净直通放行；脏且未豁免 → 拦截（调用方弹三选，返回 false 拦 GLFW close）；
// 脏且已豁免 → 放行并**一次性消费**豁免旗标（下次脏关窗重新保护）。
bool notepadCloseShouldIntercept(NotepadSession& s);

// quit guard 扫描（对给定窗序=注册表创建序的纯判定）：
//   Allow     —— 全部窗已处理完（干净/已豁免/空注册表）→ 放行退出（穷尽路径①③）
//   WaitDialog—— 已有待决三选弹窗（任意语境）→ 等用户，不叠开第二张
//   AskAt     —— 首个待决脏窗下标 → 调用方对其弹三选（退出语境）
struct NotepadQuitScan {
    enum class Kind { Allow, WaitDialog, AskAt };
    Kind kind = Kind::Allow;
    size_t index = 0;
};
NotepadQuitScan notepadQuitScan(const std::vector<NotepadSession*>& ordered);

// quit guard 本体（app::setQuitGuard 唯一消费点，app_assembly 装配期挂）：
// 按 notepadQuitScan 扫注册表创建序；AskAt → 对该窗置 protectOpen +
// protectQuitContext=true（弹窗渲染在该窗自己的 Ui 树内）并返回 false 阻止
// 本次退出；Allow → true 放行（穷尽路径：全干净/全豁免/注册表空）。
// 「拒退一次后可正常退出」与「取消后不卡死」由 probe G 段断言（clue #269）。
bool notepadQuitGuard();

}  // namespace module::notepad
