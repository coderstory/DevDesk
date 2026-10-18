# DevDesk macOS .app Bundle & 文件关联

本文档说明 DevDesk 的 macOS `.app` bundle 结构、文件关联机制，以及开发期重新打包流程。

## 为什么需要 `.app` bundle

DevDesk 当前以裸 ELF 二进制（`build/DevDesk`）形式存在。要注册文件关联、显示在 Finder「打开方式」菜单中、被 LaunchServices 识别为正式应用，必须打包成 macOS 标准 `.app` bundle 结构。

---

## `.app` bundle 结构

`build/DevDesk.app/` 由 `platform/macos/build-app.sh` 生成：

```
DevDesk.app/
└── Contents/
    ├── Info.plist           # 应用元数据 + 文件关联声明（CFBundleDocumentTypes）
    ├── MacOS/
    │   └── DevDesk          # 主可执行文件
    └── Resources/           # 字体、图标等资源（占位，预留给后续）
```

`Info.plist` 的关键字段：

| 字段 | 值 | 说明 |
|------|------|------|
| `CFBundleIdentifier` | `com.devdesk.app` | 全局唯一 bundle ID（macOS 用来标识应用） |
| `CFBundleExecutable` | `DevDesk` | MacOS/ 下的可执行文件名 |
| `CFBundlePackageType` | `APPL` | 标准应用类型 |
| `LSMinimumSystemVersion` | `11.0` | macOS Big Sur 及以上（兼容 BSM Tahoe） |
| `NSHighResolutionCapable` | `true` | 支持 HiDPI / Retina |
| `CFBundleDocumentTypes` | 见下方 | 注册的文件关联（5 种类型） |

---

## 文件关联机制（CFBundleDocumentTypes）

`Info.plist` 的 `CFBundleDocumentTypes` 数组声明 DevDesk 能打开哪些类型的文件。每个 dict 描述一种文档类型：

```xml
<key>CFBundleDocumentTypes</key>
<array>
    <dict>
        <key>CFBundleTypeName</key>      <string>Markdown Document</string>
        <key>CFBundleTypeRole</key>      <string>Editor</string>      <!-- 我们是编辑器 -->
        <key>LSHandlerRank</key>         <string>Alternate</string>   <!-- 不抢默认 -->
        <key>LSItemContentTypes</key>
        <array>
            <string>net.daringfireball.markdown</string>  <!-- 系统已有 UTI -->
            <string>public.plain-text</string>            <!-- fallback -->
        </array>
    </dict>
    <!-- ... 其他 4 种类型 ... -->
</array>
```

### 已注册的 5 种文件类型

| 文件类型 | UTI | 扩展名 |
|---------|-----|--------|
| Markdown | `net.daringfireball.markdown` / `public.plain-text` | `.md`, `.markdown` |
| JSON | `public.json` | `.json` |
| JavaScript | `com.netscape.javascript-source` | `.js` |
| HTML | `public.html` | `.html`, `.htm` |
| Text | `public.plain-text` / `public.text` | `.txt` |

### `LSHandlerRank = Alternate` 的含义

- **`Owner`** — 独占该类型（其他人不能注册同类）
- **`Default`** — 默认 handler（其他可作 Alternate）
- **`Alternate`** — 备选 handler（用户必须手动选择 "打开方式 → 其他 → DevDesk"）✓ 我们用这个
- **`None`** — 不作为 handler（仅声明支持）

我们选 `Alternate` 因为：
1. 不抢用户已有偏好（VSCode / Typora / 文本编辑 等都是用户常用）
2. 用户可以主动在 Finder 右键 → 「打开方式」里找到 DevDesk
3. 适合个人 / 实验性应用

如果未来要做正式分发，建议改成 `Default` + 用 codesign + 公证。

---

## 开发期重新打包

### 方式 A：一键脚本（推荐）

```bash
# 1. 编译二进制
cmake --build build

# 2. 打包 .app 并注册到 LaunchServices
./platform/macos/build-app.sh

# 输出:
#   ✅ App bundle: /Users/coderstory/Dev/gui/DevDesk/build/DevDesk.app
#   ✅ 注册到 LaunchServices 完成
```

`build-app.sh` 会自动调用 `lsregister -f` 把 bundle 注册到 LaunchServices 数据库。Finder 立即就能识别文件关联（无需重启 / 注销）。

### 方式 B：CMake target

```bash
cmake --build build --target devdesk_app
```

这个 target 会先构建 `DevDesk` 二进制，再调用 `build-app.sh`。仅 macOS 平台生效。

### 验证文件关联

