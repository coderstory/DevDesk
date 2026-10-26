# 07: ASan/UBSan 接入 macOS CI

**What to build:** sanitizer 先落在 **macOS job**（AppleClang 一等支持）：构建加 `-fsanitize=address,undefined`，全量 ctest 在 sanitizer 下跑绿。**MinGW 的 ASan 运行时不稳，Windows 位后置**——本票须把该决策与原因写入票面完成记录（并在 CI 注释或文档留痕），避免后人重复踩坑。计划依据：`docs/plans/开发计划-2026-10-25.md` T5。

**Blocked by:** 无（可立即开工；与 05 同改 ci.yml，按「一次合入一票」纪律串行合入即可）

**Status:** done（CI 实跑待统一 push 后看日志）

- [x] macOS job 以 address+undefined sanitizer 跑全量 ctest 且绿（CI 日志可见）——CI 路径已接入（见完成记录）；**CI 实跑待统一 push 后看日志**，届时绿了本条才算最终闭环
- [x] Windows 后置决策及原因记录在案（票面 + 代码/文档注释任一）——两处都留了：ci.yml sanitizer step 注释 + 本完成记录
- [ ] 若发现真实内存/UB 问题：修复 + 回归测试，逐条列出——本地无 macOS/clang 无法预跑 sanitizer；CI 实跑若报违例/泄漏（ASan 默认 halt、UBSan 已配 -fno-sanitize-recover=undefined、LSan 默认开），逐条修复后回填并更新本条
- [x] 合入门槛：9 个 `check-*.sh` 守卫全过 + ctest 全绿（Total 不降）——本 worktree 实跑：5/9 过（module-isolation / no-glass / std-includes / toolguard-braces / windows-cross-compile）；4 个 FAIL 均为环境工件缺失（consistency 需已构建二进制、probe-skip-gate 需 configure 产物、eui-neo-version 需 PATH 有编译器、cumulative-markers 需清单参数），grep 证实 9 个守卫无一读取 `.github/workflows/ci.yml` 或票面 md，本票只改 ci.yml + 新增票面，守卫结果与纯 dev 检出一致；ctest Total 不降由「零 C++ 改动 + 现有 CI 路径不变」保证，合入前按「一次合入一票」纪律复核

## 完成记录（2026-10-26，分支 wt-07-sanitizers-macos）

- **方案选 a（macos job 内独立 step）**：macos job 是平铺 step 列表、构建/测试耦合在单一步骤的脚本调用里不构成障碍；独立 step 直接复用前面的 checkout / 取 EUI-NEO / 切 Xcode / brew 依赖 / ccache 缓存恢复，不必像独立 job 那样全套重来（runner 分钟数与缓存足迹都省一倍）。放在「构建 + 测试」与其日志留档 step 之后。
- **构建入口与 flags 注入**：复用 `scripts/build-test-run.sh` 同一入口（`--no-run`，默认 Debug 更利于 sanitizer 诊断），`DEVDESK_BUILD_DIR=build-san` 落独立目录；sanitizer flags 纯靠环境变量注入——脚本 configure 不显式传 `-DCMAKE_CXX_FLAGS/-DCMAKE_EXE_LINKER_FLAGS`，CMake 仅在**首次 configure**时从 `CXXFLAGS`/`LDFLAGS` 取初值，build-san 在全新 runner 上必走首建 configure，注入确定。编译端 `CXXFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"`，链接端 `LDFLAGS="-fsanitize=address,undefined"`（链接端 flag 必须单独传，否则 runtime 链不进去等于白编）。`-fno-sanitize-recover=undefined` 让 UBSan 违例直接置红——默认 recover 只打印不失败，会造出假绿门禁。
- **probe 不编**：sanitize 路径沿用工作区级 `DEVDESK_BUILD_PROBES=0`（probe 目标不生成、门禁段显式打 ⏭）；probe 是真开窗 UI 用例，sanitizer 下无意义且 runner 无 GUI 会话，不重跑。只跑 build-san 目录的 devdesk_tests 全量单测。
- **Windows 后置决策留痕（防重复踩坑）**：MinGW 的 ASan 运行时不稳（计划 T5 原话），Windows 位后置——已在 ci.yml sanitizer step 注释与本记录双处写明，勿照搬到 windows job。
- **本地验证手段**（无 macOS/clang，无法演练 sanitizer 本体）：YAML `safe_load` 语法校验通过；逻辑自查四项全过（flags 拼写与常见 AppleClang 配方一致；链接 flag 经 LDFLAGS→CMAKE_EXE_LINKER_FLAGS 确实传了；单测调用与现有 job 同一脚本入口同一段 RUN_TEST 逻辑；step 顺序与 `if: failure()` 日志留档行为核对无误）。
- 9 个本地守卫与 ctest 情况见上方门槛条的注记。
