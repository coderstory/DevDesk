/* 网页终端协议层（need#490）——帧编解码 + 客户端接口占位。
 *
 * 真实接线见技术卡 v5 §5/§6（v6 修正加密为纯 JS）：X25519 握手 → HKDF 派生方向密钥 →
 * 纯 JS ChaCha20-Poly1305 信封 → SSE 承载 base64 密文帧；输入走密文 POST。
 * 本文件先落纯前端骨架：MockClient 用**同一帧格式**驱动原型
 * （encodeFrame → parseFrame 往返，保证解析函数被真实执行），
 * RealClient 只留签名，握手与 SSE 由后续任务接。
 *
 * 帧格式（技术卡 §5.1，AEAD 明文部分，字节偏移）：
 *   0 magic 'D','W' | 2 version=1 | 3 flags(bit0 光标可见)
 *   4 rows u16 | 6 cols u16 | 8 cursorRow u16 | 10 cursorCol u16
 *   12 scrollOffset u32 | 16 scrollbackCount u32 | 20 generation u32
 *   24 行区：逐行逐 run，每行累计 cells 到 cols 收行
 *   run：u16 cells | u8 width(1|2) | u8 flags(default/bold/underline)
 *        | u32 fgRGBA | u32 bgRGBA | u16 attrsRaw | u16 textLen | text bytes
 */
