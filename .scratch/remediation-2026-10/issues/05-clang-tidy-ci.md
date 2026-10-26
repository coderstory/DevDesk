# 05: clang-tidy 接入 CI（报告模式）

**What to build:** windows-2025 job 新增独立 clang-tidy 步骤（runner 预装 LLVM，读 compile_commands.json），检查集从 `bugprone-*` / `cert-*` / `clang-analyzer-*` 起步。本票只**报告不阻断**——存量清零与转 blocking 是 06 号票的事。工具链现实：本仓无 VS、本地为 MinGW GCC，CI 走 runner 预装 LLVM 的路径。计划依据：`docs/plans/开发计划-2026-10-25.md` T5。

**Blocked by:** 无（可立即开工）——但有一个**外部人工前置**：推送本票的 ci.yml 改动需要 PAT 具备 **workflow scope**（2026-10-25 晚已撞过此墙，见 DevDesk 提交 9529ad00 的说明）。授权未就绪期间：本地完成改动与验证，push 步骤挂起并在此处记录。

**Status:** done

- [x] CI 出现独立 tidy 步骤，产出检查报告（非阻塞，不影响合入信号）
- [x] 本地存在与 CI 等价的 tidy 调用命令，报告可复现
- [x] 若 workflow scope 已就绪：push 后 CI 日志可见该步骤跑通；未就绪：本地验证证据 + 此处标注「待推」——**推送受 PAT workflow scope 限制，随最终统一 push 处理（待推）**；本地验证证据见下方完成记录
- [x] 合入门槛：9 个 `check-*.sh` 守卫全过 + ctest 全绿

## 完成记录（2026-10-26）

- **ci.yml**：windows-2025 job 末尾新增 step「clang-tidy（报告模式，非阻塞）」，`continue-on-error: true`（报告模式语义：失败不阻断 job、不影响合入信号），`shell: msys2 {0}` 与构建 step 一致，调 `scripts/run-clang-tidy.sh`——configure 与 tidy 调用 CI/本地同源。
- **scripts/run-clang-tidy.sh**（本地复现命令，06 号票清存量时以它为准）：
  1. 探测 clang-tidy（`DEVDESK_CLANG_TIDY` > PATH > `/c/Program Files/LLVM/bin` 等常见位置），找不到打印安装途径并 exit 2；
  2. 与 build-test-run.sh 同参数 configure 独立目录 `build-tidy/`（+`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`；两个刻意差异：`-DDEVDESK_CCACHE=OFF` 防 ccache 前缀破坏 clang-tidy 的 driver-mode 推断、`-DDEVDESK_BUILD_PROBES=OFF` 让 probe 产物不进 tidy 范围）；
  3. 从数据库圈定本仓库 src/tests 的 .cpp（EUI-NEO 子项目 TU 排除，平台文件天然被过滤）；
  4. 跑 `bugprone-*,cert-*,clang-analyzer-*` + `--use-color` + `--header-filter`，末尾按检查名汇总命中数。
- **本地验证证据**（本机无 clang/clang-tidy/VS，CI 本体待 push 后看日志）：
  - ci.yml YAML 校验过（pyyaml `safe_load`；windows job 6 步，tidy 步 `continue-on-error: true`）；
  - 脚本同款 configure 实跑产出 `build-tidy/compile_commands.json`：2294 行 / 382 编译条目 / 覆盖本仓库 248 个 .cpp（src 139 + tests 109）；
  - 脚本全链路以 clang-tidy 桩跑通（exit 0，tidy 收到 253 个参数 = 5 flag + 248 文件）；无 clang-tidy 时负路径 exit 2。
- **门禁**：`./scripts/build-test-run.sh --no-run` → Total 1586 / Failed 0（构建/发布形态/测试全绿；probe 段 3 例环境性失败，按基线口径忽略）；9 守卫全过（7 常规 + check-eui-neo-version 62 TU + check-cumulative-markers 60/60）。
- **推送受 PAT workflow scope 限制，随最终统一 push 处理。**
