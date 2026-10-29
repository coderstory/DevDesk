# ccache 编译缓存设置指南（Windows + macOS）

> 目标：让**另一台电脑**（Windows 或 macOS）照着本文重新实现 DevDesk 的编译缓存，
> 达到「新 worktree 首建只需约 35 秒」（本机实测：冷缓存 647s → 热缓存 35s）。
> 本文内容均来自本机（Windows 11 + MSYS2 MinGW64，ccache 4.14）实测配置，macOS 部分按 ccache 官方文档 + 平台惯例写出。

## 为什么需要 ccache

DevDesk 用 git worktree 并行开发（`wt-*` / `.claude/worktrees/*`），每个 worktree 都是一套独立的 `build/`。
没有编译缓存时，每个新 worktree 都要全量重编一遍（本机实测约 11 分钟）；
ccache 把「预处理后的编译产物」按源码内容哈希后存到**持久盘**，让所有 worktree 共享同一份缓存，
新 worktree 首建只需要验证哈希命中（本机实测约 35 秒，命中率 99.2%）。

---

## Windows

### 1. 安装 ccache

主路径（本机所用）——MSYS2：

```bash
pacman -S mingw-w64-x86_64-ccache
```

装到 `/c/msys64/mingw64/bin/ccache.exe`（该目录已在编译 PATH 里，`command -v ccache` 直接可用）。

备选：`winget install ccache.ccache`，或从官方 GitHub Releases 下载 exe 放入 PATH。
只要 `command -v ccache` 能命中即可 —— DevDesk 的构建脚本会自动检测（见第 4 节）。
本机版本：ccache 4.14。

### 2. 三件套配置（核心，缺一不可）

不设任何 `CCACHE_*` 环境变量也要能跨 worktree 命中，靠的是下面这三条**持久配置**：

| 配置项 | 含义 | 不设的后果 |
|--------|------|-----------|
| `cache_dir = D:/software/ccache-devdesk` | 缓存存放的**持久盘**路径 | 缓存落默认目录，重装/清理可能丢失 |
| `base_dir = D:/project/gui` | 哈希前把 worktree 绝对路径改写成「相对当前目录」 | **白装**：每个 worktree 绝对路径不同 → 同一份源码哈希不同 → 永远 miss |
| `hash_dir = false` | 命令哈希不含工作目录 | 当前目录参与哈希，worktree 目录名一变又 miss |
| `max_size = 20G` | 缓存上限（按磁盘容量调整） | — |

三条要点：

- **`cache_dir` 绝不放 `build/` 里** —— worktree 用完即删，缓存会被一起删掉；
  放持久盘（如 `D:/software/`）才在所有 worktree 生命周期之外存活。
- **`base_dir` 必须是「worktree / DevDesk」与「EUI-NEO」的公共父目录**（本机 = `D:/project/gui`），
  且用 Windows 风格路径写（ccache 是原生 exe，不认 `/d/...` 这种 MSYS 路径）。
  注意：`-g` 调试信息里的绝对路径也会随 base_dir 一起改写。
- 环境变量优先级高于配置文件。平时不需要任何 `CCACHE_*`；
  需要覆盖时（换缓存盘 / 换基准目录 / 临时关缓存）才 `export`：
  `CCACHE_DIR` ↔ `cache_dir`、`CCACHE_BASEDIR` ↔ `base_dir`、`CCACHE_NOHASHDIR` ↔ `hash_dir`、
  `CCACHE_MAXSIZE` ↔ `max_size`。仓库里的 `scripts/ccache-env.sh` 就是干这个的，平时不用 source。

### 3. 两份配置文件（为什么是两份）

ccache 的配置按「`CCACHE_DIR` 是否被显式设置」走不同的读取路径，
所以**同一套三件套要写在两个文件里**：

**文件一：`%LOCALAPPDATA%\ccache\ccache.conf`**（ccache 的「默认缓存目录」下的配置）

```ini
cache_dir = D:/software/ccache-devdesk
base_dir = D:/project/gui
hash_dir = false
max_size = 20G
```

`CCACHE_DIR` **未设**时，ccache 先读默认缓存目录下的这份配置 → 所以 `cache_dir` 必须写在这里才生效
（写在缓存目录里那份的话，未设 `CCACHE_DIR` 时根本读不到）。

**文件二：`D:/software/ccache-devdesk/ccache.conf`**（`cache_dir` 指向的缓存目录里自带一份）

```ini
base_dir = D:/project/gui
hash_dir = false
max_size = 20G
```

当 `CCACHE_DIR` 被**显式**设成缓存目录（`export CCACHE_DIR=...` 或 source `scripts/ccache-env.sh`）时，
ccache 读的是这份而不是 `%LOCALAPPDATA%\ccache\ccache.conf` —— 所以三件套要在这里复写一份。
**不写 `cache_dir`**（自指无意义）。

一句话记法：**默认场景读 A 份（含 cache_dir 指向 B），显式设 CCACHE_DIR 时读 B 份（内容与 A 相同但不含 cache_dir）**。
验证：`ccache -p` 可看每个配置值的实际来源（本机实测 = `...\ccache.conf` → base_dir / hash_dir 均生效）。

### 4. CMake 接入

configure 时传 launcher 参数：

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DOPENSSL_ROOT_DIR=C:/msys64/mingw64 \
  -DZLIB_LIBRARY=C:/msys64/mingw64/lib/libz.dll.a \
  -DZLIB_INCLUDE_DIR=EUI-NEO/3rd/zlib-1.3.1
