# 第三方组件告示（THIRD-PARTY NOTICES）

本仓库打包分发的第三方组件及其许可、来源。凡随包分发的二进制必须在此登记一条；
新增条目沿用下面的字段格式。

## mpv（Windows 播放后端）

- 用途：壁纸模块视频/图片播放后端（`assets/mpv/mpv.exe`，随安装包分发）
- 构建来源：shinchiro 构建（mpv-winbuild-cmake）
  - 构建仓库：<https://github.com/shinchiro/mpv-winbuild-cmake>
- 上游项目主页：<https://mpv.io>
- 上游源码：<https://github.com/mpv-player/mpv>
- 许可证：GPLv2+（GNU General Public License 第 2 版或更高，见
  <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html>）
- 打包入口：`scripts/package-windows.sh`（`MPV_BIN` 拷入 `assets/mpv/mpv.exe`；
  运行时解析见 `src/module_wallpaper/platform/mpv_controller.h` 的
  `resolveMpvExecutable`）

## libvterm（Windows 终端 VT 解析）

- 用途：终端模块 Windows 后端的 VT220/xterm/ECMA-48 序列解析（vendored 静态链入
  主程序，无独立文件分发）
- 上游源码：<https://github.com/neovim/libvterm>（nvim 分支，锁 commit `934bc2f`；
  上游 leonerd v0.3.3 系）
- 许可证：MIT
- 引入方式：git submodule `third_party/libvterm` + 仓内 CMake 胶水
  （根 `CMakeLists.txt`；submodule 零写入）

## ConPTY v2 redist（Windows 终端伪控制台宿主）

