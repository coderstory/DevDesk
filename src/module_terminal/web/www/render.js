/* 网页终端画布渲染（need#490）。
 *
 * 网格快照 → canvas 自绘；透明文本层负责浏览器原生选择（UI 设计 §5.2）。
 * 颜色取 UI 设计 §3.2 终端色板：ANSI 16 色在深/浅两套 token 上映射；
 * 画布本体唯一深色区，UI 控件用浅色中性（见 app.css）。
 *
 * 行高取 UI 设计 §3.3「1.4×字号、下限 14px」。技术卡 §9.2 另写
 * 1.25×——两处冲突时以 UI 设计为准（本任务 UI 实现的设计文档优先）。
 */
(function (global) {
  'use strict';

  var MONO = 'ui-monospace, SFMono-Regular, Menlo, Consolas, monospace';
  var PAD = 8;
  var REDUCED = global.matchMedia && global.matchMedia('(prefers-reduced-motion: reduce)').matches;

  var PALETTES = {
    dark: {
      bg: '#1A1816', fg: '#FAF9F5', dim: '#9A948A', cursor: '#36D399',
      selection: 'rgba(54,211,153,0.25)',
      ansi: ['#1A1816', '#F87171', '#36D399', '#FBBF24', '#60A5FA', '#A18FC8', '#5EEAD4', '#FAF9F5',
        '#9A948A', '#F87171', '#36D399', '#FBBF24', '#60A5FA', '#A18FC8', '#5EEAD4', '#FFFFFF']
    },
    light: {
      bg: '#FAFCFB', fg: '#1F2937', dim: '#6B6D80', cursor: '#157A4E',
      selection: 'rgba(21,122,78,0.18)',
      ansi: ['#FAFCFB', '#B91C1C', '#157E4E', '#B45309', '#1D4ED8', '#71639B', '#0E7490', '#1F2937',
        '#6B6D80', '#B91C1C', '#157E4E', '#B45309', '#1D4ED8', '#71639B', '#0E7490', '#111111']
    }
  };

  function displayWidth(str) {
    var n = 0, chars = Array.from(str);
    for (var i = 0; i < chars.length; i++) {
      var cp = chars[i].codePointAt(0);
      n += (cp >= 0x1100 && (cp <= 0x115f || (cp >= 0x2e80 && cp <= 0xa4cf) ||
        (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) ||
        (cp >= 0xff00 && cp <= 0xff60) || (cp >= 0x1f300 && cp <= 0x1faff))) ? 2 : 1;
    }
    return n;
  }

  function fitLine(line, cols) {
    var out = '', w = 0, chars = Array.from(line), truncated = false;
    for (var i = 0; i < chars.length; i++) {
      var cw = displayWidth(chars[i]);
      if (w + cw > cols) { truncated = true; break; }
      out += chars[i]; w += cw;
    }
    if (truncated) {
      // 截断处补省略号，避免宽字被横切出现半字/方框
      while (out.length && displayWidth(out) > cols - 1) out = Array.from(out).slice(0, -1).join('');
      out += '…';
      w = displayWidth(out);
    }
    while (w < cols) { out += ' '; w++; }
    return out;
  }

  function TermRenderer(canvas, textLayer, scrollEl) {
    this.canvas = canvas;
    this.textLayer = textLayer;
    this.scrollEl = scrollEl;
    this.ctx = canvas.getContext('2d');
    this.palette = PALETTES.dark;
    this.paletteName = 'dark';
    this.size = 13;
    this.family = MONO;
    this.cellW = 8;
    this.rowH = 18;
    this.frame = null;
    this.scrollback = [];
    this._lines = [];      // 与文本层逐行一致的行文本（链接命中用，M3-2）
    this._lastText = null; // 文本未变则不重写，避免 aria-live 播报与选区被清（M3-4）
    this._blinkOn = true;
    this.stick = true; // 用户停在底部时新帧自动贴底；上翻后保持偏移（UI 设计 §5.2）
    this.measure();
    var self = this;
    this.scrollEl.addEventListener('scroll', function () {
      self.stick = (self.scrollEl.scrollTop + self.scrollEl.clientHeight >= self.scrollEl.scrollHeight - 4);
    });
    if (!REDUCED) {
      setInterval(function () { self._blinkOn = !self._blinkOn; self.draw(); }, 500);
    }
  }

  TermRenderer.prototype.setPalette = function (name) {
    if (!PALETTES[name]) return;
    this.palette = PALETTES[name];
    this.paletteName = name;
    document.documentElement.style.setProperty('--term-selection', this.palette.selection);
    document.documentElement.style.setProperty('--term-bg', this.palette.bg);
    document.documentElement.style.setProperty('--term-fg', this.palette.fg);
    this.draw();
  };

  TermRenderer.prototype.setFontSize = function (px) {
    this.size = px;
    this.measure();
    this.draw();
  };

  TermRenderer.prototype.measure = function () {
    this.ctx.font = this.size + 'px ' + this.family;
    this.cellW = this.ctx.measureText('M').width || this.size * 0.6;
    this.rowH = Math.max(Math.round(this.size * 1.4), 14);
    this.textLayer.style.fontFamily = this.family;
    this.textLayer.style.fontSize = this.size + 'px';
    this.textLayer.style.lineHeight = this.rowH + 'px';
  };

  /* 按可视区算本端应请求的网格（PRD P-05/P-19：钳制 20–200 列 / 8–200 行） */
  TermRenderer.prototype.measureFit = function () {
    var r = this.scrollEl.getBoundingClientRect();
    var cols = Math.floor((r.width - PAD * 2) / this.cellW);
    var rows = Math.floor((r.height - PAD * 2) / this.rowH);
    if (!isFinite(cols) || cols < 2) cols = 45;
    if (!isFinite(rows) || rows < 2) rows = 28;
    return {
      cols: Math.max(20, Math.min(200, cols)),
      rows: Math.max(8, Math.min(200, rows))
    };
  };

  TermRenderer.prototype.setScrollback = function (lines) {
    this.scrollback = lines || [];
  };

  TermRenderer.prototype.setFrame = function (frame) {
    this.frame = frame;
    this.draw();
  };

  TermRenderer.prototype._rowText = function (row) {
    var s = '', cells = row || [];
    for (var i = 0; i < cells.length; i++) {
      var c = cells[i];
      if (c.ph) continue;
      s += c.ch == null ? ' ' : c.ch;
    }
    return s;
  };

  TermRenderer.prototype._dpr = function () { return global.devicePixelRatio || 1; };

  /* ---------- 行内链接（M3-2；口径对齐 terminal_links.cpp 的 URL 分支） ----------
   * 文本层仍是单块 textContent（整体可读可复制）；命中按「行号 + 列号」换算。 */
  var LINK_RE = /https?:\/\/[^\s"'<>|`]+/gi;
  var TRAIL_ASCII = '.,;:!?)]}';
  var TRAIL_FULL = ['\uFF0C', '\u3002', '\uFF1B', '\uFF1A', '\uFF01', '\uFF1F', '\uFF09', '\u3011', '\u2026'];

  function stripTrailPunct(s) {
    for (;;) {
      if (!s) return s;
      if (TRAIL_ASCII.indexOf(s.charAt(s.length - 1)) >= 0) { s = s.slice(0, -1); continue; }
      var hit = false;
      for (var i = 0; i < TRAIL_FULL.length; i++) {
        if (s.slice(-TRAIL_FULL[i].length) === TRAIL_FULL[i]) {
          s = s.slice(0, -TRAIL_FULL[i].length);
          hit = true;
          break;
        }
      }
      if (!hit) return s;
    }
  }

  TermRenderer.prototype.lineAtRow = function (row) {
    if (!this._lines || row < 0 || row >= this._lines.length) return null;
    return this._lines[row];
  };

  /* 行 + 列 → 链接；rect 为文本层内坐标（PAD 起点），供下划线定位 */
  TermRenderer.prototype.linkAt = function (row, col) {
    var line = this.lineAtRow(row);
    if (line === null) return null;
    LINK_RE.lastIndex = 0;
    var m;
    while ((m = LINK_RE.exec(line))) {
      var target = stripTrailPunct(m[0]);
      if (!target) continue;
      var colBegin = displayWidth(line.slice(0, m.index));
      var colEnd = colBegin + displayWidth(target);
      if (col >= colBegin && col < colEnd) {
        return {
          target: target, row: row, colBegin: colBegin, colEnd: colEnd,
          rect: {
            left: PAD + colBegin * this.cellW,
            top: PAD + row * this.rowH,
            width: (colEnd - colBegin) * this.cellW,
            height: this.rowH
          }
        };
      }
    }
    return null;
  };

  /* 文本层坐标（clientX/clientY）→ 链接 */
  TermRenderer.prototype.linkAtPoint = function (clientX, clientY) {
    if (!this._lines || !this._lines.length) return null;
    var r = this.textLayer.getBoundingClientRect();
    var x = clientX - r.left - PAD;
    var y = clientY - r.top - PAD;
    if (x < 0 || y < 0) return null;
    var row = Math.floor(y / this.rowH);
    var col = Math.floor(x / this.cellW);
    return this.linkAt(row, col);
  };

  /* 测试/定位用：按目标串在文本层里找链接矩形，返回视口坐标系（client 坐标）。
   * 取最后一次出现（输出的结果行通常在命令回显之后）。 */
  TermRenderer.prototype.linkRectFor = function (target) {
    if (!this._lines || !target) return null;
    var base = this.textLayer.getBoundingClientRect();
    for (var row = this._lines.length - 1; row >= 0; row--) {
      var idx = this._lines[row].indexOf(target);
      if (idx < 0) continue;
      var colBegin = displayWidth(this._lines[row].slice(0, idx));
      var cols = displayWidth(target);
      return {
        left: base.left + PAD + colBegin * this.cellW,
        top: base.top + PAD + row * this.rowH,
        width: cols * this.cellW,
        height: this.rowH,
        x: base.left + PAD + (colBegin + cols / 2) * this.cellW,
        y: base.top + PAD + row * this.rowH + this.rowH / 2
      };
    }
    return null;
  };

  TermRenderer.prototype.draw = function () {
    var f = this.frame;
    var ctx = this.ctx;
    if (!ctx) return;
    var cols = f ? f.cols : 0;
    var visRows = f ? f.rows : 0;
    var totalRows = this.scrollback.length + visRows;
    if (cols <= 0) {
      this._lines = [];
      if (this._lastText !== '') { this.textLayer.textContent = ''; this._lastText = ''; }
      return;
    }

    var cssW = Math.ceil(cols * this.cellW) + PAD * 2;
    var cssH = Math.ceil(totalRows * this.rowH) + PAD * 2;
    var dpr = this._dpr();
    if (this.canvas.width !== Math.round(cssW * dpr) || this.canvas.height !== Math.round(cssH * dpr)) {
      this.canvas.width = Math.round(cssW * dpr);
      this.canvas.height = Math.round(cssH * dpr);
      this.canvas.style.width = cssW + 'px';
      this.canvas.style.height = cssH + 'px';
    }

    var atBottom = this.stick;

    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.fillStyle = this.palette.bg;
    ctx.fillRect(0, 0, cssW, cssH);

    // 文本层：行内容 + 与画布相同的字号/行高/内边距，颜色透明只用于原生选择
    // 逐行缓存（链接命中按行取文本）；内容不变不重写 textContent。
    var lines = [];
    for (var s = 0; s < this.scrollback.length; s++) lines.push(fitLine(this.scrollback[s], cols));
    for (var r = 0; r < visRows; r++) lines.push(fitLine(this._rowText(f.lines[r]), cols));
    this._lines = lines;
    var text = lines.join('\n');
    if (text !== this._lastText) {
      this.textLayer.textContent = text;
      this._lastText = text;
    }
    this.textLayer.style.padding = PAD + 'px';
    this.textLayer.style.width = cssW + 'px';
    this.textLayer.style.height = cssH + 'px';

    ctx.save();
    ctx.beginPath();
    ctx.rect(PAD, PAD, cols * this.cellW, totalRows * this.rowH);
    ctx.clip();
    ctx.textBaseline = 'middle';

    // 回滚区：纯文本、默认前景色（技术卡 §5.4）
    ctx.fillStyle = this.palette.dim;
    ctx.font = this.size + 'px ' + this.family;
    for (var h = 0; h < this.scrollback.length; h++) {
      var line = fitLine(this.scrollback[h], cols);
      var y = PAD + h * this.rowH + this.rowH / 2;
      ctx.fillText(line, PAD, y);
    }

    // 可视网格
    var baseRow = this.scrollback.length;
    for (var row = 0; row < visRows; row++) {
      var cells = f.lines[row] || [];
      var x = PAD;
      var yTop = PAD + (baseRow + row) * this.rowH;
      var yMid = yTop + this.rowH / 2;
      for (var c = 0; c < cols; c++) {
        var cell = cells[c] || { ch: ' ', w: 1 };
        var w = (cell.w === 2 ? 2 : 1) * this.cellW;
        if (cell.ph) { x += 0; continue; }
        if (cell.bg) {
          ctx.fillStyle = cell.bg;
          ctx.fillRect(x, yTop, w, this.rowH);
        }
        if (cell.ch && cell.ch !== ' ') {
          ctx.fillStyle = cell.fg || this.palette.fg;
          ctx.font = (cell.bold ? '700 ' : '') + this.size + 'px ' + this.family;
          ctx.fillText(cell.ch, x, yMid);
        }
        if (cell.underline) {
          ctx.fillStyle = cell.fg || this.palette.fg;
          ctx.fillRect(x, yTop + this.rowH - 2, w, 1);
        }
        x += w;
      }
    }

    // 光标：1ch × 行高实心块（减弱动效时常亮）
    if (f.cursorVisible && (REDUCED || this._blinkOn)) {
      var cx = PAD + Math.min(f.cursorCol, cols - 1) * this.cellW;
      var cy = PAD + (baseRow + f.cursorRow) * this.rowH;
      ctx.fillStyle = this.palette.cursor;
      ctx.fillRect(cx, cy, this.cellW, this.rowH);
    }

    ctx.restore();

    if (atBottom) {
      this.stick = true;
      this.scrollEl.scrollTop = this.scrollEl.scrollHeight;
    }
  };

  global.WT = global.WT || {};
  global.WT.TermRenderer = TermRenderer;
  global.WT.PALETTES = PALETTES;
})(typeof window !== 'undefined' ? window : globalThis);
