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

当用户在 Finder 双击 `.md` 文件时，macOS 调用：
```bash
/Applications/DevDesk.app/Contents/MacOS/DevDesk /path/to/file.md
```

EUI-NEO 的 `main()` 不接收 `argc/argv`，所以我们用 Apple 私有但长期稳定的 API `_NSGetArgv()` 在 `dslAppConfig()` 首次调用时读取 `argv[1]`。

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

## 生产环境分发（可选）

个人项目无需代码签名。但要正式分发到其他 Mac，需要：

### 1. 代码签名（ad-hoc 或 Developer ID）

```bash
# Ad-hoc（个人测试，仅本机）
codesign --deep --sign - DevDesk.app

# Developer ID（Gatekeeper 信任，需要 Apple Developer 账号）
codesign --deep --sign "Developer ID Application: Your Name (TEAMID)" DevDesk.app

# 验证签名
codesign -dv DevDesk.app
```

### 2. 公证（Notarization）

```bash
# 打包 zip
ditto -c -k --keepParent DevDesk.app DevDesk.zip

# 上传 Apple 公证服务
xcrun notarytool submit DevDesk.zip \
    --apple-id "you@example.com" \
    --team-id "TEAMID" \
    --password "app-specific-password"

# Staple 公证票据
xcrun stapler staple DevDesk.app
```

### 3. DMG 打包

```bash
hdiutil create -volname DevDesk -srcfolder DevDesk.app -ov -format UDZO DevDesk.dmg
```

详细流程参见 Apple 官方文档 [Distributing your Mac apps](https://developer.apple.com/distribute/)。

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
