/* 网页终端交互（need#490 W5）——默认装配 RealClient；?demo=1 仅用于截图走查。
 *
 * 契约：protocol.js 的 connect()/login()/bindConfirm()/sendInput()/onFrame()/onState()。
 * 视图：登录 → 绑定（携带 QR 令牌）→ 终端 → 配置；全屏覆盖为终态层，不在导航栈。
 */
(function () {
  'use strict';

  var $ = function (id) { return document.getElementById(id); };
  var WT = window.WT;
  var q = new URLSearchParams(location.search);
  var IS_LOOPBACK = ['127.0.0.1', 'localhost', '::1'].indexOf(location.hostname) >= 0;
  var DEMO = q.get('demo') === '1';
  var demoView = q.get('view');
  var hasConnected = false;
  var currentView = 'handshake';
  var savedScroll = null;
  var toastTimer = null;

  var appEl = $('app');
  var loginView = $('view-login');
  var bindView = $('view-bind');
  var terminalView = $('view-terminal');
  var configView = $('view-config');
  var tabsEl = $('tabs');
  var canvasScroll = $('canvas-scroll');
  var canvas = $('term-canvas');
  var textLayer = $('text-layer');
  var skeleton = $('skeleton');
  var emptyEl = $('empty');
  var exitCard = $('exit-card');
  var exitTitle = $('exit-title');
  var emptyTitle = $('empty-title');
  var btnNewEmpty = $('btn-new-empty');
  var reconnectBar = $('reconnect-bar');
  var reconnectText = $('reconnect-text');
  var connDot = $('conn-dot');
  var connText = $('conn-text');
  var pendingBadge = $('pending-badge');
  var menuEl = $('menu');
  var menuScrim = $('menu-scrim');
  var handshakeOv = $('ov-handshake');
  var connErrorOv = $('ov-conn-error');
  var connErrorBody = $('conn-error-body');
  var disconnectedOv = $('ov-disconnected');
  var discTitle = $('disc-title');
  var discBody = $('disc-body');
  var revokedOv = $('ov-revoked');
  var limitOv = $('ov-limit');
  var limitTitle = $('limit-title');
  var limitBody = $('limit-body');
  var limitPrimary = $('limit-primary');
  var limitSecondary = $('limit-secondary');
  var keybar = $('keybar');
  var pasteScrim = $('paste-scrim');
  var pasteDialog = $('paste-dialog');
  var pasteText = $('paste-text');
  var pasteHint = $('paste-hint');
  var pasteErr = $('paste-err');
  var selftestEl = $('selftest');
  var loginInput = $('login-input');
  var loginCells = $('login-cells');
  var loginErr = $('login-err');
  var loginHint = $('login-hint');
  var loginRetry = $('login-retry');
  var bindInput = $('bind-input');
  var bindCells = $('bind-cells');
  var bindErr = $('bind-err');
  var bindBanner = $('bind-banner');
  var bindConfirm = $('bind-confirm');
  var bindOpen = $('bind-open');
  var bindDead = $('bind-dead');
  var bindDeadTitle = $('bind-dead-title');
  var bindDeadBody = $('bind-dead-body');
  var bindCodeWrap = bindView.querySelector('.code-wrap');
  var platformHint = bindView.querySelector('.platform-hint');
  var state = { status: 'idle', view: 'handshake', tabs: [], activeId: null, pending: 0 };
  var loginBusy = false, bindBusy = false, lockLeft = 0, lockTimer = null;

  var client = DEMO ? new WT.MockClient() : new WT.RealClient();
  var renderer = new WT.TermRenderer(canvas, textLayer, canvasScroll);
  renderer.setPalette('dark');

  /* ---------- 自检（?selftest=1） ---------- */
  if (q.get('selftest') === '1' && window.WTCrypto) {
    var st = window.WTCrypto.selfTest();
    selftestEl.hidden = false;
    selftestEl.className = 'selftest ' + (st.ok ? 'pass' : 'fail');
    selftestEl.textContent = (st.ok ? 'PASS ' : 'FAIL ') +
      st.results.filter(function (r) { return !r.ok; }).map(function (r) { return r.name + ': ' + r.detail; }).join(' | ');
  }

  /* ---------- 隐藏输入：软键盘与 IME ---------- */
  var kbd = document.createElement('input');
  kbd.type = 'text';
  kbd.autocomplete = 'off';
  kbd.setAttribute('autocapitalize', 'off');
  kbd.setAttribute('autocorrect', 'off');
  kbd.setAttribute('spellcheck', 'false');
  kbd.style.cssText = 'position:fixed;left:0;bottom:0;width:1px;height:1px;opacity:0;border:0;padding:0;font-size:16px;';
  document.body.appendChild(kbd);
  var composing = false, composedJustEnded = false;
  var selecting = false, pressTimer = null, pressStart = null, wasSelecting = false;
  function focusKeyboard() { if (!selecting && currentView === 'terminal') kbd.focus(); }
  canvasScroll.addEventListener('click', focusKeyboard);
  kbd.addEventListener('compositionstart', function () { composing = true; });
  kbd.addEventListener('compositionend', function (e) {
    composing = false;
    composedJustEnded = true;
    setTimeout(function () { composedJustEnded = false; }, 0);
    var v = e.data != null ? e.data : kbd.value;
    kbd.value = '';
    if (v) client.sendInput(v);
  });
  kbd.addEventListener('input', function () {
    if (composing) return;
    if (composedJustEnded) { kbd.value = ''; return; } // compositionend 后部分浏览器补发 input
    var v = kbd.value; kbd.value = '';
    if (v) client.sendInput(v);
  });
  kbd.addEventListener('keydown', function (e) {
    if (composing) return;
    var b = WT.keyToBytes(e);
    if (b) { e.preventDefault(); client.sendInput(b); }
  });

  /* ---------- 长按选择模式 ---------- */
  function enterSelecting() { selecting = true; canvasScroll.classList.add('selecting'); kbd.blur(); }
  function exitSelecting() {
    selecting = false;
    canvasScroll.classList.remove('selecting');
    var sel = window.getSelection && window.getSelection();
    if (sel) sel.removeAllRanges();
  }
  canvasScroll.addEventListener('pointerdown', function (e) {
    if (e.pointerType === 'mouse') return;
    wasSelecting = selecting;
    pressStart = { x: e.clientX, y: e.clientY };
    if (selecting) return;
    clearTimeout(pressTimer);
    pressTimer = setTimeout(enterSelecting, 500);
  });
  canvasScroll.addEventListener('pointermove', function (e) {
    if (!pressStart || selecting) return;
    var dx = e.clientX - pressStart.x, dy = e.clientY - pressStart.y;
    if (Math.sqrt(dx * dx + dy * dy) > 10) { clearTimeout(pressTimer); pressStart = null; }
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

  /* ---------- 视图路由 ---------- */
  function setView(name) {
    currentView = name;
    loginView.hidden = name !== 'login';
    bindView.hidden = name !== 'bind';
    terminalView.hidden = name !== 'terminal';
    configView.hidden = name !== 'config';
    handshakeOv.hidden = name !== 'handshake';
    connErrorOv.hidden = name !== 'error';
    revokedOv.hidden = name !== 'revoked';
    if (name !== 'terminal' && name !== 'config') { disconnectedOv.hidden = true; reconnectBar.hidden = true; }
    if (name === 'login') setTimeout(function () { if (!loginInput.disabled) loginInput.focus(); }, 40);
    if (name === 'bind') setTimeout(function () { if (!bindInput.disabled) bindInput.focus(); }, 40);
    if (name === 'terminal') {
      skeleton.hidden = !!renderer.frame;
      applyGrid();
    }
  }

  function saveScrollOnce() {
    if (savedScroll === null && currentView === 'terminal') {
      savedScroll = { top: canvasScroll.scrollTop, stick: renderer.stick };
    }
  }
  function restoreScroll() {
    if (savedScroll && !savedScroll.stick) canvasScroll.scrollTop = savedScroll.top;
    savedScroll = null;
  }

  function setConn(kind) {
    connDot.className = 'dot' + (kind === 'warn' ? ' warn' : kind === 'err' ? ' err' : '');
    connText.textContent = kind === 'warn' ? '重连中' : kind === 'err' ? '已断开' : '已连接';
  }

  function applyState(s) {
    var view = (DEMO && demoView) ? demoView : (s.view || (s.status === 'error' ? 'error' : 'handshake'));
    if (view === 'config') view = 'config';
    if (view === 'terminal' && configView.hidden === false) view = 'config';
    setView(view);
    if (view === 'login') applyLoginState(s);
    if (view === 'bind') applyBindState(s);
    if (view === 'error') connErrorBody.textContent = describeConnError(s.error);
    renderTabs(s);
    if (exitCard) {
      exitCard.hidden = !s.exit;
      if (s.exit) {
        exitTabId = (s.exit.tabIndex !== undefined && s.exit.tabIndex !== null)
          ? String(s.exit.tabIndex) : null;
        if (s.exit.reason) {
          exitTitle.textContent = '终端启动失败：' + s.exit.reason;
        } else {
          exitTitle.textContent = '终端已退出' +
            (s.exit.exitCode !== undefined && s.exit.exitCode !== null ? '（退出码 ' + s.exit.exitCode + '）' : '');
        }
      }
    }
    if (view === 'terminal' && s.tabs.length === 0) {
      // P1-3：桌面重启后的空态文案；普通空态仍是「暂无终端」。
      emptyTitle.textContent = s.emptyReason === 'restart'
        ? '桌面端已重启，会话已全部结束' : '暂无终端';
    }
    updatePending(s);
    updateConn(s);
    twoStage(s);
    handleAsyncErrors(s);
    if (s.resynced) restoreScroll();
  }

  function updateConn(s) {
    if (s.status === 'connected') setConn('ok');
    else if (s.status === 'reconnecting' || s.status === 'connecting') setConn('warn');
    else if (s.status === 'closed' || s.status === 'error') setConn('err');
  }

  function twoStage(s) {
    if (s.view !== 'terminal' && s.status !== 'reconnecting' && s.status !== 'closed') return;
    if (s.status === 'reconnecting' || (s.status === 'connecting' && hasConnected)) {
      saveScrollOnce();
      reconnectBar.hidden = false;
      reconnectText.textContent = '连接中断，正在重连（第 ' + Math.max(1, (client._attempts || 0)) + ' 次）';
      disconnectedOv.hidden = true;
    } else if (s.status === 'closed') {
      saveScrollOnce();
      reconnectBar.hidden = true;
      disconnectedOv.hidden = false;
      discTitle.textContent = hasConnected ? '桌面端已关闭网页服务' : '无法连接桌面端';
      discBody.textContent = hasConnected
        ? '服务重新开启后，页面会自动恢复；若桌面端仍在运行，请检查网络或端口是否变更。'
        : '请检查网络或端口是否变更。';
    } else if (s.status === 'connected') {
      reconnectBar.hidden = true;
      disconnectedOv.hidden = true;
    }
  }

  function describeConnError(err) {
    if (err && err.http === 429) return '连接请求过于频繁，请稍后再试。';
    if (err && err.http === 400) return '握手参数被拒绝，请刷新页面后重试。';
    return '请确认桌面端网页服务已开启，且手机与电脑在同一网络。';
  }

  function updatePending(s) {
    var n = s.pending || 0;
    pendingBadge.hidden = n <= 0;
    pendingBadge.textContent = n > 0 ? '未发送 ' + n : '';
  }

  function handleAsyncErrors(s) {
    if (s.opError && s.opError.code === 'limit_session') showLimit('session');
    if (s.sendError) showToast('发送失败，请重试');
    if (s.queueDropped) showToast('部分输入未送达');
    if (s.error && (s.error.http === 429 || s.error.code === 'too_many_requests')) showLimit('hs', s.error.retryAfter);
    if (s.error && s.error.code === 'too_many_sessions') showLimit('session');
    if (s.error && (s.error.http === 503 || s.error.code === 'too_many_streams')) showLimit('sse');
  }

  /* ---------- 登录页 ---------- */
  function syncCells(inputEl, cellsEl) {
    var v = (inputEl.value || '').replace(/\D/g, '').slice(0, 6);
    inputEl.value = v;
    var cells = cellsEl.children;
    for (var i = 0; i < cells.length; i++) {
      cells[i].textContent = v[i] || '';
      cells[i].classList.toggle('filled', !!v[i]);
    }
    return v;
  }
  function clearCode(inputEl, cellsEl) { inputEl.value = ''; syncCells(inputEl, cellsEl); }
  function applyLoginState(s) {
    loginHint.hidden = s.bound !== false;
    if (s.bound === false) loginHint.textContent = '请先在桌面端完成绑定';
    if (s.authError) handleAuthError(s.authError);
  }
  function setLoginErr(msg) { loginErr.textContent = msg || ''; }
  function startLock(seconds) {
    lockLeft = Math.max(1, seconds | 0 || 60);
    loginInput.disabled = true;
    loginRetry.hidden = true;
    clearInterval(lockTimer);
    var tick = function () {
      if (lockLeft <= 0) {
        clearInterval(lockTimer);
        loginInput.disabled = false;
        setLoginErr('');
        return;
      }
      setLoginErr('尝试次数过多，请 ' + lockLeft + ' 秒后再试');
      lockLeft--;
    };
    tick();
    lockTimer = setInterval(tick, 1000);
  }
  function handleAuthError(err) {
    var code = err && (err.code || err.e);
    if (code === 'bad_code' || code === 'invalid_code') {
      setLoginErr('验证码错误，请重试');
      clearCode(loginInput, loginCells);
    } else if (code === 'locked' || err.http === 429) {
      startLock(err.retryAfter || 60);
    } else if (code === 'too_many_streams') {
      showLimit('sse');
    } else if (code === 'bad_ticket') {
      setLoginErr('登录已过期，请重新验证');
      clearCode(loginInput, loginCells);
    } else if (code === 'limit_ticket' || code === 'ticket_full' || code === 'too_many_tickets') {
      setLoginErr('登录设备数已达上限');
      loginHint.hidden = false;
      loginHint.textContent = '请先退出不用的设备';
      loginRetry.hidden = false;
    } else if (code === 'limit_session' || code === 'session_full' || (err.http === 409 && code !== 'limit_ticket' && code !== 'ticket_full')) {
      setLoginErr(/ticket/i.test(code || '') ? '登录设备数已达上限' : '会话数已达上限');
    } else {
      setLoginErr('网络错误，请重试');
    }
  }
  function submitLogin() {
    if (loginBusy || lockLeft > 0) return;
    var code = loginInput.value;
    if (code.length !== 6) return;
    loginBusy = true;
    loginInput.disabled = true;
    setLoginErr('验证中…');
    client.login(code).catch(handleAuthError).then(function () {
      loginBusy = false;
      loginInput.disabled = lockLeft > 0;
    });
  }
  loginInput.addEventListener('input', function () {
    var v = syncCells(loginInput, loginCells);
    setLoginErr('');
    if (v.length === 6) submitLogin();
  });
  loginInput.addEventListener('keydown', function (e) { if (e.key === 'Enter') submitLogin(); });
  loginCells.addEventListener('click', function () { if (!loginInput.disabled) loginInput.focus(); });
  loginRetry.addEventListener('click', function () {
    setLoginErr('');
    loginHint.hidden = state.bound !== false;
    loginRetry.hidden = true;
    clearCode(loginInput, loginCells);
    loginInput.disabled = false;
    loginInput.focus();
  });

  /* ---------- 绑定页 ---------- */
  function bindCodes(err) {
    var code = err && (err.code || err.e);
    if (code === 'bind_invalid') return 'invalid';
    if (code === 'bind_expired' || code === 'token_expired') return 'expired';
    if (code === 'bind_used' || code === 'token_used' || code === 'bind_terminal') return 'used';
    if (code === 'no_token' || code === 'bad_token') return 'expired';
    return '';
  }
  function applyBindState(s) {
    if (s.bind && s.bind.otpauth) {
      bindOpen.href = s.bind.otpauth;
      bindOpen.classList.remove('disabled');
    }
    if (s.bind && s.bind.invalid) {
      bindBanner.hidden = false;
      bindBanner.textContent = '尝试次数过多，绑定已失效，请在桌面端刷新二维码';
      disableBind();
    }
    var dead = bindCodes(s.bindError);
    if (dead) {
      bindDead.hidden = false;
      bindDeadTitle.textContent = dead === 'expired' ? '链接已过期' : '链接已失效';
      bindDeadBody.textContent = dead === 'expired'
        ? '请在桌面端刷新二维码后重新扫码。'
        : '该链接已使用或验证器绑定已失效，请在桌面端重新绑定。';
      bindCodeWrap.hidden = true;
      bindConfirm.hidden = true;
      bindOpen.hidden = true;
      platformHint.hidden = true;
    }
  }
  function disableBind() {
    bindInput.disabled = true;
    bindConfirm.disabled = true;
    bindOpen.classList.add('disabled');
  }
  bindInput.addEventListener('input', function () {
    var v = syncCells(bindInput, bindCells);
    bindErr.textContent = '';
    bindConfirm.disabled = v.length !== 6 || bindInput.disabled;
  });
  bindCells.addEventListener('click', function () { if (!bindInput.disabled) bindInput.focus(); });
  bindConfirm.addEventListener('click', function () {
    if (bindBusy) return;
    var code = bindInput.value;
    if (code.length !== 6) return;
    bindBusy = true;
    bindConfirm.disabled = true;
    bindConfirm.textContent = '确认中';
    client.bindConfirm(code).catch(function (err) {
      var dead = bindCodes(err);
      if (dead === 'invalid') {
        bindBanner.hidden = false;
        bindBanner.textContent = '尝试次数过多，绑定已失效，请在桌面端刷新二维码';
        disableBind();
        return;
      }
      if (dead) return;
      var c = err && (err.code || err.e);
      if (c === 'bad_code' || c === 'invalid_code') {
        bindErr.textContent = '验证码错误，请重试';
        clearCode(bindInput, bindCells);
      } else if (c === 'limit_ticket' || c === 'ticket_full' || c === 'too_many_tickets') {
        bindErr.textContent = '登录设备数已达上限';
      } else {
        bindErr.textContent = '网络错误，请重试';
      }
    }).then(function () {
      bindBusy = false;
      bindConfirm.textContent = '完成绑定';
      bindConfirm.disabled = bindInput.value.length !== 6 || bindInput.disabled;
    });
  });
  bindOpen.addEventListener('click', function (e) {
    if (!bindOpen.href || bindOpen.href === '#' || bindOpen.classList.contains('disabled')) e.preventDefault();
  });

  /* ---------- tab 顶栏 ---------- */
  var lastTap = { id: null, at: 0 };
  function renderTabs(s) {
    tabsEl.innerHTML = '';
    for (var i = 0; i < s.tabs.length; i++) {
      var t = s.tabs[i];
      var on = t.id === s.activeId;
      var tab = document.createElement('div');
      tab.className = 'tab' + (on ? ' on' : '');
      tab.setAttribute('role', 'tab');
      tab.setAttribute('tabindex', '0');
      tab.setAttribute('data-id', t.id);
      tab.setAttribute('aria-selected', on ? 'true' : 'false');
      var label = document.createElement('span');
      label.className = 'tab-label';
      label.textContent = t.title;
      tab.appendChild(label);
      (function (id, el) {
        tab.addEventListener('click', function (ev) {
          if (ev.target && ev.target.classList.contains('tab-x')) return;
          var now = Date.now();
          if (lastTap.id === id && now - lastTap.at < 320) { beginRename(id, el); return; }
          lastTap = { id: id, at: now };
          client.selectTab(id);
        });
        tab.addEventListener('dblclick', function () { beginRename(id, el); });
        tab.addEventListener('keydown', function (ev) { if (ev.key === 'Enter') client.selectTab(id); });
      })(t.id, tab);
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
  function beginRename(id, tabEl) {
    var label = tabEl.querySelector('.tab-label');
    if (!label || tabEl.querySelector('input')) return;
    var old = label.textContent;
    var input = document.createElement('input');
    input.className = 'tab-rename';
    input.value = old;
    input.maxLength = 32;  // P1-4：PRD 上限 32 字符
    tabEl.replaceChild(input, label);
    input.focus();
    input.select();
    var done = false;
    function commit(save) {
      if (done) return;
      done = true;
      var title = save ? input.value.trim() : old;
      if (!title) title = old;
      var span = document.createElement('span');
      span.className = 'tab-label';
      span.textContent = title;
      if (input.parentNode) input.parentNode.replaceChild(span, input);
      if (save && title !== old) {
        if (client.renameTab) client.renameTab(id, title);
        else { renderTabs(state); }
      }
    }
    input.addEventListener('blur', function () { commit(true); });
    input.addEventListener('keydown', function (e) {
      if (e.key === 'Enter') { e.preventDefault(); commit(true); }
      else if (e.key === 'Escape') { e.preventDefault(); commit(false); }
    });
    input.addEventListener('click', function (e) { e.stopPropagation(); });
  }

  $('btn-new').addEventListener('click', reuseNewTab);
  btnNewEmpty.addEventListener('click', reuseNewTab);
  var exitTabId = null;
  $('btn-exit-restart').addEventListener('click', function () {
    if (exitTabId !== null && client.restartTab) client.restartTab(exitTabId);
    exitCard.hidden = true;
  });
  $('btn-exit-close').addEventListener('click', function () {
    if (exitTabId !== null) client.closeTab(exitTabId);
    exitCard.hidden = true;
  });
  function reuseNewTab() {
    Promise.resolve(client.newTab()).catch(function (err) {
      if (err && (err.code === 'limit_session' || err.http === 409)) showLimit('session');
      else showToast('新建失败，请重试');
    });
  }

  /* ---------- 菜单 ---------- */
  function openMenu() { menuScrim.hidden = false; menuEl.hidden = false; }
  function closeMenu() { menuEl.hidden = true; menuScrim.hidden = true; }
  $('btn-menu').addEventListener('click', function (e) { e.stopPropagation(); menuEl.hidden ? openMenu() : closeMenu(); });
  menuScrim.addEventListener('click', closeMenu);
  menuEl.addEventListener('click', function (e) {
    var act = e.target && e.target.getAttribute('data-act');
    if (!act) return;
    closeMenu();
    if (act === 'config') openConfig();
    else if (act === 'fullscreen') toggleFullscreen();
    else if (act === 'logout') {
      exitSelecting();
      if (client.logout) client.logout();
      else setView('login');
    }
  });

  /* ---------- 粘贴（P-23） ---------- */
  function openPaste() {
    pasteErr.hidden = true;
    pasteText.value = '';
    if (IS_LOOPBACK && navigator.clipboard && navigator.clipboard.readText) {
      pasteHint.textContent = '已从剪贴板预填，可编辑后发送。';
      navigator.clipboard.readText().then(function (t) { pasteText.value = t || ''; }).catch(function () {
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
    var ok = client.paste(text);
    if (ok === false) { pasteErr.hidden = false; return; }
    closePaste();
  });

  /* ---------- 断线按钮 / 覆盖层 ---------- */
  function reconnect() {
    if (client.reconnect) client.reconnect();
    else if (client.connect) client.connect();
  }
  $('btn-reconnect-now').addEventListener('click', reconnect);
  $('ov-reconnect').addEventListener('click', reconnect);
  $('ov-conn-retry').addEventListener('click', function () {
    connErrorOv.hidden = true;
    reconnect();
  });
  $('ov-refresh').addEventListener('click', function () { location.reload(); });

  /* ---------- 四项上限覆盖层 ---------- */
  var limitTimer = null;
  function showLimit(kind, retryAfter) {
    limitOv.hidden = false;
    limitSecondary.hidden = true;
    limitPrimary.disabled = false;
    limitPrimary.textContent = '重试';
    clearInterval(limitTimer);
    var retry = function () { limitOv.hidden = true; reconnect(); };
    if (kind === 'hs') {
      limitTitle.textContent = '连接请求过于频繁，请稍后再试';
      var left = Math.max(1, retryAfter || 60);
      limitBody.textContent = left + ' 秒后可重试';
      limitPrimary.disabled = true;
      limitTimer = setInterval(function () {
        left--;
        if (left <= 0) {
          clearInterval(limitTimer);
          limitBody.textContent = '现在可以重试';
          limitPrimary.disabled = false;
        } else limitBody.textContent = left + ' 秒后可重试';
      }, 1000);
      limitPrimary.onclick = retry;
    } else if (kind === 'sse') {
      limitTitle.textContent = '连接数已达上限';
      limitBody.textContent = '已在线端不受影响，请稍后重试。';
      limitPrimary.onclick = retry;
    } else if (kind === 'session') {
      limitTitle.textContent = '会话数已达上限';
      limitBody.textContent = '请关闭一个不用的会话后再试。';
      limitPrimary.textContent = '关闭一个会话';
      limitPrimary.onclick = function () {
        limitOv.hidden = true;
        setView('terminal');
        renderTabs(state);
      };
      limitSecondary.hidden = false;
      limitSecondary.onclick = function () {
        limitOv.hidden = true;
        reuseNewTab();
      };
    } else {
      limitOv.hidden = true;
    }
  }

  /* ---------- 全屏 F-27 ---------- */
  var fsEl = document.documentElement;
  function fsAvailable() { return !!(fsEl.requestFullscreen || fsEl.webkitRequestFullscreen); }
  function isFullscreen() { return !!(document.fullscreenElement || document.webkitFullscreenElement); }
  function syncFsBtn() {
    var on = isFullscreen();
    var btn = $('btn-fs');
    btn.setAttribute('aria-pressed', on ? 'true' : 'false');
    btn.setAttribute('aria-label', on ? '退出全屏' : '全屏');
    appEl.classList.toggle('fs', on);
  }
  function toggleFullscreen() {
    if (isFullscreen()) {
      var exit = document.exitFullscreen || document.webkitExitFullscreen;
      if (exit) exit.call(document);
      return;
    }
    if (!fsAvailable()) {
      showToast('Safari 不支持网页全屏：点“分享”›“添加到主屏幕”，从主屏幕图标打开即为全屏。', 4000);
      return;
    }
    var req = fsEl.requestFullscreen || fsEl.webkitRequestFullscreen;
    try {
      var p = req.call(fsEl);
      if (p && p.catch) p.catch(function () { showToast('全屏不可用，可尝试“添加到主屏幕”。', 3000); });
    } catch (e) { showToast('全屏不可用，可尝试“添加到主屏幕”。', 3000); }
  }
  $('btn-fs').addEventListener('click', toggleFullscreen);
  document.addEventListener('fullscreenchange', syncFsBtn);
  document.addEventListener('webkitfullscreenchange', syncFsBtn);

  /* ---------- 键带 F-28 ---------- */
  var keybarCollapsed = false;
  function shouldShowKeybar() {
    if (q.get('keys') === '1') return true;
    return (navigator.maxTouchPoints || 0) > 0 && innerWidth <= 768;
  }
  function syncKeybar() { keybar.hidden = !shouldShowKeybar(); }
  $('key-esc').addEventListener('pointerdown', function (e) { e.preventDefault(); });
  $('key-tab').addEventListener('pointerdown', function (e) { e.preventDefault(); });
  $('key-esc').addEventListener('click', function () { focusKeyboard(); client.sendInput('\x1b'); });
  $('key-tab').addEventListener('click', function () { focusKeyboard(); client.sendInput('\x09'); });
  $('keybar-toggle').addEventListener('click', function () {
    keybarCollapsed = !keybarCollapsed;
    keybar.classList.toggle('collapsed', keybarCollapsed);
    $('keybar-toggle').textContent = keybarCollapsed ? '⌃' : '⌄';
  });

  /* ---------- 配置页 ---------- */
  var cfgFont = $('cfg-font'), cfgSize = $('cfg-size'), cfgSizeVal = $('cfg-size-val');
  var cfgSizeSpin = $('cfg-size-spin'), cfgMode = $('cfg-mode'), cfgPalette = $('cfg-palette');
  var cfgShell = $('cfg-shell'), cfgDir = $('cfg-dir'), cfgDirClear = $('cfg-dir-clear');
  var cfgDirErr = $('cfg-dir-err'), cfgBanner = $('cfg-banner');
  // value 必须是服务端内部值（'' = 平台默认），label 仅展示；发展示名会让 shell 被丢弃。
  var shellOptions = /Win/i.test(navigator.platform || '')
    ? [{ l: '默认（PowerShell）', v: '' }, { l: 'cmd.exe', v: 'powershell' }]
    : [{ l: '默认（zsh）', v: '' }, { l: '/bin/bash', v: '/bin/bash' }, { l: '/bin/sh', v: '/bin/sh' }];
  var dirMsgs = {
    not_exist: '目录不存在，已保留原值',
    not_dir: '不是目录，已保留原值',
    cannot_enter: '无法进入，已保留原值',
    not_allowed: '无法进入，已保留原值',
    unknown: '目录不存在或无法进入，已保留原值'
  };

  function applySettings(s) {
    cfgFont.value = s.fontFamily || s.font || 'builtin';
    cfgSize.value = s.fontSize;
    cfgSizeVal.textContent = s.fontSize;
    var segs = cfgMode.querySelectorAll('span');
    for (var i = 0; i < segs.length; i++) segs[i].classList.toggle('on', segs[i].getAttribute('data-v') === s.colorMode);
    cfgPalette.disabled = s.colorMode !== 'independent';
    cfgPalette.value = s.palette;
    cfgShell.innerHTML = '';
    for (var j = 0; j < shellOptions.length; j++) {
      var o = document.createElement('option');
      o.textContent = shellOptions[j].l; o.value = shellOptions[j].v;
      if (shellOptions[j].v === (s.shell || '')) o.selected = true;
      cfgShell.appendChild(o);
    }
    cfgDir.value = s.startupDir || '';
    cfgDirClear.hidden = !s.startupDir;
    cfgDirErr.hidden = true;
    cfgDir.style.borderColor = '';
    if (s.colorMode === 'independent') renderer.setPalette(s.palette);
    else renderer.setPalette('dark');
    renderer.setFontSize(s.fontSize || 13);
  }
  function saveSetting(patch, spin) {
    if (spin) spin.hidden = false;
    cfgBanner.hidden = true;
    return client.writeSettings(patch).then(function () {
      if (spin) spin.hidden = true;
    }).catch(function (err) {
      if (spin) spin.hidden = true;
      if (err && err.field === 'startupDir') {
        var reason = err.reason || 'unknown';
        cfgDirErr.textContent = dirMsgs[reason] || dirMsgs.unknown;
        cfgDirErr.hidden = false;
        cfgDir.style.borderColor = '#EF4444';
        cfgDir.value = client.settings.startupDir || '';
      } else {
        cfgBanner.hidden = false;
        setTimeout(function () { cfgBanner.hidden = true; }, 3000);
      }
    });
  }
  function openConfig() {
    exitSelecting();
    setView('config');
    applySettings(client.settings);
  }
  function closeConfig() {
    setView(state.view === 'terminal' ? 'terminal' : state.view || 'terminal');
  }
  $('cfg-back').addEventListener('click', closeConfig);
  cfgFont.addEventListener('change', function () { saveSetting({ fontFamily: cfgFont.value }); });
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
    cfgDirErr.hidden = true;
    cfgDir.style.borderColor = '';
    saveSetting({ startupDir: cfgDir.value.trim() });
  });
  cfgDir.addEventListener('keydown', function (e) { if (e.key === 'Enter') cfgDir.blur(); });
  cfgDirClear.addEventListener('click', function () { cfgDir.value = ''; saveSetting({ startupDir: '' }); });

  /* ---------- Toast ---------- */
  var toastEl = $('toast');
  function showToast(msg, ms) {
    toastEl.textContent = msg;
    toastEl.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { toastEl.hidden = true; }, ms || 1500);
  }

  /* ---------- 尺寸协同 ---------- */
  function syncAppHeight() {
    var vv = window.visualViewport;
    if (vv && innerHeight - vv.height > 80) appEl.style.height = vv.height + 'px';
    else appEl.style.height = '';
  }
  var resizeTimer = null;
  function applyGrid() {
    if (currentView !== 'terminal') return;
    var fit = renderer.measureFit();
    client.resize(fit.cols, fit.rows);
  }
  function onResize() {
    syncAppHeight();
    syncKeybar();
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(function () {
      if (currentView !== 'terminal') return;
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
  if (window.ResizeObserver) new ResizeObserver(function () { onResize(); }).observe(canvasScroll);

  /* ---------- 回前台立即重连（P1-B3） ---------- */
  function wake() {
    if (document.visibilityState === 'hidden') return;
    if (client.wake) client.wake();
  }
  document.addEventListener('visibilitychange', wake);
  window.addEventListener('pageshow', function (e) { if (e.persisted) wake(); });

  /* ---------- 客户端事件 ---------- */
  client.onState(function (s) {
    state = s;
    if (s.view === 'terminal') hasConnected = true;
    applyState(s);
  });
  client.onFrame(function (frame) {
    skeleton.hidden = true;
    exitCard.hidden = true;  // 新帧 = 会话已恢复/重建，收起结束卡
    renderer.setScrollback(client.scrollback || []);
    renderer.setFrame(frame);
  });
  client.onSettings(function (s) { applySettings(s); });
  client.onError && client.onError(function (msg) { showToast(msg); });

  /* ---------- 启动 ---------- */
  syncAppHeight();
  syncKeybar();
  setView(DEMO ? (demoView || 'terminal') : 'handshake');
  if (DEMO) {
    client.connect().then(function () {
      hasConnected = true;
      applySettings(client.settings);
      setView(demoView || 'terminal');
      if (demoView === 'login') {
        loginHint.hidden = q.get('bound') !== '0';
        if (q.get('locked')) startLock(45);
        if (q.get('bad')) setLoginErr('验证码错误，请重试');
      }
      if (demoView === 'bind') {
        bindOpen.href = 'otpauth://totp/DevDesk?secret=JBSWY3DPEHPK3PXP&issuer=DevDesk';
        bindOpen.classList.remove('disabled');
        if (q.get('invalid')) {
          bindBanner.hidden = false;
          bindBanner.textContent = '尝试次数过多，绑定已失效，请在桌面端刷新二维码';
          disableBind();
        }
      }
      if (q.get('limit')) showLimit(q.get('limit'), 45);
      if (q.get('mock') === 'restart') {
        emptyTitle.textContent = '桌面端已重启，会话已丢失';
        btnNewEmpty.textContent = '重开';
        emptyEl.hidden = false;
        skeleton.hidden = true;
      }
      if (q.get('mock') === 'disconnect') setTimeout(function () { client.simulateDisconnect(); }, 200);
      if (q.get('mock') === 'closed') setTimeout(function () {
        disconnectedOv.hidden = false; reconnectBar.hidden = true; setConn('err');
      }, 200);
      if (q.get('mock') === 'revoked') setTimeout(function () { revokedOv.hidden = false; }, 200);
      if (q.get('mock') === 'menu') setTimeout(openMenu, 200);
    });
  } else {
    client.connect().catch(function () {});
  }

  window.__wt = { client: client, renderer: renderer, state: function () { return state; } };
})();
