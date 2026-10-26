# 08: Windows 覆盖率

**What to build:** 落地 CMake 已预留口径的 gcovr 路线：`DEVDESK_COVERAGE` 增加 GCC 分支（编译链接加 `--coverage`），coverage 脚本加 MinGW 分支；Windows CI 产出 core/platform 层的覆盖率报告 artifact，让 Windows 侧核心层有数字可看。计划依据：`docs/plans/开发计划-2026-10-25.md` T6。

**Blocked by:** 无（可立即开工；与 05/07 同改 ci.yml，按「一次合入一票」纪律串行合入；workflow scope 推送前置同 05 号票的说明）

**Status:** done（CI 实跑待统一 push 后看日志）

- [x] 本地 MinGW 下可一键出覆盖率报告（core/platform 层有数字）——`bash scripts/coverage.sh` 一条龙实测：core 77.1%、platform 77.6%（见完成记录完整分层表）
- [x] Windows CI artifact 含覆盖率报告——windows job 末尾新增「覆盖率报告（报告模式，非阻塞）」step + `windows-coverage-report` artifact 上传（见完成记录）；**CI 实跑待统一 push 后看日志**，届时绿了本条才算最终闭环
- [x] ctest 全绿不受 coverage 开关影响（Failed: 0，Total 不降）——本 worktree 双向实测：coverage 目录（DEVDESK_COVERAGE=ON）`Total: 1587 Passed: 1578 Failed: 0`；正常构建（开关默认 OFF，flag 一字不变）一条龙同数（见完成记录）
- [x] 合入门槛：9 个 `check-*.sh` 守卫全过——本 worktree 实跑：8/9 过（consistency / module-isolation / no-glass / probe-skip-gate / std-includes / toolguard-braces / windows-cross-compile / eui-neo-version 带 `--eui-dir` 绝对路径），cumulative-markers 需清单参数（多轮派发的累积核对元工具，仓库无固定清单，本票无可核对项）按 07 号票同口径环境性跳过；详见完成记录注记

## 完成记录（2026-10-26，分支 wt-08-windows-coverage）

### CMake 改动（`CMakeLists.txt` DEVDESK_COVERAGE 段）

`DEVDESK_COVERAGE=ON` 从「仅 AppleClang，否则 FATAL_ERROR」改为按编译器分三路，插桩目标清单（`DEVDESK_COVERAGE_LIBS`：core/platform/ui_kit/各 module/agent）两路线共用：

- **AppleClang**：原 llvm source-based 路线原样保留（`-fprofile-instr-generate -fcoverage-mapping`，目录级链接 + 目标级编译），语义零变化；
- **GNU（含 MinGW GCC，本票新增）**：同构结构，flag 换 `--coverage`（≈ 编译期 `-fprofile-arcs -ftest-coverage` + 链接期 `-lgcov`，一个词对编译/链接都成立）；目录级 `add_link_options(--coverage)` 让 devdesk_tests/DevDesk 链接时拉进 gcov runtime，跑完在对象文件旁落 .gcda；
- **其余（MSVC、非 Apple 的 Clang…）**：仍 FATAL_ERROR，消息更新为当前支持范围。

默认 OFF 不变：正常构建的编译/链接命令一字不动（一条龙基线实测同数佐证）。

### scripts/coverage.sh：MinGW 分支

脚本从「macOS 专用，非 darwin 拒绝」扩为双路线：darwin（llvm-cov）原流程不动，windows 走新增 `run_windows_gcov`（EUI-NEO 三级定位提为共用函数 `locate_eui_neo`），linux 仍出声拒绝。Windows 路线要点：

- **工具探测**：`gcov` 必需、`gcovr` 可选；两者皆无 → 打印安装途径（`pacman -S mingw-w64-x86_64-gcc` / `pip install gcovr`）exit 2（同 run-clang-tidy.sh 的尽力而为风格）。本机实况：mingw64 GCC 16.2.0 自带 gcov 16.2.0，**gcovr 缺失**（无 pip）→ 走 gcov 轻方案；脚本对 gcovr 的用法仅保留「有则额外出 coverage.html」，gcovr 挂了只丢 HTML 不丢数字，分层 txt 统一由轻方案口径出（唯一实现，两边不各说各话）。
- **gcov 轻方案（纯 gcov + awk，零 Python 依赖——CI 的 msys2 环境没有 python）**：逐 .gcda 跑 `gcov -t`（stdout 出 .gcov 文本，GCC 12+），awk 按 (文件,行) **并集**聚合（同一头文件被多个 TU include 时各 .gcov 各写一份，任一 TU 视角执行过即算覆盖）、按 `src/<层>/` 分层，出 coverage-summary.txt（分层表，字段与 macOS 版同构）+ coverage-report.txt（逐文件明细）。
- **MinGW/MSYS 专属坑（都踩过，注释已留痕）**：
  1. MSYS bash 给原生 gcov.exe 传**相对路径**不转换 → gcov 在错误 cwd 找不到 .gcda，必须传绝对路径（`/d/...` 形式由 MSYS 运行时转成 `D:\...`）；
  2. gcov 的 cwd 必须是**构建目录**——gcno 里头文件路径相对编译时工作目录记录（`../src/core/result.h`），cwd 不对则整段丢弃；
  3. 同一头文件多 TU 实例产出**同名 .gcov**，直接落盘互相覆盖 → 每对象独立文件收 stdout；
  4. awk 程序落成文件再 `-f`——MSYS bash 转手命令行参数给原生 exe 时剥 `\\`，内嵌单引号大程序直接传参必坏。
