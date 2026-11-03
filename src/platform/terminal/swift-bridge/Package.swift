// swift-tools-version:6.2
import PackageDescription

// DevDesk 自建 SwiftPM 桥接包：path-depend 上游核心包（third_party/Corta/CortaTerminal，submodule 只读）。
// 唯一职责：把 CortaTerminal 的 Swift 公开面翻译成 C++ interop 边界上的 POD + 会话类，零终端逻辑。
let package = Package(
    name: "CortaBridge",
    platforms: [.macOS("26.0")],
    products: [
        .library(name: "CortaBridge", targets: ["CortaBridge"])
    ],
    dependencies: [
        // 相对路径：swift-bridge/ → src/platform/terminal → platform → src → 仓库根
        .package(path: "../../../../third_party/Corta/CortaTerminal")
    ],
    targets: [
        .target(
            name: "CortaBridge",
            dependencies: [
                .product(name: "CortaTerminal", package: "CortaTerminal"),
                // 审计 P1-E：显式声明对上游 executable product 的依赖，使同一条 swift build
                // 一并构建 corta-exec（Spawn 经 dladdr 宿主二进制路径向上寻址，须随包部署）。
                .product(name: "corta-exec", package: "CortaTerminal")
            ],
            path: "Sources/CortaBridge"
        ),
        .testTarget(
            name: "CortaBridgeTests",
            dependencies: ["CortaBridge"],
            path: "Tests/CortaBridgeTests"
        )
    ]
)
