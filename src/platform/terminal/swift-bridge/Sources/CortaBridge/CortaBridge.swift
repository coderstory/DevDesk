// CortaBridge — DevDesk 自建桥接门面（Phase 0 骨架，Phase 1 直接续用）。
// 职责边界：只做类型翻译与线程编排（技术卡 §5.1），零终端逻辑；解析/网格/回滚/选区文本全留核心。
import AppKit
import CortaPOSIX
import CortaTerminal
import Dispatch
import Foundation

/// 边界 POD：单 cell。C++ 侧镜像同布局 struct（swift_bridge.h）。
/// fg/bg 透传核心 Color.rawValue（kind 位+payload，Phase 1 在 widget 侧映射主题/ANSI）；
/// attrs 透传核心 CellAttributes.rawValue。
public struct BridgeCell {
    public var cp: UInt32 = 0    // 首 Unicode scalar；0=空 cell
    public var fg: UInt32 = 0
    public var bg: UInt32 = 0
    public var attrs: UInt16 = 0
    public var width: UInt8 = 1  // 1=半角 2=宽字（grapheme 占位近似，Phase 1 用 CharacterWidth 精化）
    public init() {}
}

/// 快照元数据。cell 数组内存由调用方（C++）拥有，见 snapshotCells 契约。
public struct BridgeCellMeta {
    public var rows: Int32 = 0
    public var cols: Int32 = 0
    public var cursorRow: Int32 = 0
    public var cursorCol: Int32 = 0
    public init() {}
}

/// 会话状态 POD。
public struct BridgeStatus {
    public var phase: Int32 = 0   // 0=running 1=exited
    public var exitCode: Int32 = 0
    public init() {}
}

/// 审计 P2-A 落定：可视网格容量上限 200 列 × 60 行 = 12000 cell
/// （覆盖 POC-4 的 80×24=1920 基准与 20×6 极小窗下限）。
/// 溢出契约：rows*cols > cellCapacity 时 snapshotCells 返回 false，不部分拷贝；
/// C++ 侧镜像同值常量（swift_bridge.h），两侧以 POC 断言锁定一致。
public enum BridgeLimits {
    public static let maxCols = 200
    public static let maxRows = 60
    public static let cellCapacity = maxCols * maxRows  // 12000
}

/// spawn 错误盒（let 静态实例 = 并发安全持有）。
final class SpawnErrorBox: @unchecked Sendable {
    let lock = NSLock()
    var lastError = ""
}

/// 桥接会话：spawn / write / snapshotCells / scrollback / selectionText / status / setWakeCallback / stop。
/// 线程模型（技术卡 §5.3 三规则+审计 P2-B 第 4 拆除规则）：
/// 1. 核心读线程只触发本类内部 dispatch → 主队列 → trampoline，核心线程永不直接进 C++；
/// 2. snapshotCells/write/resize/stop/status 只在 EUI 主线程调；
/// 3. 唤醒合并：脏标记已置位则不再 enqueue（审计 P3-D 防队列洪水）；
/// 4. stop() 置位后在途 main-queue block 一律静默，不触发 trampoline（悬空 ctx 防护）。
public final class CortaBridgeSession: @unchecked Sendable {
    private let session: TerminalSession
    private let lock = NSLock()
    private var wakeFnPtr: UnsafeMutableRawPointer?
    private var wakeCtx: UnsafeMutableRawPointer?
    private var wakePending = false
    private var stopped = false
    private var phase: Int32 = 0
    private var exitCode: Int32 = 0
    private var generation: Int32 = 0

    /// 最近一次 spawn 失败原因（let 盒子=并发安全；Swift 6 全局可变态禁令绕行）。
    private static let errorBox = SpawnErrorBox()

    /// spawn 失败原因写出到 UTF-8（容量不足/无记录返回 false）。
    public static func lastSpawnError(
        _ out: UnsafeMutablePointer<UInt8>, capacity: Int32
    ) -> Bool {
        errorBox.lock.lock(); defer { errorBox.lock.unlock() }
        return copyUTF8(errorBox.lastError, to: out, capacity: capacity)
    }

    /// spawn 失败返回 nil（F-07 失败卡消息面：错误经 lastSpawnError 读取）。
    public convenience init?(
        executable: String, arguments: [String] = [], rows: Int32, cols: Int32
    ) {
        let size = TerminalSize(
            rows: UInt16(max(rows, 1)), columns: UInt16(max(cols, 1)))
        let session: TerminalSession
        do {
            session = try TerminalSession(
                executable: executable, arguments: arguments, size: size)
        } catch {
            CortaBridgeSession.errorBox.lock.lock()
            CortaBridgeSession.errorBox.lastError = "\(error)"
            CortaBridgeSession.errorBox.lock.unlock()
            return nil
        }
        self.init(owned: session)
    }

