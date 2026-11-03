// BridgeFeedTests — 门面平铺缓冲语义断言（技术卡 §9）：改门面翻译逻辑必红。
// 用短命真实会话（zsh -f -c，POC 已证确定性），断言打在 snapshotCells 语义上。
import XCTest

@testable import CortaBridge

final class BridgeFeedTests: XCTestCase {
    private func make(_ cmd: String, rows: Int32 = 10, cols: Int32 = 40) -> CortaBridgeSession {
        guard let s = CortaBridgeSession.spawnShell(command: cmd, rows: rows, cols: cols) else {
            XCTFail("spawn 失败")
            return CortaBridgeSession(
                executable: "/bin/echo", arguments: [], rows: rows, cols: cols)!
        }
        return s
    }

    private func waitExit(_ s: CortaBridgeSession, timeoutMs: Int32 = 3000) {
        for _ in 0..<timeoutMs / 5 where s.status().phase != 1 {
            usleep(5_000)
        }
    }

    private func snapshot(_ s: CortaBridgeSession) -> BridgeCellMeta {
        var meta = BridgeCellMeta()
        var cells = [BridgeCell](repeating: BridgeCell(), count: BridgeLimits.cellCapacity)
        let ok = s.snapshotCells(meta: &meta, cells: &cells, capacity: Int32(BridgeLimits.cellCapacity))
        XCTAssertTrue(ok, "snapshotCells 应成功")
        return meta
    }

    func testExitStatusReportsZeroCode() {
        let s = make("echo feed-test")
        waitExit(s)
        let st = s.status()
        XCTAssertEqual(st.phase, 1, "echo 后应 exited")
        XCTAssertEqual(st.exitCode, 0)
        s.stop()
    }

    func testSnapshotCarriesCpAndAnsiColor() {
        // 红色 X：ANSI indexed 红=1 → fg rawValue = (1 << 24) | 1
        let s = make("printf '\\033[31mX\\033[0m'")
        waitExit(s)
        var meta = BridgeCellMeta()
        var cells = [BridgeCell](repeating: BridgeCell(), count: BridgeLimits.cellCapacity)
        XCTAssertTrue(s.snapshotCells(
            meta: &meta, cells: &cells, capacity: Int32(BridgeLimits.cellCapacity)))
        XCTAssertTrue(meta.rows > 0 && meta.cols > 0, "元数据应为正")
        let first = cells[0]
        XCTAssertEqual(first.cp, UInt32(UnicodeScalar("X").value), "首 cell 码点=X")
        XCTAssertEqual(first.fg >> 24, 1, "ANSI 色应 kind=indexed")
        XCTAssertEqual(first.fg & 0xff, 1, "index 应为红=1")
        s.stop()
    }

    func testWideCharCellPresent() {
        // CJK：宽字按 grapheme 近似记 width（displayWidth internal，Phase 1 精化——见 clue #23）
        let s = make("printf '中'")
        waitExit(s)
        var meta = BridgeCellMeta()
        var cells = [BridgeCell](repeating: BridgeCell(), count: BridgeLimits.cellCapacity)
        XCTAssertTrue(s.snapshotCells(
            meta: &meta, cells: &cells, capacity: Int32(BridgeLimits.cellCapacity)))
        XCTAssertEqual(cells[0].cp, 0x4E2D, "CJK 码点应进缓冲")
        s.stop()
    }

    func testResizeChangesGridMeta() {
        let s = make("sleep 0.5", rows: 10, cols: 40)
        usleep(100_000)
        s.resize(rows: 6, cols: 20)
        var meta = BridgeCellMeta()
        var cells = [BridgeCell](repeating: BridgeCell(), count: BridgeLimits.cellCapacity)
        XCTAssertTrue(s.snapshotCells(
            meta: &meta, cells: &cells, capacity: Int32(BridgeLimits.cellCapacity)))
        XCTAssertEqual(meta.rows, 6, "resize 后 rows 应为 6")
        XCTAssertEqual(meta.cols, 20, "resize 后 cols 应为 20")
        s.stop()
    }

    func testScrollbackOrderAndContent() {
        let s = make("seq 1 60", rows: 10, cols: 40)
        waitExit(s)
        let count = s.scrollbackCount()
        XCTAssertGreaterThanOrEqual(count, 50, "60 行输出应推入 ≥50 行回滚")
        var text = [UInt8](repeating: 0, count: 256)
        XCTAssertTrue(s.scrollbackLineText(0, &text, capacity: 256), "读最旧行应成功")
        XCTAssertEqual(String(bytes: text.prefix(while: { $0 != 0 }), encoding: .utf8), "1")
        s.stop()
    }
}
