// need#490 W7 真产品首连 E2E：起 Debug/Release 产品 → 握手 → 回环免验登录 → 建终端 →
// 网格出帧 → 输入回显 → 杀进程再起 → 自动重连并恢复出帧。
// 启动一律用 debug-only 入口（DEVDESK_WEB_AUTOSTART + DEVDESK_WEB_PORT）与独立
// DEVDESK_DATA_ROOT，不驱动桌面 UI、不碰真实数据根。
// 用法：node web_terminal_e2e.mjs [--bin <路径>]；CI 上无 GUI 会话时设
// DEVDESK_E2E_ALLOW_SKIP=1 → 产品起不来则打印 E2E_SKIP_NO_DISPLAY 并退出 0（非静默）。
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, '..', '..');
const ART = path.join(ROOT, 'evidence', 'need490-w7');
fs.mkdirSync(ART, { recursive: true });

const argv = process.argv.slice(2);
const binArg = argv.indexOf('--bin');
const ALLOW_SKIP = process.env.DEVDESK_E2E_ALLOW_SKIP === '1';

function resolveBin() {
  if (binArg >= 0 && argv[binArg + 1]) return path.resolve(argv[binArg + 1]);
  if (process.env.DEVDESK_E2E_BIN) return path.resolve(process.env.DEVDESK_E2E_BIN);
  const isWin = process.platform === 'win32';
  const appMac = path.join('DevDesk.app', 'Contents', 'MacOS', 'DevDesk');
  const cands = [];
  for (const dir of ['build', 'build-release']) {
    if (process.platform === 'darwin') cands.push(path.join(ROOT, dir, appMac));
    cands.push(path.join(ROOT, dir, isWin ? 'DevDesk.exe' : 'DevDesk'));
  }
  for (const c of cands) if (fs.existsSync(c)) return c;
  return null;
}

function freePort() {
  return new Promise((resolve, reject) => {
    const srv = net.createServer();
    srv.once('error', reject);
    srv.listen(0, '127.0.0.1', () => {
      const p = srv.address().port;
      srv.close(() => resolve(p));
    });
  });
}

async function waitHttp(url, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const r = await fetch(url);
      if (r.ok) return true;
    } catch { /* 未起来 */ }
    await new Promise((r) => setTimeout(r, 250));
  }
  return false;
}

const state = { bin: null, port: 0, dataRoot: '', steps: [], screenshots: [], edge: 'unknown' };