- 用途：终端模块 Windows 后端的 pinned 伪控制台（`conpty.dll` + `OpenConsole.exe`
  成对随包，落安装目录 `conpty\`；运行时三级查找，不可用回落系统 inbox ConPTY）
- 上游源码：<https://github.com/microsoft/terminal>（ConPTY 组件）
- 版本 pin：随 node-pty 的 pinned 副本（研究核证时点 1.25.260303002）；确切版本与
  SHA256 以 `third_party/conpty/README.md` 回填表为准
- 获取渠道：node-pty 树内 `third_party/conpty/`（主）/ NuGet 包
  `Microsoft.Windows.Console.ConPTY`（备）
- 许可证：MIT
- 引入方式：手工放包 `third_party/conpty/`（版本 pin 靠仓内二进制钉死，构建/打包/
  装机链拷至 exe 旁 `conpty\`；发布形态守卫白名单覆盖）

## Noto Sans CJK SC（子集随包，UI 正文字体）

- 用途：跨平台 UI 正文字体统一（need:132，`assets/DevDeskSansSC-Regular.otf` +
  `assets/DevDeskSansSC-Bold.otf`，family 改名 `DevDesk Sans SC`）
- 上游项目：<https://github.com/notofonts/noto-cjk>（release Sans2.004，
  `08_NotoSansCJKsc.zip`）
- 许可证：SIL Open Font License 1.1，全文随包 `assets/OFL-NotoSansCJK.txt`
- 修改声明：子集化（GB2312 全集+源码码点，7150 码点）+ 改名避开 OFL Reserved
  Font Name，详见随包 `assets/FONT-MODIFICATIONS.txt` 与
  `scripts/make_font_subset.py`

## JetBrains Mono（原文件随包，等宽字体）

- 用途：终端 / 代码块等宽字体统一（need:132，`assets/JetBrainsMono-Regular.ttf`，
  原文件捆绑未修改）
- 上游项目：<https://github.com/JetBrains/JetBrainsMono>（v2.304）
- 许可证：SIL Open Font License 1.1，全文随包 `assets/OFL-JetBrainsMono.txt`
- 修改声明：无修改（Original Version），说明见随包 `assets/FONT-MODIFICATIONS.txt`

## llama.cpp（macOS 记忆模块本地推理引擎）

- 用途：记忆模块本地 embedding 推理引擎（`assets/llama/bin/llama-server` 及其依赖
  闭包随 `.app` 分发，bug:225；运行时定位序见
  `src/module_memory/engine/llama_host.cpp` 的 `locateLlamaServerBetween`）
- 构建来源：打包机 Homebrew 构建的 `llama-server`（`brew install llama.cpp`），
  打包装机整体拷入并重写 install name
- 上游项目主页：<https://github.com/ggml-org/llama.cpp>
- 版本 pin：0.9.0（build 12110，commit `cd959bae9`，见
  `evidence/llama-bundle/isolation-run.txt`）
- 许可证：MIT（版权人 The ggml authors；分发义务为保留版权声明与许可文本）
- 打包入口：`scripts/package-macos.sh` 的 llama 段（依赖闭包 fixpoint 拷入
  `Contents/MacOS/assets/llama/bin/`，全部非系统 install name 重写为
  `@loader_path/<basename>`）

## ggml（llama.cpp 张量计算底层，随其依赖闭包分发）

- 用途：llama-server 的 ggml 张量计算与后端库（`assets/llama/bin/libggml.0.dylib` +
  `libggml-base.0.dylib`，随 llama.cpp 依赖闭包分发）
- 上游项目：<https://github.com/ggml-org/ggml>（与 llama.cpp 同组织、同版权人
  The ggml authors）
- 版本 pin：0.29.0（包内 `libggml*.dylib` current version 0.29.0，见
  `evidence/llama-bundle/otool-closure.txt`）
- 许可证：MIT（⚠ 待补：仓库内尚无 vendored LICENSE 副本，许可名称依据打包机同版本
  组件自带 LICENSE；下次打包时补登原文）
- 引入方式：随 `scripts/package-macos.sh` 的 llama 依赖闭包拷入，无独立引入步骤

## OpenSSL（TLS / 加密，随 llama 闭包分发 + Windows 静态链入）

- 用途：TLS 与加密原语。macOS 包内 `assets/llama/bin/libssl.4.dylib` +
  `libcrypto.4.dylib`（llama-server 依赖闭包）；Windows 侧静态链入主程序，
  无独立文件分发
- 上游项目主页：<https://www.openssl.org>
- 上游源码：<https://github.com/openssl/openssl>
- 版本 pin：4.1.0（包内 `libcrypto.4.dylib` 内嵌版本串 `OpenSSL 4.1.0 23 Oct 2026`）；
  Windows 侧版本由 configure 解析 `opensslv.h` 后随构建日志打印，见
  `cmake/Dependencies.cmake`（`OPENSSL_VERSION` 段）
- 许可证：Apache-2.0（⚠ 待补：仓库内尚无 vendored LICENSE 副本，许可名称依据打包机
  同版本组件自带 `LICENSE.txt`；下次打包时补登原文）
- 引入方式：macOS 随 `scripts/package-macos.sh` 的 llama 依赖闭包拷入；Windows 由
  `find_package(OpenSSL REQUIRED)` 静态链入（`DEVDESK_STATIC`）

## libomp（LLVM OpenMP 运行时，随 llama 闭包分发）

- 用途：llama-server/ggml 的 OpenMP 并行运行时（`assets/llama/bin/libomp.dylib`，
  经 `libggml-base.0.dylib` 依赖链随包）
- 上游项目：<https://github.com/llvm/llvm-project>（openmp 运行时，LLVM Project）
- 版本 pin：随打包机 Homebrew `libomp`（24 系列之前为 23.x 线；包内
  `libomp.dylib` current version 5.0.0 = OpenMP 运行时兼容版本，见
  `evidence/llama-bundle/otool-closure.txt`；⚠ 待补：LLVM 版本号在仓库内无台账副本，
  下次打包时回填确切版本）
- 许可证：Apache-2.0 WITH LLVM-exception（LLVM Project 标准条款；⚠ 待补：仓库内尚无
  vendored LICENSE 副本，许可名称依据打包机同版本组件自带 `LICENSE.TXT`）
- 引入方式：随 `scripts/package-macos.sh` 的 llama 依赖闭包拷入，无独立引入步骤

## bge-small-zh-v1.5（记忆模块本地 embedding 模型，GGUF Q8_0）

- 用途：记忆模块的本地向量化模型（`assets/models/bge-small-zh-v1.5-q8_0.gguf`，
  26,472,640 字节，SHA-256 `5a88d266870fbd27c6f329df60de80e2d4cf3bbd5e6f080bd5c1b2e5abb12039`，
  随包分发，need:241；运行时定位序见 `src/module_memory/engine/llama_host.cpp` 的
  `locateEmbedModelBetween`：exe 旁 `assets/models` → 数据根 → HF 下载兜底）
- 上游模型主页：<https://huggingface.co/BAAI/bge-small-zh-v1.5>（BAAI，原模型）
- GGUF 量化发布方：<https://huggingface.co/CompendiumLabs/bge-small-zh-v1.5-gguf>
  （第三方转换，非 BAAI 官方；模型卡自述 `Source model:` 指向上游 BAAI）
- 版本 pin：Q8_0 量化，`bge-small-zh-v1.5-q8_0.gguf`（512 维，索引维度在建索引前定死）
- 许可证：MIT（实查 2026-11-08：两份模型卡 YAML 均为 `license: mit`——
  `https://huggingface.co/BAAI/bge-small-zh-v1.5/raw/main/README.md` 首行与
  `https://huggingface.co/CompendiumLabs/bge-small-zh-v1.5-gguf/raw/main/README.md` 首行；
  BAAI 卡正文另述 FlagEmbedding 为 MIT 且「released models can be used for commercial
  purposes free of charge」。⚠ 待补：仓库内尚无 vendored LICENSE 原文副本）
- 引入方式：仓内 `assets/models/`（`.gitignore:124` 白名单）随 assets 分发；
  macOS 由 `scripts/package-macos.sh` 的 embedding 模型段拷入 `Contents/MacOS/assets/models/`；
  Windows 由 `scripts/unpack-memory-assets.sh` 从 `assets/memory-embedding.7z` 解出，
  `scripts/package-windows.sh` 另做单文件兜底