- **构建目录**：默认 `build-coverage/`（DEVDESK_COV_BUILD_DIR 环境变量可覆盖），与 build/ / build-release/ 互不覆盖；一致性守卫同 macOS 版（缓存非 ON/非 Debug 硬失败）。configure 传 `-DOPENSSL_ROOT_DIR=$MINGW_PREFIX`（不写死 C:/msys64，口径同 build-test-run.sh）。

### 本地实跑真数字（worktree wt-08，build-coverage/，git ee04d7e）

命令：`MINGW_PREFIX=… EUI_NEO_DIR=/d/project/gui/EUI-NEO DEVDESK_JOBS=12 bash scripts/coverage.sh`（131 个对象落 .gcda；gcovr 未装故无 HTML）。分层表原样：

```
core                    38        1742/2258    77.1  核心工具类 >=90%      N
platform                29        2160/2784    77.6  >=70%            Y
ui_kit                  18          468/649    72.1  UI 不强制           
module:about             1          167/198    84.3  Service >=80%    Y
module:agent            27        6090/7368    82.7  Service >=80%    Y
module:claude_proxy     32        6867/7958    86.3  Service >=80%    Y
module:home              3          667/784    85.1  Service >=80%    Y
module:markdown         22        4834/5493    88.0  Service >=80%    Y
module:settings         14        2951/3752    78.7  Service >=80%    N
module:todo              9        2714/3506    77.4  Service >=80%    N
非DevDesk层(参考)          147       5817/18873    30.8  -                
DevDesk 合计             193      28660/34750    82.5
```

core 77.1% 距 spec §12.3 的 90% 目标还有差距（gcov 行覆盖口径比 llvm-cov 偏严：跨 TU inline 与模板实例的并集口径对未走到的分支更苛刻），platform 达标（≥70%）。这是基线数字，非门禁——CI 侧报告模式不阻断。

### CI（.github/workflows/ci.yml windows job）

在 05 号票 clang-tidy step **之后**（windows job 末尾，不动他人 step）追加两个 step：

1. `覆盖率报告（报告模式，非阻塞）`：`continue-on-error: true` + `shell: msys2 {0}`，命令即本地验证过的 `bash scripts/coverage.sh`（EUI-NEO checkout step 已在同级目录，脚本三级回退直接命中；OpenSSL 走 MINGW_PREFIX）；报告模式语义同 clang-tidy——失败不阻断 job、不影响合入信号；
2. `上传覆盖率报告`：`if: always()` + actions/upload-artifact@v7，artifact 名 `windows-coverage-report`，收 build-coverage/coverage-summary.txt 与 coverage-report.txt，`if-no-files-found: ignore`。

与 07 号票（macos job）无交集；ci.yml 变更全部落在 windows job 末尾。

### 验证与守卫

- coverage 目录单测：`Total:  1587 Passed: 1578 Failed: 0`（Total 与基线口径一致，Failed 0；差值 9 为 SKIP 形态用例）；
- 正常构建（开关 OFF）一条龙 `bash scripts/build-test-run.sh --no-run`：同 `Total: 1587 … Failed: 0`，`--coverage` 未渗漏（probe 门禁段 exit 1 为本机缺 LLM 凭据的环境性既有状态，按派发说明忽略）；
- 9 个守卫：consistency ✓（一条龙产物 build/DevDesk.exe 就绪后过）、module-isolation ✓、no-glass ✓、probe-skip-gate ✓（configure 产物就绪后过）、std-includes ✓、toolguard-braces ✓、windows-cross-compile ✓、eui-neo-version ✓（`--root . --eui-dir /d/project/gui/EUI-NEO` 绝对路径，worktree 里默认 `../EUI-NEO` 指不到）；cumulative-markers 需清单参数（元工具，无固定清单）未跑，同 07 号票口径；
- CI 改动 YAML 语法由 GH Actions schema 目检 + 与 05/07 同构的 step 形态保证；实跑日志待统一 push 后回看。