function startApp() {
  const child = spawn(state.bin, [], {
    env: {
      ...process.env,
      DEVDESK_DATA_ROOT: state.dataRoot,
      DEVDESK_WEB_AUTOSTART: '1',
      DEVDESK_WEB_PORT: String(state.port),
    },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  child.log = '';
  const keep = (buf) => {
    child.log = (child.log + buf.toString()).slice(-6000);
    state.lastLog = child.log;
  };
  child.stdout.on('data', keep);
  child.stderr.on('data', keep);
  return child;
}

function stopApp(child) {
  return new Promise((resolve) => {
    if (!child || child.exitCode !== null) return resolve();
    const timer = setTimeout(() => { try { child.kill('SIGKILL'); } catch {} }, 5000);
    child.once('exit', () => { clearTimeout(timer); resolve(); });
    try { child.kill('SIGTERM'); } catch { resolve(); }
  });
}

// 截图走 CDP Page.captureScreenshot：Playwright 的 page.screenshot 在持续重绘的
// canvas 页面上会卡在合成器等待（W7 实测 30s 超时）；CDP 直接取当前帧，不受
// 「等待稳定」影响。拿不到 CDP 时退回 page.screenshot。
async function shot(page, name) {
  const file = path.join(ART, name);
  try {
    if (!page.__cdp) page.__cdp = await page.context().newCDPSession(page);
    const { data } = await page.__cdp.send('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(file, Buffer.from(data, 'base64'));
  } catch {
    await page.screenshot({ path: file, timeout: 15000 });
  }
  state.screenshots.push(path.relative(ROOT, file));
}

async function main() {
  state.bin = resolveBin();
  if (!state.bin) throw new Error('找不到产品二进制（用 DEVDESK_E2E_BIN 或 --bin 指定）');
  state.port = await freePort();
  state.dataRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'devdesk-e2e-'));
  const base = `http://127.0.0.1:${state.port}`;

  let app = startApp();
  try {
    if (!(await waitHttp(`${base}/api/status`, 30000))) {
      const log = app.log || '';
      if (ALLOW_SKIP) {
        console.log(`E2E_SKIP_NO_DISPLAY: 产品 30s 内未监听 ${state.port}`);
        console.log(log.split('\n').slice(-8).join('\n'));
        return 0;
      }
      throw new Error(`产品 30s 内未监听 ${state.port}\n${log.split('\n').slice(-15).join('\n')}`);
    }
    state.steps.push('server-up');

    // 静态资源可达性（嵌入验证，不依赖浏览器）
    const index = await fetch(`${base}/`);
    const html = await index.text();
    if (!index.ok || !html.includes('网页终端')) throw new Error(`GET / 异常: ${index.status}`);
    const js = await fetch(`${base}/assets/app.js`);
    if (!js.ok || !(js.headers.get('content-type') || '').includes('javascript')) {
      throw new Error(`GET /assets/app.js 异常: ${js.status} ${js.headers.get('content-type')}`);
    }
    const etag = js.headers.get('etag');
    if (etag) {
      const cached = await fetch(`${base}/assets/app.js`, { headers: { 'If-None-Match': etag } });
      if (cached.status !== 304) throw new Error(`ETag 未命中 304: ${cached.status}`);
    }
    state.steps.push('assets-embedded');

    // Edge 可用性探测：缺失时显式标记降级，不静默
    let edge = null;
    try { edge = await chromium.launch({ channel: 'msedge' }); state.edge = 'present'; }
    catch { state.edge = 'EDGE_MISSING'; console.log('EDGE_MISSING: 无 msedge，降级 Chromium'); }
    finally { if (edge) await edge.close().catch(() => {}); }

    const browser = await chromium.launch();
    const context = await browser.newContext({
      viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true,
      userAgent: 'Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Mobile/15E148 Safari/604.1',
    });
    const page = await context.newPage();
    page.on('pageerror', (e) => console.log('PAGE_ERROR', e.message));
    await page.goto(`${base}/`, { waitUntil: 'load', timeout: 30000 });
    await page.waitForFunction(
      () => window.__wt && window.__wt.state().status === 'connected' && window.__wt.state().view === 'terminal',
      null, { timeout: 30000 });
    state.steps.push('handshake+loopback-login');
    await page.evaluate(() => {
      window.__events = [];
      window.__wt.client.onState((s) => window.__events.push({ status: s.status, view: s.view, resynced: !!s.resynced, at: Date.now() }));
    });
    await shot(page, '01-terminal-empty.png');

    // 开机不建会话（bug247）：先新建终端再等网格帧
    await page.evaluate(() => window.__wt.client.newTab());
    await page.waitForFunction(() => !!(window.__wt.client && window.__wt.client._lastFrame), null, { timeout: 30000 });
    state.steps.push('grid-frame');
    console.log('STEP grid-frame');
    // 网格出帧后先确认页面主线程仍可响应（渲染死循环会让截图卡死）
    const probe = await page.evaluate(() => ({
      lines: (window.__wt.client._lastFrame || {}).lines ? window.__wt.client._lastFrame.lines.length : -1,
      rows: (window.__wt.client._lastFrame || {}).rows,
      cols: (window.__wt.client._lastFrame || {}).cols,
    }), { timeout: 10000 }).catch((e) => ({ evaluateError: String(e && e.message ? e.message : e) }));
    console.log('GRID_PROBE', JSON.stringify(probe));
    // 回归钉子（bug#497）：帧头端序错位时 rows/cols 变 256 倍、lines 与 rows 不等。
    // C++ web_grid 写小端；protocol.js 任一数值漏传 littleEndian=true 即在此红。
    if (!(probe.rows >= 8 && probe.rows <= 200) ||
        !(probe.cols >= 20 && probe.cols <= 200) ||
        probe.lines !== probe.rows) {
      throw new Error('GRID_FRAME_INVALID ' + JSON.stringify(probe));
    }
    await shot(page, '02-grid.png');
    if (process.env.DEVDESK_E2E_DEBUG === '1') {
      const diag = await page.evaluate(() => JSON.stringify({
        tabs: window.__wt.client.tabs,
        frame: window.__wt.client._lastFrame
          ? { rows: window.__wt.client._lastFrame.rows, cols: window.__wt.client._lastFrame.cols,
              text: JSON.stringify(window.__wt.client._lastFrame).slice(0, 600) }
          : null,
      }));
      console.log('DIAG', diag);
    }

    // 输入回显
    await page.evaluate(() => {
      // 帧里每格是独立对象（{ch:'h'},{ch:'i'}），JSON.stringify 不会出现连续 "hi"；
      // 回显断言必须按行拼回文本再查。
      window.__frameText = () => {
        const f = window.__wt.client._lastFrame;
        if (!f || !f.lines) return '';
        return f.lines.map((r) => (r || []).map((c) => (c && c.ph ? '' : (c && c.ch) || ' ')).join('')).join('\n');
      };
    });
    await page.evaluate(() => window.__wt.client.sendInput('echo hi\r'));
    await page.waitForFunction(
      () => window.__frameText().includes('hi'),
      null, { timeout: 30000 });
    state.steps.push('echo-hi');
    console.log('STEP echo-hi');
    await shot(page, '03-echo-hi.png');

    // ===== M3：网页端轻量项（键带 / 链接 / 拖文件 / 无障碍 / 标签排序） =====
    // M3-1 最小键带：手机视口 hasTouch → 键带自动可见（不额外加 ?keys=1）
    const keybarVisible = await page.evaluate(() => !document.getElementById('keybar').hidden);
    if (!keybarVisible) throw new Error('M3-1 KEYBAR_HIDDEN');
    await page.evaluate(() => { window.__wt.__lastInput = null; });
    await page.evaluate(() => document.getElementById('key-ctrl').click());
    const ctrlState = await page.evaluate(() => ({
      latched: document.getElementById('key-ctrl').classList.contains('latched'),
      pressed: document.getElementById('key-ctrl').getAttribute('aria-pressed'),
    }));
    if (!ctrlState.latched || ctrlState.pressed !== 'true') {
      throw new Error('M3-1 CTRL_LATCH_MISSING ' + JSON.stringify(ctrlState));
    }
    await page.evaluate(() => document.getElementById('kbd-capture')
      .dispatchEvent(new KeyboardEvent('keydown', { key: 'c', bubbles: true, cancelable: true })));
    const ctrlInput = await page.evaluate(() => window.__wt.__lastInput);
    if (ctrlInput !== '\x03') throw new Error('M3-1 CTRL_C_BYTE ' + JSON.stringify(ctrlInput));
    // 锁存保持到再点一次 Ctrl（点字母不自动解除）
    if (!(await page.evaluate(() => document.getElementById('key-ctrl').classList.contains('latched')))) {
      throw new Error('M3-1 CTRL_LATCH_AUTO_CLEARED');
    }
    await page.evaluate(() => document.getElementById('key-ctrl').click());
    if (await page.evaluate(() => document.getElementById('key-ctrl').classList.contains('latched'))) {
      throw new Error('M3-1 CTRL_UNLATCH_FAILED');
    }
    await page.evaluate(() => document.getElementById('key-up').click());
    if (await page.evaluate(() => window.__wt.__lastInput) !== '\x1b[A') throw new Error('M3-1 UP_KEY');
    await page.evaluate(() => document.getElementById('key-down').click());
    if (await page.evaluate(() => window.__wt.__lastInput) !== '\x1b[B') throw new Error('M3-1 DOWN_KEY');
    state.steps.push('m3-keybar');
    console.log('STEP m3-keybar');

    // M3-2 链接：按三端矩阵手机网页「不做」——触屏视口点击不打开、也无悬停下划线
    await page.evaluate(() => {
      window.__lastOpen = null;
      window.__wt.__lastOpen = null;
      window.open = (u) => { window.__lastOpen = u; return null; };  // 拦掉真实弹窗
    });
    await page.evaluate(() => window.__wt.client.sendInput("echo 'https://a.io/x.'\r"));
    await page.waitForFunction(() => window.__frameText && window.__frameText().includes('https://a.io/x'),
      null, { timeout: 30000 });
    const linkRect = await page.evaluate(() => window.__wt.renderer.linkRectFor('https://a.io/x'));
    if (!linkRect) throw new Error('M3-2 LINK_NOT_FOUND');
    await page.mouse.click(linkRect.x, linkRect.y);
    if (await page.evaluate(() => window.__wt.__lastOpen)) throw new Error('M3-2 MOBILE_LINK_OPENED');
    await page.mouse.move(linkRect.x, linkRect.y);
    if (!(await page.evaluate(() => document.getElementById('link-underline').hidden))) {
      throw new Error('M3-2 MOBILE_HOVER_UNDERLINE');
    }
    state.steps.push('m3-link-mobile');
    console.log('STEP m3-link-mobile');

    // M3-3 拖文件：单引号转义、空格连接、走 bracketed paste 通道
    await page.evaluate(() => { window.__wt.__lastInput = null; window.__wt.__lastPaste = false; });
    await page.evaluate(() => {
      const dt = new DataTransfer();
      const add = (name, p) => {
        const f = new File(['x'], name, { type: 'text/plain' });
        try { Object.defineProperty(f, 'path', { value: p }); } catch (e) { /* 浏览器可能不可写 */ }
        dt.items.add(f);
      };
      add('a b.txt', '/tmp/a b.txt');
      add("it's.txt", "/tmp/it's.txt");
      document.getElementById('canvas-scroll')
        .dispatchEvent(new DragEvent('drop', { bubbles: true, cancelable: true, dataTransfer: dt }));
    });
    const dropped = await page.evaluate(() => ({ text: window.__wt.__lastInput, paste: window.__wt.__lastPaste }));
    const dropExpect = "'/tmp/a b.txt' '/tmp/it'\\''s.txt'";
    if (dropped.text !== dropExpect) throw new Error('M3-3 DROP_TEXT ' + JSON.stringify(dropped.text));
    if (!dropped.paste) throw new Error('M3-3 DROP_NOT_PASTE_CHANNEL');
    // 非终端区拖入不插入
    await page.evaluate(() => {
      const before = window.__wt.__lastInput;
      const dt = new DataTransfer();
      dt.items.add(new File(['x'], 'z.txt', { type: 'text/plain' }));
      document.getElementById('view-terminal')
        .dispatchEvent(new DragEvent('drop', { bubbles: true, cancelable: true, dataTransfer: dt }));
      window.__dropUnchanged = window.__wt.__lastInput === before;
    });
    if (!(await page.evaluate(() => window.__dropUnchanged))) throw new Error('M3-3 DROP_OUTSIDE_INSERTED');
    state.steps.push('m3-drop');
    console.log('STEP m3-drop');

    // M3-4 无障碍语义：文本层 role/aria、连接状态 aria-live、键带与粘贴钮可聚焦有标签
    const a11y = await page.evaluate(() => {
      const ids = ['key-esc', 'key-tab', 'key-ctrl', 'key-up', 'key-down'];
      const tl = document.getElementById('text-layer');
      return {
        role: tl.getAttribute('role'),
        live: tl.getAttribute('aria-live'),
        label: tl.getAttribute('aria-label'),
        hidden: tl.getAttribute('aria-hidden'),
        connLive: document.getElementById('conn-text').getAttribute('aria-live'),
        pasteLabel: document.getElementById('btn-paste').getAttribute('aria-label'),
        keyLabels: ids.map((id) => document.getElementById(id).getAttribute('aria-label')),
        focusable: ids.every((id) => {
          const b = document.getElementById(id);
          b.focus();
          return document.activeElement === b && b.tabIndex >= 0;
        }),
      };
    });
    if (a11y.role !== 'log' || a11y.live !== 'polite' || a11y.label !== '终端输出' || a11y.hidden !== null) {
      throw new Error('M3-4 TEXT_LAYER_ARIA ' + JSON.stringify(a11y));
    }
    if (a11y.connLive !== 'polite') throw new Error('M3-4 CONN_LIVE');
    if (!a11y.pasteLabel) throw new Error('M3-4 PASTE_LABEL');
    if (a11y.keyLabels.some((l) => !l) || !a11y.focusable) throw new Error('M3-4 KEYBAR_A11Y ' + JSON.stringify(a11y));
    state.steps.push('m3-a11y');
    console.log('STEP m3-a11y');

    // M3-5 标签拖拽排序 + 关闭其他（桌面视口；触屏不启用）
    const mobileDrag = await page.evaluate(() => {
      const el = document.querySelector('#tabs .tab');
      const draggable = el ? el.getAttribute('draggable') : null;
      let menuOpen = false;
      if (el) {
        el.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: 60, clientY: 60 }));
        menuOpen = !document.getElementById('tab-menu').hidden;
      }
      return { draggable, menuOpen };
    });
    if (mobileDrag.draggable === 'true') throw new Error('M3-5 MOBILE_DRAG_ENABLED');
    if (mobileDrag.menuOpen) throw new Error('M3-5 MOBILE_CONTEXT_MENU_OPEN');

    const desktop = await browser.newContext({ viewport: { width: 1280, height: 800 } });
    const dpage = await desktop.newPage();
    dpage.on('pageerror', (e) => console.log('PAGE_ERROR', e.message));
    await dpage.goto(`${base}/`, { waitUntil: 'load', timeout: 30000 });
    await dpage.waitForFunction(
      () => window.__wt && window.__wt.state().status === 'connected' && window.__wt.state().view === 'terminal',
      null, { timeout: 30000 });

    // 晚连接的网页客户端不会收到初始 tab 快照（服务端只在 tab 变更时推 session 事件）：
    // 先建一个 tab 让 session 事件带回 tabs，否则 #empty 覆盖层会挡住画布点击。
    await dpage.evaluate(() => window.__wt.client.newTab());
    await dpage.waitForFunction(
      () => window.__wt.state().tabs.length >= 1 && document.getElementById('empty').hidden,
      null, { timeout: 15000 });

    // M3-2（桌面）：点击打开（剥尾标点）、拖动与长按不打开
    await dpage.evaluate(() => {
      window.__lastOpen = null;
      window.__wt.__lastOpen = null;
      window.open = (u) => { window.__lastOpen = u; return null; };
    });
    await dpage.evaluate(() => window.__wt.client.sendInput("echo 'https://a.io/x.'\r"));
    await dpage.waitForFunction(() => !!window.__wt.renderer.linkRectFor('https://a.io/x'),
      null, { timeout: 30000 });
    const dLink = await dpage.evaluate(() => window.__wt.renderer.linkRectFor('https://a.io/x'));
    if (!dLink) throw new Error('M3-2 DESKTOP_LINK_NOT_FOUND');
    await dpage.mouse.click(dLink.x, dLink.y);
    const dOpened = await dpage.evaluate(() => window.__wt.__lastOpen);
    if (dOpened !== 'https://a.io/x') throw new Error('M3-2 DESKTOP_LINK_OPEN ' + JSON.stringify(dOpened));
    // 拖动（拖选）不得打开
    await dpage.evaluate(() => { window.__wt.__lastOpen = null; });
    await dpage.mouse.move(dLink.x, dLink.y);
    await dpage.mouse.down();
    await dpage.mouse.move(dLink.x + 40, dLink.y + 4);
    await dpage.mouse.up();
    if (await dpage.evaluate(() => window.__wt.__lastOpen)) throw new Error('M3-2 DESKTOP_DRAG_OPENED');
    // 长按（≥500ms）不得打开
    await dpage.evaluate((pt) => new Promise((resolve) => {
      window.__wt.__lastOpen = null;
      const el = document.getElementById('canvas-scroll');
      el.dispatchEvent(new PointerEvent('pointerdown', {
        bubbles: true, clientX: pt.x, clientY: pt.y, pointerType: 'touch',
      }));
      setTimeout(() => {
        el.dispatchEvent(new PointerEvent('pointerup', { bubbles: true, clientX: pt.x, clientY: pt.y, pointerType: 'touch' }));
        el.dispatchEvent(new MouseEvent('click', { bubbles: true, clientX: pt.x, clientY: pt.y }));
        resolve();
      }, 600);
    }), dLink);
    if (await dpage.evaluate(() => window.__wt.__lastOpen)) throw new Error('M3-2 DESKTOP_LONG_PRESS_OPENED');
    state.steps.push('m3-link-desktop');
    console.log('STEP m3-link-desktop');

    await dpage.waitForFunction(() => document.querySelectorAll('#tabs .tab').length >= 2, null, { timeout: 15000 });
    const tabBefore = await dpage.evaluate(() => ({
      ids: Array.from(document.querySelectorAll('#tabs .tab')).map((el) => el.getAttribute('data-id')),
      active: window.__wt.state().activeId,
      draggable: Array.from(document.querySelectorAll('#tabs .tab')).every((el) => el.getAttribute('draggable') === 'true'),
    }));
    if (!tabBefore.draggable) throw new Error('M3-5 DESKTOP_DRAG_HANDLE_MISSING');
    await dpage.evaluate(() => {
      const tabs = Array.from(document.querySelectorAll('#tabs .tab'));
      const from = tabs[0], to = tabs[tabs.length - 1];
      const dt = new DataTransfer();
      const fire = (el, type) => el.dispatchEvent(new DragEvent(type, { bubbles: true, cancelable: true, dataTransfer: dt }));
      fire(from, 'dragstart');
      fire(to, 'dragover');
      fire(to, 'drop');
      fire(from, 'dragend');
    });
    const tabAfter = await dpage.evaluate(() => ({
      ids: Array.from(document.querySelectorAll('#tabs .tab')).map((el) => el.getAttribute('data-id')),
      active: window.__wt.state().activeId,
    }));
    if (tabAfter.ids[0] !== tabBefore.ids[1] ||
        tabAfter.ids[tabAfter.ids.length - 1] !== tabBefore.ids[0]) {
      throw new Error('M3-5 DRAG_ORDER ' + JSON.stringify({ tabBefore, tabAfter }));
    }
    if (tabAfter.active !== tabBefore.active) throw new Error('M3-5 DRAG_ACTIVE_CHANGED');
    const ctxTarget = await dpage.evaluate(() => {
      const el = document.querySelectorAll('#tabs .tab')[1];
      el.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: 120, clientY: 40 }));
      return { id: el.getAttribute('data-id'), open: !document.getElementById('tab-menu').hidden };
    });
    if (!ctxTarget.open) throw new Error('M3-5 CONTEXT_MENU_NOT_OPEN');
    await dpage.evaluate(() => document.getElementById('tab-close-others').click());
    await dpage.waitForFunction(() => document.querySelectorAll('#tabs .tab').length === 1, null, { timeout: 15000 });
    const keptId = await dpage.evaluate(() => document.querySelector('#tabs .tab').getAttribute('data-id'));
    if (keptId !== ctxTarget.id) throw new Error('M3-5 CLOSE_OTHERS_KEPT ' + keptId + ' != ' + ctxTarget.id);
    state.steps.push('m3-tabs');
    console.log('STEP m3-tabs');
    await desktop.close();


    // 杀产品再起：SSE 断开 → 自动重连 → 建新终端仍有帧（resync/订阅链路活着）
    await stopApp(app);
    await page.waitForFunction(() => window.__wt.state().status !== 'connected', null, { timeout: 30000 });
    await new Promise((r) => setTimeout(r, 1500));
    app = startApp();
    if (!(await waitHttp(`${base}/api/status`, 30000))) throw new Error('重启后服务未恢复');
    await page.waitForFunction(
      () => window.__wt.state().status === 'connected',
      null, { timeout: 60000 });
    state.steps.push('auto-reconnect');
    console.log('STEP auto-reconnect');
    await page.evaluate(() => { window.__wt.client._lastFrame = null; });
    await page.evaluate(() => window.__wt.client.newTab()).catch(() => {});
    await page.waitForFunction(
      () => !!(window.__wt.client && window.__wt.client._lastFrame),
      null, { timeout: 30000 });
    state.steps.push('resync-frame');
    await shot(page, '04-reconnected.png');

    const events = await page.evaluate(() => window.__events || []);
    state.reconnectSeen = events.some((e) => e.status === 'reconnecting' || e.status === 'closed');
    state.resyncedSeen = events.some((e) => e.resynced);
    await browser.close();
    return 0;
  } finally {
    await stopApp(app);
  }
}

let code = 0;
try {
  code = await main();
  if (code === 0 && state.steps.length) {
    console.log('E2E_OK ' + state.steps.join(' -> '));
  }
} catch (err) {
  code = 1;
  console.log('E2E_FAIL ' + (err && err.message ? err.message : String(err)));
  if (state.lastLog) console.log('APP_LOG_TAIL\n' + state.lastLog.split('\n').slice(-12).join('\n'));
}
console.log(JSON.stringify({
  bin: state.bin, port: state.port, edge: state.edge, steps: state.steps,
  screenshots: state.screenshots, reconnectSeen: !!state.reconnectSeen, resyncedSeen: !!state.resyncedSeen,
}, null, 2));
process.exit(code);
