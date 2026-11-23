// need#490: three-engine smoke + browser capability matrix.
// Engines: Chromium (iPhone mobile emulation), installed Microsoft Edge (channel msedge), WebKit.
// Origins probed per engine: data:, file:, http://127.0.0.1:<port>, http://<LAN IP>:<port>.
// Outputs: artifacts/<engine>-<origin>.png + artifacts/results.json (stdout = one-line-per-engine).
import http from 'node:http';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium, webkit } from 'playwright';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ART = path.join(__dirname, 'artifacts');
fs.mkdirSync(ART, { recursive: true });

const probeSource = fs.readFileSync(path.join(__dirname, 'probe.js'), 'utf8');
const DATA_HTML = `<html><body><pre id="out"></pre><script>${probeSource}</script></body></html>`;
const DATA_URL = 'data:text/html;charset=utf-8,' + encodeURIComponent(DATA_HTML);

const server = http.createServer((req, res) => {
  const { pathname } = new URL(req.url, 'http://localhost');
  if (pathname === '/' || pathname === '/capability.html') {
    res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
    return res.end(fs.readFileSync(path.join(__dirname, 'capability.html')));
  }
  if (pathname === '/probe.js') {
    res.writeHead(200, { 'content-type': 'text/javascript; charset=utf-8' });
    return res.end(probeSource);
  }
  res.writeHead(404);
  res.end('not found');
});
await new Promise((resolve) => server.listen(0, '0.0.0.0', resolve));
const port = server.address().port;
const localhostUrl = `http://127.0.0.1:${port}`;

function lanIp() {
  for (const list of Object.values(os.networkInterfaces())) {
    for (const iface of list ?? []) {
      if (iface.family === 'IPv4' && !iface.internal) return iface.address;
    }
  }
  return null;
}
const lan = lanIp();

const IPHONE_UA = 'Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Mobile/15E148 Safari/604.1';
const ENGINES = [
  {
    name: 'chromium-mobile',
    browser: chromium,
    grantsClipboard: true,
    options: { viewport: { width: 375, height: 667 }, deviceScaleFactor: 3, isMobile: true, hasTouch: true, userAgent: IPHONE_UA },
  },
  { name: 'edge', browser: chromium, launch: { channel: 'msedge' }, grantsClipboard: true, options: { viewport: { width: 900, height: 600 } } },
  { name: 'webkit', browser: webkit, options: { viewport: { width: 375, height: 667 }, deviceScaleFactor: 3, isMobile: true, hasTouch: true, userAgent: IPHONE_UA } },
];

async function collect(page) {
  try {
    await page.waitForFunction(() => window.__CAP__ !== undefined, null, { timeout: 20000 });
    return await page.evaluate(() => window.__CAP__);
  } catch (e) {
    return { probeError: String(e && e.message ? e.message : e) };
  }
}

async function shot(page, file) {
  const target = path.join(ART, file);
  await page.screenshot({ path: target });
  return path.relative(__dirname, target);
}

const results = {
  playwright: '1.65.1',
  node: process.version,
  os: `${process.platform} ${os.release()} ${os.arch()}`,
  lanIp: lan,
  httpPort: port,
  generatedAt: new Date().toISOString(),
  engines: {},
};

for (const engine of ENGINES) {
  const entry = { launchOptions: engine.launch ?? {}, contextOptions: engine.options, clipboardPermission: 'not-requested', origins: {}, screenshots: [] };
  let browser;
  try {
    browser = await engine.browser.launch(engine.launch ?? {});
    const context = await browser.newContext(engine.options);
    try {
      if (!engine.grantsClipboard) throw new Error('engine does not support clipboard permission grants');
      await context.grantPermissions(['clipboard-read', 'clipboard-write'], { origin: localhostUrl });
      entry.clipboardPermission = 'granted for ' + localhostUrl;
    } catch (e) {
      entry.clipboardPermission = 'not available: ' + (e && e.name ? e.name : e);
    }
    const page = await context.newPage();

    await page.goto(DATA_URL, { timeout: 20000 });
    entry.screenshots.push(await shot(page, `${engine.name}-data.png`));
    entry.origins.data = await collect(page);

    await page.goto('file://' + path.join(__dirname, 'capability.html'), { timeout: 20000 });
    entry.screenshots.push(await shot(page, `${engine.name}-file.png`));
    entry.origins.file = await collect(page);

    await page.goto(localhostUrl + '/capability.html', { timeout: 20000 });
    entry.screenshots.push(await shot(page, `${engine.name}-localhost.png`));
    entry.origins.localhost = await collect(page);

    if (lan) {
      try {
        await page.goto(`http://${lan}:${port}/capability.html`, { timeout: 20000 });
        entry.screenshots.push(await shot(page, `${engine.name}-lan.png`));
        entry.origins.lan = await collect(page);
      } catch (e) {
        entry.origins.lan = { probeError: 'navigation failed: ' + (e && e.message ? e.message : e) };
      }
    }
  } catch (e) {
    entry.fatal = (e && e.name ? e.name : 'Error') + ': ' + (e && e.message ? e.message : String(e));
  } finally {
    if (browser) await browser.close().catch(() => {});
  }
  results.engines[engine.name] = entry;
}

server.close();
fs.writeFileSync(path.join(ART, 'results.json'), JSON.stringify(results, null, 2));

function verdict(cap) {
  if (!cap) return 'n/a';
  if (cap.probeError) return 'probe-error';
  if (cap.ok) return 'ok';
  return cap.error;
}
function clipVerdict(c) {
  if (!c) return 'n/a';
  if (c.probeError) return 'probe-error';
  if (!c.api) return 'api-absent';
  return 'write=' + (c.write ?? 'n/a') + ' read=' + (c.read ?? 'n/a');
}

for (const [name, e] of Object.entries(results.engines)) {
  if (e.fatal) { console.log(`${name}: FATAL ${e.fatal}`); continue; }
  const lh = e.origins.localhost ?? {};
  console.log([
    `${name}:`,
    `secure=${lh.secureContext}`,
    `dpr=${lh.devicePixelRatio}`,
    `x25519=${verdict(lh.x25519)}`,
    `hkdf=${verdict(lh.hkdf)}`,
    `aesgcm=${verdict(lh.aesgcm)}`,
    `visualViewport=${lh.visualViewport ? lh.visualViewport.present : 'n/a'}`,
    `clipboard(localhost)=${clipVerdict(lh.clipboard)}`,
  ].join(' '));
  for (const originName of ['data', 'file', 'lan']) {
    const o = e.origins[originName];
    if (!o) continue;
    console.log(`  ${originName}: secure=${o.secureContext} subtle=${o.subtlePresent} clipboard=${clipVerdict(o.clipboard)}`);
  }
}
console.log('wrote', path.relative(process.cwd(), path.join(ART, 'results.json')));