    /// 选区文本写系统剪贴板（Cmd+C 硬验收 F-04）。
    public static func copyToClipboard(_ bytes: UnsafePointer<UInt8>, count: Int32) -> Bool {
        guard count > 0,
              let text = String(bytes: UnsafeBufferPointer(start: bytes, count: Int(count)),
                                encoding: .utf8)
        else { return false }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
        return true
    }

    /// 交互 shell 入口（DevDesk 集成用）。核心语义：executable 自身即 argv0，
    /// arguments 只放额外参数（实测传 [executable] 会把二进制当脚本执行 → 127）。
    public static func spawnInteractive(
        executable: String, rows: Int32, cols: Int32
    ) -> CortaBridgeSession? {
        CortaBridgeSession(executable: executable, arguments: [], rows: rows, cols: cols)
    }

    /// C++ 侧便捷口：shell 命令串（zsh -f -c，无交互/无 prompt，测试与 POC 用）。
    /// 与 init?(executable:) 签名同形会撞 C++ 重载，故取独立静态工厂名。
    public static func spawnShell(
        command: String, rows: Int32, cols: Int32
    ) -> CortaBridgeSession? {
        CortaBridgeSession(
            executable: "/bin/zsh", arguments: ["-f", "-c", command],
            rows: rows, cols: cols)
    }

    private init(owned session: TerminalSession) {
        self.session = session
        session.onOutput = { [weak self] in self?.scheduleWake() }
        session.onChildExit = { [weak self] exit in
            guard let self else { return }
            self.lock.lock()
            self.phase = 1
            switch exit {
            case .exited(let code): self.exitCode = code
            case .signalled(let sig): self.exitCode = 128 + sig
            }
            self.lock.unlock()
            self.scheduleWake()
        }
        session.start()
    }

    // MARK: - 输入

    public func write(_ bytes: UnsafePointer<UInt8>, count: Int32) {
        _ = session.write(Array(UnsafeBufferPointer(start: bytes, count: Int(count))))
    }

    public func writePaste(_ bytes: UnsafePointer<UInt8>, count: Int32) {
        // bracketed paste 结束序列 ESC[201~（核心 write(paste:closing:) 语义，取消切割时最后单独发）。
        let closing: [UInt8] = [0x1b, 0x5b, 0x32, 0x30, 0x31, 0x7e]
        _ = session.write(paste: [Array(UnsafeBufferPointer(start: bytes, count: Int(count)))], closing: closing)
    }

    public func resize(rows: Int32, cols: Int32) {
        session.resize(to: TerminalSize(
            rows: UInt16(max(rows, 1)), columns: UInt16(max(cols, 1))))
    }

    // MARK: - 快照（主线程拉取）

    /// 拉取平铺缓冲。返回 false = 网格超 capacity（溢出契约，不部分拷贝）。
    /// 边界用 UnsafeMutableRawPointer（void*）：typed `UnsafeMutablePointer<Struct>`
    /// 参数会被 C++ interop 丢弃（实测）；Swift 侧 rebind 回类型指针。
    public func snapshotCells(
        meta: UnsafeMutableRawPointer, cells: UnsafeMutableRawPointer, capacity: Int32
    ) -> Bool {
        let metaOut = meta.assumingMemoryBound(to: BridgeCellMeta.self)
        let cellsOut = cells.assumingMemoryBound(to: BridgeCell.self)
        let grid = session.snapshot()  // 锁下值拷贝
        let rows = grid.rows, cols = grid.columns
        if rows * cols > Int(capacity) { return false }
        metaOut.pointee.rows = Int32(rows)
        metaOut.pointee.cols = Int32(cols)
        metaOut.pointee.cursorRow = Int32(grid.cursor.row)
        metaOut.pointee.cursorCol = Int32(grid.cursor.column)
        for r in 0..<rows {
            for c in 0..<cols {
                let cell = grid[r, c]
                var bc = BridgeCell()
                // 池 bug248：宽字对（.wide lead + .wideSpacer）必须映射成
                //   lead  → cp=汉字,  width=2
                //   spacer→ cp=0,     width=0
                // 与 libvterm 后端（terminal_vterm_core.cpp translateCell）同一契约，
                // 下游 rowSliceText 靠「跳过 cp==0 的零宽占位格」拼出连续的 "杭州"。
                //
                // 旧实现用 `cell.grapheme.isNone ? 1 : 2` 判宽——错了：Corta 的
                // 宽字 lead 与 spacer **都** 满足 grapheme.isNone（grapheme 只标记
                // 超出单 scalar 的 cluster），于是两者都被记成 width=1，而 spacer
                // 的 scalar 是真实 0x20 空格 → 终端文本变成 "杭 州 今 天 天 气"，
                // 中文永远无法连续匹配（UI 渲染看不出，只有文本提取是脏的）。
                // 正确判据是 Corta 自己打的属性位：.wide / .wideSpacer。
                let isSpacer = cell.attributes.contains(.wideSpacer)
                let isWide = cell.attributes.contains(.wide)
                bc.cp = isSpacer ? 0 : cell.scalar
                bc.fg = cell.foreground.rawValue
                bc.bg = cell.background.rawValue
                bc.attrs = cell.attributes.rawValue
                bc.width = isSpacer ? 0 : (isWide ? 2 : 1)
                cellsOut[r * cols + c] = bc
            }
        }
        return true
    }