```bash
# 1. 启动 .app
open build/DevDesk.app

# 2. 在 Finder 找一个 .md 文件
#    右键 → "打开方式" → 应能看到 DevDesk

# 3. 命令行查询 LS 数据库
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister \
    -dump | grep -A 5 "com.devdesk.app"

# 4. 查看默认 handler
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister \
    -dump | grep -B 2 -A 10 "net.daringfireball.markdown"
```

---

## 文件参数传递（argv → Router）

当通过命令行启动应用并带文件参数时，macOS 调用形式为：

```bash
/Applications/DevDesk.app/Contents/MacOS/DevDesk /path/to/file.md
```

（Finder 双击不走 argv，走 Apple Event —— 见下方「两条打开路径」）

EUI-NEO 的 `main()` 不接收 `argc/argv`，所以我们用 Apple 私有但长期稳定的 API `_NSGetArgv()` 在 `dslAppConfig()` 首次调用时读取 `argv[1]`。

> ⚠️ **签名陷阱**：`_NSGetArgc()` 返回 `int*`，`_NSGetArgv()` 返回 `char***` —— 它们返回的是 argc/argv 的**地址**，必须解引用一次：
> ```cpp
> extern "C" int* _NSGetArgc(void);
> extern "C" char*** _NSGetArgv(void);
> int argc = *_NSGetArgc();
> char** argv = *_NSGetArgv();
> ```
> 若误写成 `int argc = _NSGetArgc();`，拿到的会是地址的低 32 位（垃圾值），导致 `argc < 2` 误判而静默跳过。

> ⚠️ **顺序要求**：`captureOpenFileFromArgv()` 内部有 `LOG_INFO`，必须放在 `setupLogging()` **之后**调用，否则日志输出尚未注册、捕获日志会丢失。

完整流程在 `src/main.cpp`：

```cpp
// 1. 捕获 argv[1]（仅第一次）
captureOpenFileFromArgv();  // → pendingOpenFilePath()

// 2. Router 启动后，根据扩展名 dispatch 到对应 Page
//    .md/.markdown → Markdown
//    .json         → ClaudeProxy
//    .js/.html/.htm/.txt → Markdown（用 Markdown 模块查看文本）
//    其他 → Home
::core::PageId dispatchPageForFile(const std::string& path);
router.navigateTo(target);
```

### 当前 MVP 状态

- ✅ argv[1] 路径读取 + 写入 LOG
- ✅ 根据扩展名切换 PageId
- ❌ **不实际加载文件内容** — 留给后续 P 阶段实现（让对应模块的 `onStart()` 读 `pendingOpenFilePath()` 并渲染）

### ⚠️ 两条打开路径（重要）

macOS 传递文件给应用有**两条完全不同的路径**：

| 触发方式 | 传递机制 | 当前是否生效 |
|---------|---------|------------|
| `DevDesk.app/Contents/MacOS/DevDesk file.md`（命令行直接调用二进制） | **argv** | ✅ 已生效（本文档所述实现） |
| Finder 双击 / 右键「打开方式」/ `open -a DevDesk file.md` | **Apple Event**（`kAEOpenDocuments`） | ❌ 尚未生效 |

Finder 路径走的是 Apple Event，最终落到 `NSApplication` 的 `application:openFile:` 代理方法 —— EUI-NEO 没有注册 NSApplication delegate，所以事件被静默丢弃（实测 `open -a DevDesk file.md` 后日志无捕获记录）。

**后续方案（推荐按 CLAUDE.md 上游政策处理）**：在 EUI-NEO 加一个轻量 ObjC shim 注册 `NSApplicationDelegate`，把 Apple Event 转成一个 C 回调（或落盘到临时文件让应用轮询），DevDesk 侧消费该回调拿到文件路径。这属于"通用 UI 能力缺失"，适合反哺 EUI-NEO 上游。

当前 MVP 下，Finder 里「打开方式」菜单**能看到 DevDesk**（LaunchServices 注册已生效），但双击文件还不会加载内容。

### P 阶段实现路径

```cpp
// Markdown 模块 onStart()
void MarkdownModule::onStart() {
    auto path = core::ModuleInterface::pendingOpenFilePath();
    if (!path.empty() && isMarkdownFile(path)) {
        loadFile(path);  // 读文件 + 渲染
    }
}
```

---

## 代码签名与分发

### 1. 本机开发 / 个人分发：ad-hoc 签名（已接入）

DevDesk 的 `.app` 必须打 ad-hoc 签名：链接器自动生成的签名与 `Info.plist` 的
`CFBundleIdentifier` 不一致（`Identifier=DevDesk`、`Info.plist=not bound`），
`codesign --verify --strict` 会直接失败；系统通知等按 bundle 身份识别应用的能力也依赖它。

统一入口：

```bash
bash scripts/sign-macos-app.sh build/DevDesk.app
```

