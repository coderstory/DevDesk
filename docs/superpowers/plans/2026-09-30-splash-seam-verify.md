# splash 测试缝验证留证（2026-09-30）

修复 splash（28e5fad）打破无头测试的回归：`app_sidebar.cpp compose()` 开头的
`drawSplashIfNeeded` 在无头测试进程内恒真（进程 2.3s 内跑完 < splash 2.0s+0.3s
时序）→ 主界面不画 → 4 条 sidebar 几何用例红。修法 = 测试缝：测试/probe 进程
禁 splash，主程序路径零变化。

## 改动

| 文件 | 内容 |
|------|------|
| `src/ui_kit/splash.h` | 测试缝 API：`inline setSplashDisabledForTests(bool)` + 标志存储（inline 函数局部 static）；`drawSplashIfNeeded` 置位后恒返 false，不启动计时/动画/shader/日志/requestUpdate |
| `src/ui_kit/splash.cpp` | `drawSplashIfNeeded` 入口最短路读缝标志 |
| `tests/test_main.cpp` | TIN-07 登记第 13 项：`resetCaseGlobals` 调 `setSplashDisabledForTests(true)` |
| `tests/probes/probe_harness.h` | `initializeWindowBackend()` 同调（probe dump 不拍到 splash） |

**实现偏离说明（对原方案的唯一修改）**：标志存储原定放 `splash.cpp` 静态变量，
实际放 `splash.h` inline 函数局部 static。原因：出线 setter 符号会让 probe 链接
拉入 `splash.cpp.o`，其 `app::requestUpdate()` 依赖在 probe 侧无解——实现 TU
（glfw_app_main）只编进 DevDesk 主程序；`<eui/detail/dsl_app_impl.h>` 注入
（test_settings.cpp 的既有方式）也无法统一用于全部 probe：about/todo probe 已
自带注入，home/claude/markdown probe 注入会缺 `app::compose` 定义（实测两次
链接失败后定位）。inline 化后 probe 链接零新增依赖，语义（进程级、默认 false、
置位恒禁）不变。

## 验收数据

| 项 | 结果 |
|----|------|
| `--filter sidebar`（修复后） | Total: 8 / Passed: 8 / Failed: 0（修复前 4/8 红） |
| 全量（`--no-run`） | **Total: 868 / Passed: 862 / Failed: 0**（差值 6 = `DEVDESK_SOAK=1` 等门控 SKIP，与 2026-09-30-skipped-cases.md 口径一致） |
| `ctest -L probe` | 100% tests passed out of 8 |
| 模块隔离守卫（随脚本） | 通过 |

## 变异自检（断言非恒真）

把 `resetCaseGlobals` 里的 `::ui_kit::setSplashDisabledForTests(true);` 注释掉
（seam 失效）重跑：

```
[FAIL] sidebar: 折叠态 72px、label 隐藏、图标居中、tooltip、内容区 +136
[FAIL] sidebar: 展开态 208px、label/icon 现状不变、tooltip 无、内容区 -208
[FAIL] sidebar: 顶部留白只在侧边栏内部（页面几何不变 + 无透明带）
[FAIL] sidebar: compose 每帧写入页面区原点 x（展开 208 / 折叠 72）
Total:  8 Passed: 4 Failed: 4
```

恰为回归时的 4 条几何用例复红 → 恢复调用后 `Total: 8 / Passed: 8 / Failed: 0`
复绿。证明用例红绿确实由本缝控制，不是恒真假绿。

## splash 正常路径不受影响

- `grep -c setSplashDisabledForTests src/main.cpp` → **0**（主程序不调用该 API）
- 置位只发生在测试基建两处（`tests/test_main.cpp:147`、`tests/probes/probe_harness.h`
  `initializeWindowBackend`），均为测试/probe 进程专属入口
- 缺省态 `flag = false`，`DevDesk.exe` 正常链接并保留完整 splash 路径
  （构建产物含 `assets/shaders/splash.frag` 拷贝步骤，行为与 28e5fad 一致）

## 环境注记

验证在 worktree `/Users/coderstory/dev/gui/wt-splash-seam`（分支
`fix/splash-test-seam`，自 master 28e5fad 切出），构建/测试全走
`scripts/build-test-run.sh`（macOS arm64，ccache 热缓存）。
