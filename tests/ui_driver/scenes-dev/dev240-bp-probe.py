"""bug240③ 验收探针：bracketed paste 包裹行为直读。

判据（PTY 输入端字节流）：
  - 修复后：单键 'a' 到达子进程的字节流 = 0x61 → 打印 `RX %61`
  - 未修复：write() 对每次写入包裹 ESC[200~ ... ESC[201~ →
    打印 `PASTE %1b%5b%32%30%30%7e%61%1b%5b%32%30%31%7e`
上方第一行 `\x1b[?2004h` 让前端 vterm 认为子进程已开启 DECSET 2004
（与 Claude Code 等 TUI 同条件）。
"""
import ctypes

k = ctypes.windll.kernel32
in_h = k.GetStdHandle(-10)
out_h = k.GetStdHandle(-11)

mode = ctypes.c_uint32(0)
k.GetConsoleMode(in_h, ctypes.byref(mode))
# 清 LINE_INPUT(0x2)/ECHO_INPUT(0x4)，置 ENABLE_VIRTUAL_TERMINAL_INPUT(0x200)
new_mode = (mode.value & ~0x6) | 0x200
set_ok = k.SetConsoleMode(in_h, new_mode)


def w(s):
    n = ctypes.c_uint32(0)
    k.WriteConsoleW(out_h, s, len(s), ctypes.byref(n), None)


w("\x1b[?2004h")  # 开启 bracketed paste（模拟 TUI）
w("BP-PROBE-READY mode=%d set=%d\r\n" % (mode.value, set_ok))

buf = ctypes.create_unicode_buffer(256)
nread = ctypes.c_uint32(0)
while True:
    got = k.ReadConsoleW(in_h, buf, 256, ctypes.byref(nread), None)
    if not got:
        w("ERR last=%d\r\n" % k.GetLastError())
        break
    s = buf[: nread.value]
    hexs = "".join("%%%02x" % ord(ch) for ch in s)
    w(("PASTE " if "\x1b[200~" in s else "RX ") + hexs + "\r\n")