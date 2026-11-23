/* 网页终端交互骨架（need#490）——mock 数据驱动，不发起任何网络请求。
 *
 * 接线契约见 protocol.js：connect()/sendInput()/onFrame()。
 * 真实实现替换 MockClient 即可，本文件只依赖该契约形状。
 */
(function () {
  'use strict';

  var $ = function (id) { return document.getElementById(id); };
  var WT = window.WT;
  var IS_LOOPBACK = ['127.0.0.1', 'localhost', '::1'].indexOf(location.hostname) >= 0;

  var tabsEl = $('tabs');
  var canvasScroll = $('canvas-scroll');
  var canvas = $('term-canvas');
  var textLayer = $('text-layer');
  var skeleton = $('skeleton');
  var emptyEl = $('empty');
  var reconnectBar = $('reconnect-bar');
  var reconnectText = $('reconnect-text');
  var connDot = $('conn-dot');
  var connText = $('conn-text');
  var menuEl = $('menu');
  var menuScrim = $('menu-scrim');
  var terminalView = $('view-terminal');
  var configView = $('view-config');
  var handshakeOv = $('ov-handshake');
  var disconnectedOv = $('ov-disconnected');
  var revokedOv = $('ov-revoked');
  var pasteScrim = $('paste-scrim');
  var pasteDialog = $('paste-dialog');
  var pasteText = $('paste-text');
  var pasteHint = $('paste-hint');
  var pasteErr = $('paste-err');

  var client = new WT.MockClient();
  var renderer = new WT.TermRenderer(canvas, textLayer, canvasScroll);
  renderer.setPalette('dark');

  var selecting = false;
  var pressTimer = null;
  var pressStart = null;
  var wasSelecting = false;
  var hasConnected = false;
  var toastTimer = null;
  var resizeTimer = null;

  /* ---------- 隐藏输入：承接软键盘与 IME（UI 设计 §5.2 / PRD F-12） ---------- */
  var kbd = document.createElement('input');
  kbd.type = 'text';
  kbd.autocomplete = 'off';
  kbd.setAttribute('autocapitalize', 'off');
  kbd.setAttribute('autocorrect', 'off');
  kbd.setAttribute('spellcheck', 'false');
  kbd.style.cssText = 'position:fixed;left:0;bottom:0;width:1px;height:1px;opacity:0;border:0;padding:0;font-size:16px;';
  document.body.appendChild(kbd);
  var composing = false;

  function focusKeyboard() { if (!selecting) kbd.focus(); }
  canvasScroll.addEventListener('click', focusKeyboard);
  kbd.addEventListener('compositionstart', function () { composing = true; });
  kbd.addEventListener('compositionend', function (e) {
    composing = false;
    var v = e.data != null ? e.data : kbd.value;
    kbd.value = '';
    if (v) client.sendInput(v);
  });
  kbd.addEventListener('input', function () {
    if (composing) return;
    var v = kbd.value; kbd.value = '';
    if (v) client.sendInput(v);
  });
  kbd.addEventListener('keydown', function (e) {
    if (composing) return;
    var b = WT.keyToBytes(e);
    if (b) { e.preventDefault(); client.sendInput(b); }
  });

  /* ---------- 长按选择模式（PRD P-21：500ms 进入，10px 内判滚动） ---------- */
  function enterSelecting() {
    selecting = true;
    canvasScroll.classList.add('selecting');
    kbd.blur();
  }
  function exitSelecting() {
    selecting = false;
    canvasScroll.classList.remove('selecting');
    var sel = window.getSelection && window.getSelection();
    if (sel) sel.removeAllRanges();
  }
  canvasScroll.addEventListener('pointerdown', function (e) {
    if (e.pointerType === 'mouse') return; // 桌面对直接原生选择
    wasSelecting = selecting;
    pressStart = { x: e.clientX, y: e.clientY };
    if (selecting) return;
    clearTimeout(pressTimer);
    pressTimer = setTimeout(enterSelecting, 500);
  });
  canvasScroll.addEventListener('pointermove', function (e) {
    if (!pressStart || selecting) return;
    var dx = e.clientX - pressStart.x;
    var dy = e.clientY - pressStart.y;
    if (Math.sqrt(dx * dx + dy * dy) > 10) { clearTimeout(pressTimer); pressStart = null; } // 判为滚动
  });
  function endPress() {
    clearTimeout(pressTimer);
    if (selecting && wasSelecting) {
      var sel = window.getSelection && window.getSelection();
      if (!sel || sel.toString().length === 0) exitSelecting();
    }
    pressStart = null;
  }
  canvasScroll.addEventListener('pointerup', endPress);
  canvasScroll.addEventListener('pointercancel', function () { clearTimeout(pressTimer); pressStart = null; });

  /* ---------- 连接状态与两段式断线 ---------- */
  function setConn(state) {
    connDot.className = 'dot' + (state === 'warn' ? ' warn' : state === 'err' ? ' err' : '');
    connText.textContent = state === 'warn' ? '重连中' : state === 'err' ? '已断开' : '已连接';
  }

  client.onState(function (s) {
    if (s.status === 'connecting') {
      if (!hasConnected) handshakeOv.hidden = false;
      reconnectBar.hidden = !hasConnected;
      disconnectedOv.hidden = true;
      setConn('warn');
    } else if (s.status === 'connected') {
      hasConnected = true;
      handshakeOv.hidden = true;
      reconnectBar.hidden = true;
      disconnectedOv.hidden = true;
      setConn('ok');
    } else if (s.status === 'reconnecting') {
      reconnectBar.hidden = false;             // ≤1 秒细条：状态回调即时出现
      reconnectText.textContent = '连接中断，正在重连（第 ' + (client._rcAttempts + 1) + ' 次）';
      disconnectedOv.hidden = true;
      setConn('warn');
    } else if (s.status === 'closed') {
      reconnectBar.hidden = true;
      disconnectedOv.hidden = false;           // 连续 2 次失败（约 3 秒）全屏
      setConn('err');
    }
    renderTabs(s);
  });

  client.onFrame(function (frame) {
    skeleton.hidden = true;
    renderer.setScrollback(client.scrollback);
    renderer.setFrame(frame);
  });

  client.onSettings(function (s) { applySettings(s); });

  /* ---------- tab 顶栏 ---------- */
  function renderTabs(s) {
    tabsEl.innerHTML = '';
    for (var i = 0; i < s.tabs.length; i++) {
      var t = s.tabs[i];
      var on = t.id === s.activeId;
      var tab = document.createElement('div');
      tab.className = 'tab' + (on ? ' on' : '');
      tab.setAttribute('role', 'tab');
      tab.setAttribute('tabindex', '0');
      tab.setAttribute('aria-selected', on ? 'true' : 'false');
      tab.textContent = t.title;
      (function (id) { tab.addEventListener('click', function () { client.selectTab(id); }); })(t.id);
      if (on) {
        var x = document.createElement('button');
        x.className = 'tab-x';
        x.textContent = '×';
        x.setAttribute('aria-label', '关闭 ' + t.title);
        (function (id) { x.addEventListener('click', function (ev) { ev.stopPropagation(); client.closeTab(id); }); })(t.id);
        tab.appendChild(x);
      }
      tabsEl.appendChild(tab);
      if (on) tab.scrollIntoView({ inline: 'nearest', block: 'nearest' });
    }
    emptyEl.hidden = s.tabs.length > 0;
  }

  $('btn-new').addEventListener('click', function () { client.newTab(); });
  $('btn-new-empty').addEventListener('click', function () { client.newTab(); });

  /* ---------- 菜单 ---------- */
  function openMenu() { menuMar(); menuEl.hidden = false; }
  function closeMenu() { menuEl.hidden = true; menuScrim.hidden = true; }
  function menuMar() { menuScrim.hidden = false; }
  $('btn-menu').addEventListener('click', function (e) { e.stopPropagation(); menuEl.hidden ? openMenu() : closeMenu(); });
  menuScrim.addEventListener('click', closeMenu);
  menuEl.addEventListener('click', function (e) {
    var act = e.target && e.target.getAttribute('data-act');
    if (!act) return;
    closeMenu();
    if (act === 'config') openConfig();
    else if (act === 'logout') { exitSelecting(); showToast('已退出登录（原型占位）'); }
  });

  /* ---------- 粘贴（P-23：局域网手动框 / 回环可预填） ---------- */
  function openPaste() {
    pasteErr.hidden = true;
    pasteText.value = '';
    if (IS_LOOPBACK && navigator.clipboard && navigator.clipboard.readText) {
      pasteHint.textContent = '已从剪贴板预填，可编辑后发送。';
      navigator.clipboard.readText().then(function (t) {
        pasteText.value = t || '';
      }).catch(function () {
        pasteHint.textContent = '局域网下浏览器不提供剪贴板读取，请手动粘贴到下方。';
      });
    } else {
      pasteHint.textContent = '局域网下浏览器不提供剪贴板读取，请手动粘贴到下方。';
    }
    pasteScrim.hidden = false;
    pasteDialog.hidden = false;
    pasteText.focus();
  }
  function closePaste() { pasteScrim.hidden = true; pasteDialog.hidden = true; }
  $('btn-paste').addEventListener('click', openPaste);
  $('paste-cancel').addEventListener('click', closePaste);
  $('paste-send').addEventListener('click', function () {
    var text = pasteText.value;
    if (!text) { closePaste(); return; }
    client.paste(text);            // bracketed paste：多行按原文插入
    closePaste();
  });

  /* ---------- 断线覆盖层按钮 ---------- */
  function reconnect() { client.reconnect(); }
  $('btn-reconnect-now').addEventListener('click', reconnect);
  $('ov-reconnect').addEventListener('click', reconnect);
  $('ov-refresh').addEventListener('click', function () { location.reload(); });

  /* ---------- 配置页 ---------- */
  var cfgFont = $('cfg-font');
  var cfgSize = $('cfg-size');
  var cfgSizeVal = $('cfg-size-val');
  var cfgSizeSpin = $('cfg-size-spin');
  var cfgMode = $('cfg-mode');
  var cfgPalette = $('cfg-palette');
  var cfgShell = $('cfg-shell');
  var cfgDir = $('cfg-dir');
  var cfgDirClear = $('cfg-dir-clear');
  var cfgDirErr = $('cfg-dir-err');
  var cfgBanner = $('cfg-banner');
  var shellOptions = /Win/i.test(navigator.platform || '')
    ? ['默认（PowerShell）', 'cmd.exe', 'Git Bash']
    : ['默认（zsh）', '/bin/bash', '/bin/sh'];

  function applySettings(s) {
    cfgFont.value = s.font;
    cfgSize.value = s.fontSize;
    cfgSizeVal.textContent = s.fontSize;
    var segs = cfgMode.querySelectorAll('span');
    for (var i = 0; i < segs.length; i++) segs[i].classList.toggle('on', segs[i].getAttribute('data-v') === s.colorMode);
    cfgPalette.disabled = s.colorMode !== 'independent';
    cfgPalette.value = s.palette;
    cfgShell.innerHTML = '';
    for (var j = 0; j < shellOptions.length; j++) {
      var o = document.createElement('option');
      o.textContent = shellOptions[j]; o.value = shellOptions[j];
      if (shellOptions[j] === s.shell) o.selected = true;
      cfgShell.appendChild(o);
    }
    cfgDir.value = s.startupDir || '';
    cfgDirClear.hidden = !s.startupDir;
    cfgDirErr.hidden = true;
    if (s.colorMode === 'independent') renderer.setPalette(s.palette);
    else renderer.setPalette('dark');
  }

  function saveSetting(patch, spin) {
    if (spin) spin.hidden = false;
    cfgBanner.hidden = true;
    return client.writeSettings(patch).then(function () {
      if (spin) spin.hidden = true;
    }).catch(function (err) {
      if (spin) spin.hidden = true;
      if (err && err.field === 'startupDir') {
        cfgDirErr.textContent = '目录不存在或无法进入';
        cfgDirErr.hidden = false;
        cfgDir.value = client.settings.startupDir || ''; // 保留原值
      } else {
        cfgBanner.hidden = false;
        setTimeout(function () { cfgBanner.hidden = true; }, 3000);
      }
    });
  }

  function openConfig() {
    exitSelecting();
    terminalView.hidden = true;
    configView.hidden = false;
    applySettings(client.settings);
  }
  function closeConfig() {
    configView.hidden = true;
    terminalView.hidden = false;
    applyGrid();
  }
  $('cfg-back').addEventListener('click', closeConfig);
  cfgFont.addEventListener('change', function () { saveSetting({ font: cfgFont.value }); });
  cfgSize.addEventListener('input', function () {
    cfgSizeVal.textContent = cfgSize.value;
    renderer.setFontSize(parseInt(cfgSize.value, 10));
  });
  cfgSize.addEventListener('change', function () { saveSetting({ fontSize: parseInt(cfgSize.value, 10) }, cfgSizeSpin); });
  cfgMode.addEventListener('click', function (e) {
    var v = e.target && e.target.getAttribute('data-v');
    if (!v) return;
    saveSetting({ colorMode: v }).then(function () { applySettings(client.settings); });
  });
  cfgPalette.addEventListener('change', function () { saveSetting({ palette: cfgPalette.value }); });
  cfgShell.addEventListener('change', function () { saveSetting({ shell: cfgShell.value }); });
  cfgDir.addEventListener('change', function () {
    saveSetting({ startupDir: cfgDir.value.trim() });
  });
  cfgDir.addEventListener('keydown', function (e) { if (e.key === 'Enter') cfgDir.blur(); });
  cfgDirClear.addEventListener('click', function () {
    cfgDir.value = '';
    saveSetting({ startupDir: '' });
  });

  /* ---------- Toast ---------- */
  var toastEl = $('toast');
  function showToast(msg) {
    toastEl.textContent = msg;
    toastEl.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { toastEl.hidden = true; }, 1500);
  }

  /* ---------- 尺寸协同（键盘/旋转，去抖 150ms） ---------- */
  function syncAppHeight() {
    var vv = window.visualViewport;
    var app = $('app');
    // 只在软键盘确实压缩可视区时覆盖，避免把 d vh 的即时值锁成过期像素
    if (vv && innerHeight - vv.height > 80) app.style.height = vv.height + 'px';
    else app.style.height = '';
  }
  function applyGrid() {
    if (!configView.hidden) return;
    var fit = renderer.measureFit();
    client.resize(fit.cols, fit.rows);
  }
  function onResize() {
    syncAppHeight();
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(function () {
      if (!configView.hidden) return;
      var fit = renderer.measureFit();
      client.resize(fit.cols, fit.rows);
      renderer.draw();
    }, 150);
  }
  window.addEventListener('resize', onResize);
  window.addEventListener('orientationchange', onResize);
  if (window.visualViewport) {
    window.visualViewport.addEventListener('resize', onResize);
    window.visualViewport.addEventListener('scroll', syncAppHeight);
  }
  if (window.ResizeObserver) {
    var ro = new ResizeObserver(function () { onResize(); });
    ro.observe(canvasScroll);
  }

  /* ---------- 启动 ---------- */
  syncAppHeight();
  client.connect().then(function () {
    hasConnected = true;
    handshakeOv.hidden = true;
    applyGrid();                       // 首次按可视区请求网格（随后服务端回 full）
    applySettings(client.settings);
  });

  /* ---------- 原型截图/走查入口（?view=config / ?paste=lan / ?mock=…） ---------- */
  var q = new URLSearchParams(location.search);
  if (q.get('view') === 'config') setTimeout(openConfig, 120);
  if (q.get('paste')) setTimeout(openPaste, 700);
  if (q.get('mock') === 'disconnect') setTimeout(function () { client.simulateDisconnect(); }, 900);
  if (q.get('mock') === 'closed') setTimeout(function () { disconnectedOv.hidden = false; reconnectBar.hidden = true; setConn('err'); }, 700);
  if (q.get('mock') === 'menu') setTimeout(openMenu, 700);
  if (q.get('mock') === 'revoked') setTimeout(function () { revokedOv.hidden = false; }, 700);

  window.__wt = { client: client, renderer: renderer };
})();