(function (global) {
  'use strict';

  var MAGIC0 = 0x44, MAGIC1 = 0x57, VERSION = 1;
  var FLAG_CURSOR = 1;
  var F_FG_DEFAULT = 1, F_BG_DEFAULT = 2, F_BOLD = 4, F_UNDERLINE = 8;

  /* fgRGBA/bgRGBA 打包为 0xRRGGBBAA（技术卡未定字节序，前后端须一致） */
  function hexToU32(hex) {
    if (!hex) return 0;
    var h = hex.replace('#', '');
    if (h.length === 3) h = h[0] + h[0] + h[1] + h[1] + h[2] + h[2];
    if (h.length === 6) h += 'ff';
    return (parseInt(h.slice(0, 8), 16) >>> 0);
  }
  function u32ToHex(v) {
    var a = v & 255;
    if (a === 0) return null;
    var p = function (n) { return n.toString(16).toUpperCase().padStart(2, '0'); };
    return '#' + p((v >>> 24) & 255) + p((v >>> 16) & 255) + p((v >>> 8) & 255);
  }

  function styleKey(c) { return (c.fg || '') + '|' + (c.bg || '') + '|' + (c.bold ? 1 : 0) + '|' + (c.underline ? 1 : 0); }
  function runFlags(c) {
    var f = 0;
    if (!c.fg) f |= F_FG_DEFAULT;
    if (!c.bg) f |= F_BG_DEFAULT;
    if (c.bold) f |= F_BOLD;
    if (c.underline) f |= F_UNDERLINE;
    return f;
  }

  function encodeFrame(f) {
    var enc = new TextEncoder();
    var header = new Uint8Array(24);
    var hv = new DataView(header.buffer);
    header[0] = MAGIC0; header[1] = MAGIC1; header[2] = VERSION;
    header[3] = f.cursorVisible ? FLAG_CURSOR : 0;
    hv.setUint16(4, f.rows); hv.setUint16(6, f.cols);
    hv.setUint16(8, f.cursorRow); hv.setUint16(10, f.cursorCol);
    hv.setUint32(12, f.scrollOffset || 0); hv.setUint32(16, f.scrollbackCount || 0);
    hv.setUint32(20, f.generation || 0);
    var parts = [header];
    for (var r = 0; r < f.rows; r++) {
      var row = f.lines[r] || [];
      var c = 0;
      while (c < f.cols) {
        var cell = row[c] || { ch: ' ', w: 1 };
        if (cell.ph) { c++; continue; }
        var width = cell.w === 2 ? 2 : 1;
        var cells = width;
        var text = cell.ch || ' ';
        var key = styleKey(cell);
        var j = c + width;
        if (width === 1) {
          while (j < f.cols) {
            var n = row[j];
            if (!n || n.ph || n.w === 2 || styleKey(n) !== key) break;
            cells++; text += (n.ch || ' '); j++;
          }
        }
        var tb = enc.encode(text);
        var H = new Uint8Array(16); var dv = new DataView(H.buffer);
        dv.setUint16(0, cells); H[2] = width; H[3] = runFlags(cell);
        dv.setUint32(4, hexToU32(cell.fg)); dv.setUint32(8, hexToU32(cell.bg));
        dv.setUint16(12, 0); dv.setUint16(14, tb.length);
        parts.push(H, tb);
        c = j;
      }
    }
    var len = 0; for (var k = 0; k < parts.length; k++) len += parts[k].length;
    var out = new Uint8Array(len); var o = 0;
    for (var m = 0; m < parts.length; m++) { out.set(parts[m], o); o += parts[m].length; }
    return out;
  }

  function parseFrame(input) {
    var buf = input instanceof Uint8Array ? input : new Uint8Array(input);
    if (buf[0] !== MAGIC0 || buf[1] !== MAGIC1) throw new Error('bad magic');
    if (buf[2] !== VERSION) throw new Error('bad version');
    var dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
    var flags = buf[3];
    var rows = dv.getUint16(4), cols = dv.getUint16(6);
    var cursorRow = dv.getUint16(8), cursorCol = dv.getUint16(10);
    var scrollOffset = dv.getUint32(12), scrollbackCount = dv.getUint32(16), generation = dv.getUint32(20);
    var dec = new TextDecoder();
    var off = 24, lines = [];
    for (var r = 0; r < rows; r++) {
      var row = new Array(cols);
      var filled = 0;
      while (filled < cols && off + 16 <= buf.length) {
        var cells = dv.getUint16(off);
        var width = buf[off + 2];
        var rf = buf[off + 3];
        var fg = u32ToHex(dv.getUint32(off + 4));
        var bg = u32ToHex(dv.getUint32(off + 8));
        var textLen = dv.getUint16(off + 14);
        var text = dec.decode(buf.subarray(off + 16, off + 16 + textLen));
        off += 16 + textLen;
        var fgv = (rf & F_FG_DEFAULT) ? null : fg;
        var bgv = (rf & F_BG_DEFAULT) ? null : bg;
        var bold = !!(rf & F_BOLD), underline = !!(rf & F_UNDERLINE);
        var runStart = filled;
        var chars = Array.from(text);
        for (var q = 0; q < chars.length && filled < cols; q++) {
          if (width === 2) {
            row[filled] = { ch: chars[q], w: 2, bold: bold, underline: underline, fg: fgv, bg: bgv };
            if (filled + 1 < cols) row[filled + 1] = { ph: true, w: 0, bold: bold, underline: underline, fg: fgv, bg: bgv };
            filled += 2;
          } else {
            row[filled] = { ch: chars[q], w: 1, bold: bold, underline: underline, fg: fgv, bg: bgv };
            filled += 1;
          }
        }
        while (filled < runStart + cells && filled < cols) {
          row[filled] = { ch: ' ', w: 1, bold: bold, underline: underline, fg: fgv, bg: bgv };
          filled++;
        }
      }
      for (var p = 0; p < cols; p++) if (!row[p]) row[p] = { ch: ' ', w: 1 };
      lines.push(row);
    }
    return {
      version: VERSION, flags: flags, cursorVisible: !!(flags & FLAG_CURSOR),
      rows: rows, cols: cols, cursorRow: cursorRow, cursorCol: cursorCol,
      scrollOffset: scrollOffset, scrollbackCount: scrollbackCount,
      generation: generation, lines: lines
    };
  }

  /* bracketed paste：多行按原文插入，不被 shell 逐行执行（PRD 裁决 14 / P-23） */
  function bracketedPaste(text) { return '\x1b[200~' + text + '\x1b[201~'; }

  /* ---------- 传输加密（技术卡 §6 / v6 纯 JS） ----------
   * 局域网明文 HTTP = 非安全上下文，crypto.subtle 不存在（Chromium/WebKit/Firefox 一致，
   * 实测 tests/e2e/CAPABILITY-MATRIX.md）。因此生产路径**不得依赖 WebCrypto**：
   * X25519(RFC 7748) + HKDF-SHA256(RFC 5869) + ChaCha20-Poly1305(RFC 8439) 全部纯 JS。
   * 本任务只落接口与模块边界，实现留 TODO；WebCrypto 仅可作回环调试加速（可选、默认关），
   * 且行为必须与局域网纯 JS 路径一致。
   *   握手：GET /api/hs → {hid, spk, salt}；deriveSharedKey(csk, spk, salt, hid)
   *   帧：encryptFrame(key, nonceCounter, plaintext) → ver‖nonce‖ct‖tag
   *       decryptFrame(key, counter, envelope) → plaintext（失败即弃钥重握手，不渲染）
   */
  var cryptoDevWebCrypto = false; // 仅回环调试可置 true；生产恒 false
  function webCryptoAvailable() { return typeof crypto !== 'undefined' && !!crypto.subtle; }
  function deriveSharedKey(/* clientSecret, serverPublic, salt, hid */) {
    throw new Error('TODO: 按技术卡 v6 纯 JS 实现 X25519 + HKDF-SHA256 派生方向密钥');
  }
  function encryptFrame(/* key, counter, plaintext */) {
    throw new Error('TODO: 按技术卡 v6 纯 JS 实现 ChaCha20-Poly1305 加密帧');
  }
  function decryptFrame(/* key, counter, envelope */) {
    throw new Error('TODO: 按技术卡 v6 纯 JS 实现 ChaCha20-Poly1305 解密帧');
  }

  var KEYMAP = {
    Enter: '\r', Backspace: '\x7f', Tab: '\t', Escape: '\x1b',
    ArrowUp: '\x1b[A', ArrowDown: '\x1b[B', ArrowRight: '\x1b[C', ArrowLeft: '\x1b[D',
    Home: '\x1b[H', End: '\x1b[F', PageUp: '\x1b[5~', PageDown: '\x1b[6~', Delete: '\x1b[3~'
  };
  function keyToBytes(ev) {
    if (KEYMAP[ev.key]) return KEYMAP[ev.key];
    if (/^F([1-9]|1[0-2])$/.test(ev.key)) return '\x1b[' + (10 + +ev.key.slice(1)) + '~';
    if (ev.ctrlKey && ev.key.length === 1) {
      var c = ev.key.toUpperCase().charCodeAt(0);
      if (c >= 64 && c <= 95) return String.fromCharCode(c - 64);
    }
    return null;
  }

  /* ---------- 客户端接口（真实/原型同一形状） ---------- */
  /* 真实实现：connect() 先 GET /api/hs 握手，再 auth/state，再申请流票据开 SSE；
   * sendInput() 打包 m:"input" 走密文 POST /api/e；onFrame() 收 SSE 解密帧。
   * 原型 MockClient 不联网，用本地帧生成器模拟同一契约。 */

  var DEFAULT_SHELL = /Win/i.test((global.navigator && navigator.platform) || '') ? '默认（PowerShell）' : '默认（zsh）';

  function MockClient() {
    this._frameCbs = []; this._stateCbs = []; this._settingsCbs = [];
    this.status = 'idle';
    this.tabs = [{ id: 't1', title: '终端 1' }, { id: 't2', title: '终端 2' }];
    this.activeId = 't1';
    this.viewport = { cols: 45, rows: 28 };
    this.generation = 0;
    this.scrollback = [];
    this.settings = {
      font: 'builtin', fontSize: 13, colorMode: 'follow', palette: 'dark',
      shell: DEFAULT_SHELL, startupDir: ''
    };
    this._rcTimer = null; this._rcAttempts = 0;
  }
  MockClient.prototype.onFrame = function (cb) { this._frameCbs.push(cb); };
  MockClient.prototype.onState = function (cb) { this._stateCbs.push(cb); };
  MockClient.prototype.onSettings = function (cb) { this._settingsCbs.push(cb); };
  MockClient.prototype._emitState = function () {
    var s = { status: this.status, tabs: this.tabs.slice(), activeId: this.activeId };
    this._stateCbs.forEach(function (cb) { cb(s); });
  };
  MockClient.prototype._emitSettings = function () {
    var s = Object.assign({}, this.settings);
    this._settingsCbs.forEach(function (cb) { cb(s); });
  };
  MockClient.prototype.connect = function () {
    var self = this;
    this.status = 'connecting'; this._emitState();
    return new Promise(function (resolve) {
      setTimeout(function () {
        self.status = 'connected';
        self.scrollback = makeScrollback();
        self._emitState(); self._emitFrame(); resolve();
      }, 500);
    });
  };
  MockClient.prototype.setViewport = function (v) {
    if (v.cols === this.viewport.cols && v.rows === this.viewport.rows) return;
    this.viewport = { cols: v.cols, rows: v.rows };
    this._emitFrame();
  };
  MockClient.prototype._activeTab = function () {
    for (var i = 0; i < this.tabs.length; i++) if (this.tabs[i].id === this.activeId) return this.tabs[i];
    return null;
  };
  MockClient.prototype._emitFrame = function () {
    var tab = this._activeTab();
    if (!tab) return;
    var payload = buildPayload(tab, this.viewport, ++this.generation, this.scrollback.length);
    var parsed = parseFrame(encodeFrame(payload)); // 每次都真实执行解析函数
    this._frameCbs.forEach(function (cb) { cb(parsed); });
  };
  MockClient.prototype.selectTab = function (id) {
    if (!this.tabs.some(function (t) { return t.id === id; })) return;
    this.activeId = id; this._emitState(); this._emitFrame();
  };
  MockClient.prototype.newTab = function () {
    var id = 't' + (Date.now());
    this.tabs.push({ id: id, title: '终端 ' + (this.tabs.length + 1) });
    this.activeId = id;
    this._emitState(); this._emitFrame();
  };
  MockClient.prototype.closeTab = function (id) {
    var idx = -1;
    for (var i = 0; i < this.tabs.length; i++) if (this.tabs[i].id === id) idx = i;
    if (idx < 0) return;
    this.tabs.splice(idx, 1);
    if (this.activeId === id) {
      var next = this.tabs[idx] || this.tabs[idx - 1] || null; // 落右邻，无右邻落左邻
      this.activeId = next ? next.id : null;
    }
    this._emitState();
    if (this.activeId) this._emitFrame();
  };
  MockClient.prototype.sendInput = function (text) {
    if (this.status !== 'connected' || !text) return;
    storeEcho(text); // 原型：仅记录，真实实现走密文 POST m:"input"
  };
  MockClient.prototype.paste = function (text) { this.sendInput(bracketedPaste(text)); };
  MockClient.prototype.resize = function (cols, rows) {
    var clamp = function (v, lo, hi) { return Math.max(lo, Math.min(hi, v)); };
    this.setViewport({ cols: clamp(cols, 20, 200), rows: clamp(rows, 8, 200) });
  };
  MockClient.prototype.writeSettings = function (patch) {
    var self = this;
    return new Promise(function (resolve, reject) {
      setTimeout(function () {
        var dir = patch.startupDir;
        if (dir !== undefined && dir !== '') {
          // 原型校验：POSIX 绝对路径或盘符；真实实现由服务端 canEnterDirectory 判定
          var ok = /^\/.+/.test(dir) || /^[A-Za-z]:\\/.test(dir);
          if (!ok) { reject({ code: 'dir_invalid', field: 'startupDir' }); return; }
        }
        Object.assign(self.settings, patch);
        self._emitSettings(); resolve(Object.assign({}, self.settings));
      }, 450);
    });
  };
  MockClient.prototype.simulateDisconnect = function () {
    var self = this;
    if (this.status !== 'connected') return;
    this.status = 'reconnecting'; this._emitState(); // ≤1s 顶部细条由状态回调立刻触发
    this._rcAttempts = 0;
    this._rcTimer = setTimeout(function () { self._reconnectTick(); }, 1000);
  };
  MockClient.prototype._reconnectTick = function () {
    var self = this;
    this._rcAttempts++;
    if (this._rcAttempts >= 2) {          // 连续 2 次失败（约 3 秒）升级全屏
      this.status = 'closed'; this._emitState(); return;
    }
    this._rcTimer = setTimeout(function () { self._reconnectTick(); }, 2000);
  };
  MockClient.prototype.reconnect = function () {
    var self = this;
    if (this._rcTimer) { clearTimeout(this._rcTimer); this._rcTimer = null; }
    this.status = 'connecting'; this._emitState();
    setTimeout(function () {
      self.status = 'connected'; self._emitState(); self._emitFrame();
    }, 400);
  };

  /* ---------- 原型数据 ---------- */
  var _echo = [];
  function storeEcho(t) { _echo.push(t); if (_echo.length > 50) _echo.shift(); }

  function charWidth(cp) {
    return (cp >= 0x1100 && (cp <= 0x115f || cp === 0x2329 || cp === 0x232a ||
      (cp >= 0x2e80 && cp <= 0xa4cf) || (cp >= 0xac00 && cp <= 0xd7a3) ||
      (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xfe30 && cp <= 0xfe4f) ||
      (cp >= 0xff00 && cp <= 0xff60) || (cp >= 0xffe0 && cp <= 0xffe6) ||
      (cp >= 0x1f300 && cp <= 0x1faff))) ? 2 : 1;
  }

  function makeScrollback() {
    var out = [];
    for (var i = 1; i <= 30; i++) out.push('  ' + i + '  ~/dev/DevDesk  git status / ls / cat …（回滚历史行占位）');
    return out;
  }

  /* 用 ANSI 深色 token 把带样式的片段展开成格网 */
  var ANSI = ['#1A1816', '#F87171', '#36D399', '#FBBF24', '#60A5FA', '#A18FC8', '#5EEAD4', '#FAF9F5',
    '#9A948A', '#F87171', '#36D399', '#FBBF24', '#60A5FA', '#A18FC8', '#5EEAD4', '#FFFFFF'];
  function expand(lines, cols) {
    var rows = [];
    for (var i = 0; i < lines.length; i++) {
      var cells = [];
      var segs = lines[i] || [];
      for (var s = 0; s < segs.length; s++) {
        var seg = segs[s];
        var fg = seg.c == null ? null : ANSI[seg.c];
        var chars = Array.from(seg.t);
        for (var k = 0; k < chars.length; k++) {
          var cp = chars[k].codePointAt(0);
          var w = charWidth(cp);
          if (w === 2 && cells.length + 2 > cols) { cells.push({ ch: ' ', w: 1, fg: fg, bold: !!seg.b }); continue; }
          cells.push({ ch: chars[k], w: w, fg: fg, bold: !!seg.b, underline: !!seg.u });
          if (w === 2) cells.push({ ph: true, w: 0, fg: fg, bold: !!seg.b });
        }
      }
      while (cells.length < cols) cells.push({ ch: ' ', w: 1 });
      if (cells.length > cols) cells.length = cols;
      rows.push(cells);
    }
    return rows;
  }

  function contentLines(tab) {
    var wide = tab.id === 't2';
    var L = [
      [{ t: '$ ', c: 8 }, { t: 'claude' }],
      [{ t: '╭──────────────────────────────────────╮', c: 2 }],
      [{ t: '│ ', c: 2 }, { t: '✻ Welcome to Claude Code', b: true }, { t: '         │', c: 2 }],
      [{ t: '╰──────────────────────────────────────╯', c: 2 }],
      [{ t: '  cwd: ~/dev/DevDesk' }],
      [{ t: '  model: claude-opus-4' }],
      [{ t: '' }],
      [{ t: '> ', c: 4 }, { t: '检查 terminal 模块的 tab 同步逻辑' }],
      [{ t: '● ', c: 2 }, { t: '正在读取 src/module_terminal/tab_manager.cpp' }],
      [{ t: '  ⎿ ', c: 8 }, { t: 'Read 412 lines' }],
      [{ t: '● ', c: 2 }, { t: '发现 2 处需要确认' }],
      [{ t: '  ⎿ ', c: 8 }, { t: 'tab 关闭后的激活落位逻辑' }],
      [{ t: '  ⎿ ', c: 8 }, { t: '多端同步的时序窗口' }],
      [{ t: '' }],
      [{ t: '⚠ 待确认：关闭最后一个 tab 时，网页端是否自动新建？', c: 3 }],
      [{ t: '  （PRD P-07：不自动新建，进入空态）', c: 8 }],
      [{ t: '' }],
      [{ t: '> ', c: 4 }, { t: '█' }]
    ];
    if (wide) {
      L.push([{ t: '' }]);
      L.push([{ t: '# 本行故意超出手机可视宽度，用于验证画布横向滚动', c: 8 }]);
      L.push([{ t: '━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━', c: 4 }]);
      L.push([{ t: '● ', c: 2 }, { t: '横向拖动只在网格宽于视口时出现；顶栏与底栏保持固定。' }]);
    }
    return L;
  }

  function buildPayload(tab, viewport, generation, scrollbackCount) {
    var cols = tab.id === 't2' ? Math.min(200, viewport.cols + 30) : viewport.cols;
    var rows = viewport.rows;
    var lines = contentLines(tab);
    var all = expand(lines, cols);
    var offset = Math.max(0, all.length - rows); // 超出视口取末行（终端视口语义，提示符留在底部）
    var grid = all.slice(offset, offset + rows);
    while (grid.length < rows) {
      var pad = [];
      for (var c = 0; c < cols; c++) pad.push({ ch: ' ', w: 1 });
      grid.push(pad);
    }
    var cr = Math.min(rows - 1, Math.max(0, lines.length - 1 - offset));
    var cc = 0;
    var lastRow = grid[cr]; for (var i = 0; i < lastRow.length; i++) if (!lastRow[i].ph) cc = i + 1;
    return {
      cursorVisible: true, rows: rows, cols: cols,
      cursorRow: cr, cursorCol: Math.min(cols - 1, cc),
      scrollOffset: 0, scrollbackCount: scrollbackCount,
      generation: generation, lines: grid
    };
  }

  global.WT = {
    encodeFrame: encodeFrame, parseFrame: parseFrame, bracketedPaste: bracketedPaste,
    keyToBytes: keyToBytes, hexToU32: hexToU32, u32ToHex: u32ToHex,
    deriveSharedKey: deriveSharedKey, encryptFrame: encryptFrame, decryptFrame: decryptFrame,
    webCryptoAvailable: webCryptoAvailable,
    get cryptoDevWebCrypto() { return cryptoDevWebCrypto; },
    set cryptoDevWebCrypto(v) { cryptoDevWebCrypto = !!v; },
    MockClient: MockClient, ANSI: ANSI
  };
})(typeof window !== 'undefined' ? window : globalThis);