```

两条注意：

- launcher 是 **configure 期属性**：已存在的 `build/` 不会自动接上。
  要么删 `build/` 重配（`--clean`），要么手敲一次带 launcher 的 configure。
- 仓库脚本 `scripts/build-test-run.sh` **已自动带这两个参数**（构建路径统一走它）：
  内部 `if command -v ccache` 检测，未安装 ccache 时自动降级、不传 launcher，行为与从前完全一致（不硬失败）。

### 5. 并行度封顶

`DEVDESK_JOBS=3`（默认值）→ `export CMAKE_BUILD_PARALLEL_LEVEL="${DEVDESK_JOBS:-3}"`。
多 worktree 并发构建时避免互相抢满 CPU/内存（本机 12 核，全占反而更慢），单条 build 也更稳。

### 6. 实测数据（本机）

- 环境：12 核 / `DEVDESK_JOBS=3` / 两个全新 worktree 首建
- 冷缓存（ccache 统计清零后）：**647 秒**
- 热缓存：**35 秒**——239 次查找，237 命中 = **99.2%**，全 direct 命中、**零预处理**（直接复用，连预处理都不重跑）

---

## macOS

与 Windows 方法论完全相同，只有工具链与路径习惯不同。**ccache 4.x 支持 AppleClang**。

### 1. 安装 ccache

```bash
brew install ccache
```

装到 `/opt/homebrew/bin/ccache`（Apple Silicon）或 `/usr/local/bin/ccache`（Intel）。

### 2. 三件套配置（语义同 Windows）

`~/.ccache/ccache.conf`（macOS 下 ccache 默认读 `$HOME/.ccache/ccache.conf`）：

```ini
cache_dir = /Users/<user>/Library/Caches/ccache
base_dir = /Users/<user>/project/gui
hash_dir = false
max_size = 20G
```

- `cache_dir`：macOS 习惯放 `~/Library/Caches/`（系统语义就是「可重建的缓存」）；同样**不要放 `build/` 里**。
- `base_dir`：按实际项目根写（把 Windows 例子的 `D:/project/gui` 换成 macOS 的对应路径），
  必须是 DevDesk 与 EUI-NEO 的公共父目录。
- `hash_dir = false`、`max_size = 20G`：同 Windows。

### 3. 配置文件位置（同 Windows 双份逻辑）

- `~/.ccache/ccache.conf`：默认读取位置，`cache_dir` 写在这里。
- `<cache_dir>/ccache.conf`（如 `~/Library/Caches/ccache/ccache.conf`）：显式设了 `CCACHE_DIR` 时读这份，
  内容同上去掉 `cache_dir` 一行。

### 4. CMake 接入

与 Windows 完全相同：

```bash
cmake -B build -G Ninja \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache ...
```

### 5. AppleClang 注意

- ccache 4.x+ 支持 AppleClang（旧版本对 AppleClang 支持很差，务必 `ccache --version` 确认 ≥ 4.x）。
- `compiler_check`（编译器哈希校验方式）：`content` 更精确但每次要读编译器文件；
  Apple 系机器上可考虑 `compiler_check = mtime`（靠修改时间，更快）。
  日常开发编译器不变时两者命中率相同，按需取舍。

### 6. 跨平台共享缓存

**Windows 与 macOS 的编译产物不能共享缓存**——编译器不同（MinGW g++ vs AppleClang）、系统头文件不同，
同一份源码的哈希也映射到不同的产物，混用只会互相污染（甚至因格式不兼容产生误命中风险）。
两台机器各自建自己的缓存即可；**配置方法论完全相同**，照着本文各自配一遍。

---

## 验证

```bash
ccache -s            # 看命中率：Cacheable calls / Hits，命中 > 80% = 健康
ccache -z            # 清零统计（可选，用于干净测量）
```

验收标准：

- **新 worktree 首建从 ~11 分钟降到 ~35 秒** = 缓存生效（这是最直观的端到端验证）。
- `ccache -s` 里本机健康值参考：239 次查找 / 237 命中（99.2%），零预处理、全 direct 命中。
- 跨 worktree 验证法：`git worktree add <新树> master` 后在**新树**里跑一次构建脚本，观察耗时骤降。

---

## 常见坑

| 坑 | 症状 | 解法 |
|----|------|------|
| 不设 `base_dir`（或设错基准） | `ccache -s` 命中率长期 < 10%，新 worktree 仍是满编 | `base_dir` 指向 DevDesk 与 EUI-NEO 的公共父目录；`ccache -p` 核对生效值 |
| 缓存放 `build/` 里 | worktree 清理后缓存被一并删除，下次重建全 miss | `cache_dir` 放持久盘（如 `D:/software/`、`~/Library/Caches/`） |
| 已存在 `build/` 不自动接 launcher | 缓存毫无累积，构建时间没变化 | launcher 是 configure 期属性：`--clean` 重配，或手敲一次带 launcher 的 configure |
| 用 ccache 官方 masquerade 方式 | MSYS2 自带 `/mingw64/lib/ccache/bin` 是 **bash 脚本**，Git Bash 下可能指错编译器，行为诡异 | **不用 masquerade**，统一用 CMake launcher 参数（`-DCMAKE_*_COMPILER_LAUNCHER=ccache`） |
| Windows 上 `base_dir` 写成 MSYS 路径（`/d/...`） | ccache 不认，改写不生效 | 用 Windows 风格：`D:/project/gui` |