脚本实际执行：

```bash
codesign --force --sign - --identifier com.devdesk.app <path-to-.app>
codesign --verify --strict --verbose=4 <path-to-.app>
```

- 固定 `--identifier com.devdesk.app`，与 `Info.plist` 的 `CFBundleIdentifier` 一致。
- **不加 `--deep`**：当前 bundle 没有嵌套 framework/dylib，只签 bundle 根；Apple 也不推荐
  递归签名，将来真引入框架时应逐个显式签。
- **不加 `--options runtime`**：hardened runtime 的 library validation 会拒绝加载 bundle 外、
  未签名的动态库。本机依赖 Homebrew OpenSSL（如
  `/opt/homebrew/opt/openssl@3/lib/libssl.3.dylib`、`libcrypto.3.dylib`），加上 hardened
  runtime 会让 app 启动即失败。本机 ad-hoc 自用不做硬化运行时/公证。

**签名必须是 bundle 的最后一次写入**：`codesign` 会把 Info.plist、Resources、assets 等
全部封进 `_CodeSignature/CodeResources`；签名后再改 Info.plist 版本号、换图标或补拷
assets，封条就会失效，`codesign --verify --strict` 随即报错。三条路径已按此顺序接入：

| 路径 | 签名位置 |
|---|---|
| `platform/macos/build-app.sh` | Info.plist 版本、图标、assets 全部就位之后，`lsregister` 之前；调用方若还要补拷资源可置 `DEVDESK_SKIP_SIGN=1`，自行在最后一次写入后签 |
| `scripts/package-macos.sh` | 构建树内补拷 assets 之后重签；拷进 DMG 暂存目录后再对副本签一次 |
| `scripts/build-test-run.sh --install` | 补拷 assets、版本/二进制断言之后、`ditto` 到 `/Applications` 之前；装完再 `codesign --verify --strict` |

验证：

```bash
codesign --verify --strict --verbose=4 build/DevDesk.app
codesign -dv --verbose=4 build/DevDesk.app 2>&1 | grep '^Identifier='
# 期望：Identifier=com.devdesk.app
```

ad-hoc 签名**不满足 Gatekeeper**（`spctl` 仍会拒绝，首次运行仍需去隔离）；它只保证
bundle 身份自洽。每次重建签名都会变，macOS 可能因此重复询问通知/钥匙串授权，属已知限制。

### 2. 打包 DMG

```bash
bash scripts/package-macos.sh
```

脚本内部会重新组装 `.app`、补拷 assets、签名，再对 DMG 暂存副本签一次；不要手敲
`hdiutil` 绕过它。

### 3. 正式分发（可选，当前未接入）

要把 app 分发给其他 Mac，需要 Apple Developer ID 签名 + 公证；并且必须先把 Homebrew
OpenSSL 等第三方动态库收进 `.app` 内逐个签名，否则 hardened runtime 的 library
validation 会拒绝加载。本仓库当前只实现 ad-hoc 路径，正式分发前需另做打包工程；
流程见 Apple 官方文档 [Distributing your Mac apps](https://developer.apple.com/distribute/)。

---

## 已知限制

1. **macOS 11.0+** —— 低于此版本的 macOS 不支持当前 bundle 结构
2. **Apple Silicon vs Intel** —— 当前未区分 architecture；如需 universal binary：
   ```bash
   lipo -create -arch arm64 DevDesk-arm64 -arch x86_64 DevDesk-x86_64 -output DevDesk
   ```
3. **图标资源** —— 当前 `Resources/` 目录为空，未提供 `.icns` 图标（Finder 会用默认图标）
4. **沙箱化（Sandbox）** —— 当前 bundle 未声明 entitlements；如要提交 Mac App Store，需要 sandbox + entitlements

---

## 相关文件

| 文件 | 作用 |
|------|------|
| `platform/macos/Info.plist` | Bundle 元数据 + 文件关联声明 |
| `platform/macos/build-app.sh` | 打包脚本 + LaunchServices 注册 |
| `src/main.cpp` | argv 捕获 + PageId dispatch（MVP） |
| `CMakeLists.txt` | `devdesk_app` target |

---

## 参考资料

- Apple Developer — [Information Property List](https://developer.apple.com/documentation/bundleresources/information_property_list)
- Apple Developer — [CFBundleDocumentTypes](https://developer.apple.com/documentation/bundleresources/information_property_list/cfbundledocumenttypes)
- Apple Developer — [Uniform Type Identifier](https://developer.apple.com/documentation/uniformtypeidentifiers)
- [macOS File Associations: The Right Way](https://cocoacasts.com/macos-development-essentials-how-to-register-a-file-association-on-macos)