    // MARK: - 回滚与选区文本（POC-3）

    /// 已推入 scrollback 的总行数。
    public func scrollbackCount() -> Int32 {
        Int32(session.snapshot().scrollback.count)
    }

    /// 读 scrollback 第 index 行（0=最旧）UTF-8 文本；容量不足或越界返回 false。
    public func scrollbackLineText(
        _ index: Int32, _ out: UnsafeMutablePointer<UInt8>, capacity: Int32
    ) -> Bool {
        let grid = session.snapshot()
        let sb = grid.scrollback
        guard Int(index) < sb.count else { return false }
        var text = ""
        for cell in sb[Int(index)].cells where cell.scalar != 0 {
            if let scalar = Unicode.Scalar(cell.scalar) { text.unicodeScalars.append(scalar) }
        }
        return Self.copyUTF8(text, to: out, capacity: capacity)
    }

    /// 选区矩形→文本提取（所见即所得走核心 Selection.text；Phase 2 接 UI，本签名即 POC-3 API 存在性验证）。
    public func selectionText(
        rowStart: Int32, colStart: Int32, rowEnd: Int32, colEnd: Int32,
        _ out: UnsafeMutablePointer<UInt8>, capacity: Int32
    ) -> Bool {
        let grid = session.snapshot()
        let range = SelectionRange(
            anchor: SelectionPoint(row: Int(rowStart), column: Int(colStart)),
            head: SelectionPoint(row: Int(rowEnd), column: Int(colEnd)))
        return Self.copyUTF8(Selection.text(of: range, in: grid), to: out, capacity: capacity)
    }

    private static func copyUTF8(
        _ text: String, to out: UnsafeMutablePointer<UInt8>, capacity: Int32
    ) -> Bool {
        let bytes = Array(text.utf8)
        guard bytes.count < Int(capacity) else { return false }
        for (i, b) in bytes.enumerated() { out[i] = b }
        out[bytes.count] = 0
        return true
    }

    // MARK: - 状态与唤醒

    public func status() -> BridgeStatus {
        lock.lock(); defer { lock.unlock() }
        var s = BridgeStatus()
        s.phase = phase
        s.exitCode = exitCode
        return s
    }

    /// 注册唤醒回调（EUI 主线程触发；stop 后保证静默）。
    /// C 函数指针以 void* 跨界（Swift 闭包参数不上 C++ interop 面，实测）：
    /// cb = 指向 `void fn(void* ctx)` 的指针，内部还原调用。
    public func setWakeCallback(cb: UnsafeMutableRawPointer?, ctx: UnsafeMutableRawPointer?) {
        lock.lock()
        wakeFnPtr = cb
        wakeCtx = ctx
        lock.unlock()
    }

    public func clearWakeCallback() {
        lock.lock()
        wakeFnPtr = nil
        wakeCtx = nil
        lock.unlock()
    }

    public func stop() {
        lock.lock()
        stopped = true
        lock.unlock()
        session.stop()
    }

    /// 合并唤醒（审计 P3-D）：pending 未消费前不再 enqueue，读线程风暴只产生一个主队列 block。
    /// DevDesk 集成主路径：每帧 poll 该计数（原子读），变化才 snapshot（技术卡 §5.3 收敛：
    /// GLFW 主循环不泵 GCD 主队列，trampoline 通路留作 POC/未来 timer 用）。
    public func pendingGeneration() -> Int32 {
        lock.lock(); defer { lock.unlock() }
        return generation
    }

    private func scheduleWake() {
        lock.lock()
        generation &+= 1
        lock.unlock()
        let shouldEnqueue = lock.withLock {
            if stopped || wakePending { return false }
            wakePending = true
            return true
        }
        guard shouldEnqueue else { return }
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            let fnCtx: (UnsafeMutableRawPointer?, UnsafeMutableRawPointer?)? = self.lock.withLock {
                self.wakePending = false
                // 第 4 拆除规则：stop 后在途 block 静默。
                guard !self.stopped, let fnPtr = self.wakeFnPtr else { return nil }
                return (fnPtr, self.wakeCtx)
            }
            guard let (fnPtr, ctx) = fnCtx else { return }
            let fn = unsafeBitCast(fnPtr, to: (@convention(c) (UnsafeMutableRawPointer?) -> Void).self)
            fn(ctx)
        }
    }
}
