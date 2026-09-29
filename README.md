<div align="center">

<br>

<img src="docs/screenshots/app-home.png" alt="DevDesk" width="820">

<br>
<br>

# DevDesk

**把日常散落的开发琐事，收进同一个窗口**

原生 C++20 GUI · 单文件可执行 · 零外部运行时依赖

[![platform](https://img.shields.io/badge/platform-macOS%20%7C%20Windows-1f2328?style=flat-square)](#平台支持)
[![c++](https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&logo=cplusplus&logoColor=white)](#架构)
[![gui](https://img.shields.io/badge/GUI-EUI--NEO-D97757?style=flat-square)](#架构)
[![deps](https://img.shields.io/badge/runtime%20deps-none-2ea44f?style=flat-square)](#快速开始)
[![tests](https://img.shields.io/badge/tests-847-2ea44f?style=flat-square)](#项目状态)

<br>

[功能](#功能) · [截图](#截图) · [快速开始](#快速开始) · [架构](#架构) · [项目状态](#项目状态) · [文档](#文档)

</div>

---

<h2 align="center"><a name="功能"></a>✨ 功能</h2>

<table align="center">
<tr><th align="left" width="200">模块</th><th align="left">做什么</th></tr>
<tr>
<td><b>📝 Markdown 笔记本</b></td>
<td>笔记就是普通 <code>.md</code> 文件，不锁定格式 —— 随时可以拿别的编辑器打开，或者直接丢进 Git。<br>
FTS5 全文搜索（中文可用）、目录树、标签、大纲面板；编辑 / 双栏 / 预览三态，400ms 自动保存。</td>
</tr>
<tr>
<td><b>✅ Todo 看板</b></td>
<td>多列看板，拖拽跨列 + 同列重排（可插到两张卡之间）。<br>
优先级直接体现在卡片底色上；截止日期、标签、搜索、逾期视图。</td>
</tr>
<tr>
<td><b>🔀 Claude 配置切换器</b></td>
<td>多套 API Profile 管理 + 本地流式代理（SSE 逐块透传）。<br>
启动代理即自动接管 Claude Code <code>settings.json</code>，停止时按键还原；多 Profile 失败自动 failover。</td>
</tr>
<tr>
<td><b>📊 首页仪表盘</b></td>
<td>今日笔记 / 待办 / 逾期 / API 调用统计；Token 速率取自最近一次成功调用，用量按小时分布。</td>
</tr>
<tr>
<td><b>⚙️ 设置</b></td>
<td>外观（主题 / 全局字号 / 窗口效果三档）、模块插拔、桌宠、笔记目录、代理端口与接管状态、日志保留。</td>
</tr>
<tr>
<td><b>🐾 桌宠</b></td>
<td>无边框置顶透明小窗，可拖拽、四态动画、气泡对话、右键菜单；默认关闭，需要时再开。</td>
</tr>
</table>

---

<h2 align="center"><a name="截图"></a>📸 截图</h2>

<table align="center">
<tr>
<td width="48%">
<img src="docs/screenshots/app-todo.png" alt="Todo 看板">
<p align="center"><sub><b>Todo 看板</b><br>优先级即卡片底色，拖拽跨列 / 同列重排</sub></p>
</td>
<td width="48%">
<img src="docs/screenshots/app-markdown.png" alt="Markdown 笔记本">
<p align="center"><sub><b>Markdown 笔记本</b><br>目录树、标签与三态视图</sub></p>
</td>
</tr>
<tr>
<td width="48%">
<img src="docs/screenshots/app-claude.png" alt="Claude 配置切换器">
<p align="center"><sub><b>Claude 切换器</b><br>Profile / 模型映射 / 环境变量 / 调用记录</sub></p>
</td>
<td width="48%">
<img src="docs/screenshots/app-about.png" alt="关于页">
<p align="center"><sub><b>关于</b><br>版本、依赖与构建信息一目了然</sub></p>
</td>
</tr>
</table>

---

<h2 align="center"><a name="快速开始"></a>🚀 快速开始</h2>

> [!IMPORTANT]
> DevDesk 依赖 **[EUI-NEO](https://github.com/sudoevolve/EUI-NEO)**，源码需要放在 DevDesk 的**同级目录**（不是 submodule）：

```text
your-workspace/
├── DevDesk/     ← 本仓库
└── EUI-NEO/     ← GUI 框架
```

放在别处也可以，configure 时用 `-DEUI_NEO_DIR=<路径>` 指定。

<details open>
<summary><b>🍎 macOS</b></summary>

<br>

```bash
brew install cmake ninja openssl@3

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3
cmake --build build
./build/DevDesk
```

</details>

<details>
<summary><b>🪟 Windows 11（MSYS2 MinGW64）</b></summary>

<br>

```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-sqlite3 \
          mingw-w64-x86_64-curl mingw-w64-x86_64-openssl

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/DevDesk.exe
```

Windows 构建默认静态链接（sqlite3 / curl / ssl / crypto / C++ 运行时），产物是**单个 exe、零外部 DLL**。

</details>

> [!TIP]
> 仓库里的 `scripts/build-test-run.sh` 把「构建 → 测试 → 启动」串成一条命令，并封装了 ccache、平台 PATH、模块隔离守卫等约定。**日常开发建议直接用它，别手敲 cmake** —— 手敲容易漏掉约定，造出互相漂移的构建目录。

---

<h2 align="center"><a name="架构"></a>🏗 架构</h2>

```mermaid
graph TD
    M["main"] --> MOD["modules"]
    MOD --> M1["home"] & M2["markdown"] & M3["todo"] & M4["claude_proxy"] & M5["settings"] & M6["pet"]
    MOD --> CORE["core<br/><sub>Result · SqliteDb · Router · Theme · Logging</sub>"]
    CORE --> PLAT["platform<br/><sub>HttpClient · HttpServer · Keychain · Json</sub>"]
    PLAT --> EUI["EUI-NEO<br/><sub>OpenGL / Vulkan × GLFW / SDL2</sub>"]

    style M fill:#D97757,color:#fff,stroke:none
    style MOD fill:#f5f2ec,stroke:#d0c9bd
    style CORE fill:#f5f2ec,stroke:#d0c9bd
    style PLAT fill:#f5f2ec,stroke:#d0c9bd
    style EUI fill:#f5f2ec,stroke:#d0c9bd
```

<h4 align="center">贯穿全局的几条约定</h4>

<table align="center">
<tr><th align="left" width="200">约定</th><th align="left">说明</th></tr>
<tr><td><b>不抛异常</b></td><td>所有错误经 <code>Result&lt;T&gt;</code> 返回，调用方必须显式处理</td></tr>
<tr><td><b>模块隔离</b></td><td>每个 <code>module_*</code> 是独立静态库，之间不可互相依赖</td></tr>
<tr><td><b>core 不反向依赖</b></td><td><code>core</code> 不依赖 <code>platform</code>，也不调用 GUI 框架的运行时行为</td></tr>
<tr><td><b>平台适配接口隔离</b></td><td>平台差异按 <code>*_macos.cpp</code> / <code>*_win.cpp</code> / <code>*_posix.cpp</code> 拆文件、CMake 选源；业务代码里没有平台宏</td></tr>
<tr><td><b>数据根在用户主目录</b></td><td>macOS <code>~/DevDesk</code>、Windows <code>%USERPROFILE%\DevDesk</code>；可用 <code>DEVDESK_DATA_ROOT</code> 覆盖</td></tr>
</table>

---

<h2 align="center"><a name="项目状态"></a>📌 项目状态</h2>

<p align="center">
六个模块全功能运行，可经设置页自由插拔<br>
全量测试 <b>847</b> 条，<b>836</b> 通过 —— 其余 3 条是<b>已知的平台不对称红</b><br>
<sub>（macOS 侧 FileWatcher 为轮询兜底 ×2、keychain 空账号语义 ×1），非回归</sub>
</p>

<h4 align="center">平台支持</h4>

<table align="center">
<tr><th align="left">平台</th><th align="left">状态</th><th align="left">说明</th></tr>
<tr><td><b>macOS 27+</b> (arm64)</td><td>✅ 主开发平台</td><td>窗口效果三档（关闭 / 半透 / 磨砂）走原生材质</td></tr>
<tr><td><b>Windows 11</b></td><td>✅ 移植目标</td><td>单 exe 零外部 DLL；窗口效果走 DWM</td></tr>
</table>

---

<h2 align="center"><a name="文档"></a>📚 文档</h2>

<table align="center">
<tr><th align="left">文件</th><th align="left">内容</th></tr>
<tr><td><a href="CLAUDE.md"><code>CLAUDE.md</code></a></td><td>开发指南：工具链、构建约定、架构约束、协作纪律</td></tr>
<tr><td><a href="docs/superpowers/specs/"><code>docs/superpowers/specs/</code></a></td><td>设计与需求 spec（权威文档）</td></tr>
<tr><td><a href="docs/USER_GUIDE.md"><code>docs/USER_GUIDE.md</code></a></td><td>使用说明</td></tr>
<tr><td><a href="docs/KNOWN_LIMITATIONS.md"><code>docs/KNOWN_LIMITATIONS.md</code></a></td><td>已知限制</td></tr>
</table>

---

<div align="center">
<sub>私有项目，保留所有权利。</sub>
</div>
