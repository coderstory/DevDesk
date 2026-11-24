// need#490 E2E：鉴权双路径（回环免验 + 非回环 TOTP）+ SSE 并发上限 + 业务错误口径。
//
// 覆盖：局域网地址（非回环）claim → 自算 6 位 TOTP confirm → 票据 → SSE 首帧
//      → 密文输入回显 → 网页 revoke 使其它连接票据失效；8 条 SSE 订阅成功、第 9 条 503
//      且其它 API 仍可响应；票据满/登录锁定为 HTTP 200 + 密文内 409/429。
//
// 启动走 Debug 入口（DEVDESK_WEB_AUTOSTART/DEVDESK_WEB_PORT/DEVDESK_WEB_TEST_HOOKS）与
// 独立 DEVDESK_DATA_ROOT，不驱动桌面 UI、不碰真实数据根。
//
// 已知边界（不静默）：产品只在桌面二维码 URL 内携带绑定令牌，不通过 HTTP 暴露；本脚本用
// 调试测试缝（x-devdesk-test-addr 模拟回环）取得 claim 的 secret，TOTP 校验、票据、SSE、
// 密文输入全部走真实局域网地址。网页发起的 bind.revoke 只清票据、不向其它 SSE 广播
// revoke，故「其它连接失效」断言按 API 层（旧票据被拒）验证，SSE 未断记为未验证。
//
// 用法：node auth_e2e.mjs [--bin <路径>]；证据落 evidence/need490-e2e/auth_e2e.log。
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, '..', '..');
const WWW = path.join(ROOT, 'src', 'module_terminal', 'web', 'www');
const ART = path.join(ROOT, 'evidence', 'need490-e2e');
fs.mkdirSync(ART, { recursive: true });

const argv = process.argv.slice(2);
const binArg = argv.indexOf('--bin');

const logLines = [];
function log(...parts) {
  const line = parts.join(' ');
  console.log(line);
  logLines.push(line);
}
function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}
function expectErr(p) {
  return p.then(() => null, (e) => e);
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function withTimeout(p, ms, label) {
  return Promise.race([
    p,
    new Promise((_, rej) => setTimeout(() => rej(new Error(`超时 ${ms}ms: ${label}`)), ms)),
  ]);
}

// 复用前端密码学：crypto.js（RFC 向量同源）+ protocol.js 的 RealClient。
new Function('globalThis', fs.readFileSync(path.join(WWW, 'crypto.js'), 'utf8'))(globalThis);
new Function('globalThis', fs.readFileSync(path.join(WWW, 'protocol.js'), 'utf8'))(globalThis);
const C = globalThis.WTCrypto;
const WT = globalThis.WT;
assert(C && WT, 'crypto.js / protocol.js 未加载');

const realFetch = globalThis.fetch;

// ---- TOTP（RFC 6238：SHA1 / 6 位 / 30s），复用 crypto.js 的 hmacSha1 + u64be ----
const B32 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567';
function b32decode(text) {
  let bits = 0;
  let val = 0;
  const out = [];
  for (const ch of text.replace(/=+$/, '').toUpperCase()) {
    const i = B32.indexOf(ch);
    if (i < 0) continue;
    val = (val << 5) | i;
    bits += 5;
    if (bits >= 8) {
      out.push((val >>> (bits - 8)) & 255);
      bits -= 8;
    }
  }
  return new Uint8Array(out);
}
function totp(secret, atSec = Math.floor(Date.now() / 1000)) {
  const mac = C.hmacSha1(b32decode(secret), C.u64be(Math.floor(atSec / 30)));
  const off = mac[19] & 0x0f;
  const n = ((mac[off] & 0x7f) << 24) | (mac[off + 1] << 16) | (mac[off + 2] << 8) | mac[off + 3];
  return String(n % 1000000).padStart(6, '0');
}

// ---- 环境 ----
function resolveBin() {
  if (binArg >= 0 && argv[binArg + 1]) return path.resolve(argv[binArg + 1]);
  if (process.env.DEVDESK_E2E_BIN) return path.resolve(process.env.DEVDESK_E2E_BIN);
  const isWin = process.platform === 'win32';
  const mac = path.join('DevDesk.app', 'Contents', 'MacOS', 'DevDesk');
  for (const dir of ['build', 'build-release']) {
    const cands = process.platform === 'darwin'
      ? [path.join(ROOT, dir, mac), path.join(ROOT, dir, 'DevDesk')]
      : [path.join(ROOT, dir, isWin ? 'DevDesk.exe' : 'DevDesk')];
    for (const c of cands) if (fs.existsSync(c)) return c;
  }
  return null;
}
function lanIPv4() {
  for (const list of Object.values(os.networkInterfaces())) {
    for (const n of list || []) {
      if (n.family === 'IPv4' && !n.internal) return n.address;
    }
  }
  return null;
}
function freePort() {
  return new Promise((res, rej) => {
    const srv = net.createServer();
    srv.once('error', rej);
    srv.listen(0, '0.0.0.0', () => {
      const p = srv.address().port;
      srv.close(() => res(p));
    });
  });
}
async function waitHttp(url, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const r = await realFetch(url);
      if (r.ok) return true;
    } catch { /* 未起来 */ }
    await sleep(300);
  }
  return false;
}

// ---- 测试缝注入：claim 需要绑定令牌，产品不通过 HTTP 暴露，故模拟回环自测 ----
const hook = { addr: null, sealedReq: null, sealedInput: null, sealedResp: null };
globalThis.fetch = async (url, opts = {}) => {
  const u = String(url);
  let o = opts;
  if (hook.addr) {
    o = { ...opts, headers: { ...(opts.headers || {}), 'x-devdesk-test-addr': hook.addr } };
  }
  if (typeof o.body === 'string' && o.body.length > 40 && /^[A-Za-z0-9_-]+$/.test(o.body)) {
    const pathOnly = u.replace(/^https?:\/\/[^/]+/, '');
    const sealed = { firstByte: C.b64urlDecode(o.body)[0], len: C.b64urlDecode(o.body).length, path: pathOnly };
    if (!hook.sealedReq) hook.sealedReq = sealed;
    // 独立记录输入请求的密文证据：claim/login 也是信封，不能拿它们冒充「密文输入」。
    if (pathOnly === '/api/input' && !hook.sealedInput) hook.sealedInput = sealed;
  }
  const res = await realFetch(url, o);
  const env = res.headers.get('x-devdesk-envelope');
  if (env && !hook.sealedResp) hook.sealedResp = env;
  return res;
};

const state = { bin: null, port: 0, host: '', dataRoot: '', appLog: '', steps: [] };
function startApp() {
  const child = spawn(state.bin, [], {
    env: {
      ...process.env,
      DEVDESK_DATA_ROOT: state.dataRoot,
      DEVDESK_WEB_AUTOSTART: '1',
      DEVDESK_WEB_PORT: String(state.port),
      DEVDESK_WEB_TEST_HOOKS: '1',
    },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  const keep = (buf) => {
    state.appLog = (state.appLog + buf.toString()).slice(-8000);
  };
  child.stdout.on('data', keep);
  child.stderr.on('data', keep);
  child.log = () => state.appLog;
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

function newClient(base) {
  return new WT.RealClient({ base });
}
function frameText(f) {
  if (!f || !f.lines) return '';
  return f.lines.map((r) => (r || []).map((c) => (c && c.ph ? '' : (c && c.ch) || ' ')).join('')).join('\n');
}
async function openStreamRaw(ticket, timeoutMs = 20000) {
  const ctrl = new AbortController();
  const res = await withTimeout(
    realFetch(`${state.base}/api/stream?t=${encodeURIComponent(ticket)}`, { signal: ctrl.signal }),
    timeoutMs, 'SSE 响应头');
  return { res, ctrl };
}
function drain(res) {
  (async () => {
    try {
      const rd = res.body.getReader();
      for (;;) {
        const { done } = await rd.read();
        if (done) break;
      }
    } catch { /* 关闭即结束 */ }
  })();
}

async function main() {
  state.bin = resolveBin();
  assert(state.bin, '找不到产品二进制（用 DEVDESK_E2E_BIN 或 --bin 指定）');
  state.host = lanIPv4();
  assert(state.host, '非回环地址不可用：未找到局域网 IPv4（鉴权双路径必须非回环）');
  state.port = await freePort();
  state.dataRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'devdesk-auth-e2e-'));
  state.base = `http://${state.host}:${state.port}`;
  const base = state.base;
  log('ENV', `host=${state.host} port=${state.port} bin=${state.bin}`);

  const app = startApp();
  try {
    assert(await waitHttp(`${base}/api/status`, 30000), `产品 30s 内未监听 ${base}/api/status`);
    const status = await (await realFetch(`${base}/api/status`)).json();
    assert(status.running === true, `/api/status running=false: ${JSON.stringify(status)}`);
    state.steps.push('server-up');
    log('STEP server-up', JSON.stringify(status));

    // ==== 1. 非回环 claim 的令牌校验（负路径）====
    const c1 = newClient(base);
    await c1._handshake();
    const noToken = await expectErr(c1._req(WT.API.bindClaim, { m: 'bind.claim', b: { token: '' } }));
    assert(noToken && noToken.code === 'no_token' && noToken.inner === 400,
      `非回环空令牌 claim 未拒绝 no_token/400: ${JSON.stringify(noToken)}`);
    const badToken = await expectErr(c1._req(WT.API.bindClaim, { m: 'bind.claim', b: { token: 'not-the-token' } }));
    assert(badToken && badToken.code === 'bad_token' && badToken.inner === 400,
      `非回环错误令牌 claim 未拒绝 bad_token/400: ${JSON.stringify(badToken)}`);
    state.steps.push('claim-token-rejected');
    log('STEP claim-token-rejected', `no_token=${noToken.inner} bad_token=${badToken.inner}`);

    // ==== 2. claim 取 secret（测试缝模拟回环：令牌只在桌面二维码内）====
    hook.addr = '127.0.0.1';
    const claim = await c1._req(WT.API.bindClaim, { m: 'bind.claim', b: { token: '' } });
    hook.addr = null;
    assert(claim && claim.secret && claim.otpauth, `bind.claim 未返回 secret/otpauth: ${JSON.stringify(claim)}`);
    assert(claim.secret.length === 32, `base32 secret 长度异常: ${claim.secret.length}`);
    assert(/^otpauth:\/\/totp\/[^?]+\?secret=[A-Z2-7]+&issuer=DevDesk&algorithm=SHA1&digits=6&period=30$/.test(claim.otpauth),
      `otpauth 参数不符: ${claim.otpauth}`);
    state.steps.push('bind-claim');
    log('STEP bind-claim', `secretLen=${claim.secret.length} otpauth=${claim.otpauth.slice(0, 42)}…`);

    // ==== 3. 自算 TOTP confirm（真实局域网地址）→ 票据 → 终端视图 ====
    const code = totp(claim.secret);
    await c1.bindConfirm(code);
    assert(c1._ticket, 'bind.confirm 未返回票据');
    assert(c1.status === 'connected' && c1.view === 'terminal',
      `confirm 后状态异常 status=${c1.status} view=${c1.view}`);
    state.steps.push('totp-confirm+ticket');
    log('STEP totp-confirm+ticket', `ticket=${c1._ticket.slice(0, 12)}… view=${c1.view}`);

    // ==== 4. SSE 首帧（建终端）与密文输入回显 ====
    const frames = [];
    c1.onFrame((f) => frames.push(f));
    await c1.newTab();
    const deadlineGrid = Date.now() + 20000;
    let first = null;
    while (Date.now() < deadlineGrid && !first) {
      first = frames.find((f) => f && f.rows > 0 && f.cols > 0) || null;
      if (!first) await sleep(150);
    }
    assert(first, 'SSE 20s 内无网格帧');
    assert(first.rows >= 8 && first.rows <= 200 && first.cols >= 20 && first.cols <= 200,
      `首帧几何异常 rows=${first.rows} cols=${first.cols}`);
    state.steps.push('sse-first-frame');
    log('STEP sse-first-frame', `rows=${first.rows} cols=${first.cols} frames=${frames.length}`);

    const marker = 'auth490' + Date.now().toString().slice(-6);
    c1.sendInput(`echo ${marker}\r`);
    const deadlineEcho = Date.now() + 20000;
    let echoed = false;
    while (Date.now() < deadlineEcho && !echoed) {
      echoed = frames.some((f) => frameText(f).includes(marker));
      if (!echoed) await sleep(200);
    }
    assert(echoed, `密文输入 20s 内未回显: ${marker}`);
    assert(hook.sealedInput && hook.sealedInput.firstByte === 1 && hook.sealedInput.len > 29,
      `输入请求未见密文信封: ${JSON.stringify(hook.sealedInput)}`);
    assert(hook.sealedResp === 'v1', `响应未标 X-DevDesk-Envelope: v1 (got ${hook.sealedResp})`);
    state.steps.push('cipher-input-echo');
    log('STEP cipher-input-echo', `marker=${marker} input=${JSON.stringify(hook.sealedInput)} envelope=${hook.sealedResp}`);

    // ==== 5. SSE 并发上限：8 条成功、第 9 条 503、其它 API 仍响应 ====
    // c1 的 SSE 已占 1 条，再开 7 条 = 8；第 9 条必须 503。
    const ncpu = (os.cpus() || []).length;
    const pool = Math.max(8, ncpu - 1);
    log('ENV', `ncpu=${ncpu} httplib_pool=${pool}（SSE 上限 8 需 pool≥9）`);
    const extraStreams = [];
    for (let i = 0; i < 7; i++) {
      const t = (await c1._req(WT.API.env, { m: 'stream.ticket' })).t;
      assert(t, `第 ${i + 2} 条 stream.ticket 未返回票据`);
      const { res, ctrl } = await openStreamRaw(t);
      assert(res.status === 200, `第 ${i + 2} 条 SSE 未 200: ${res.status}`);
      drain(res);
      extraStreams.push(ctrl);
    }
    state.steps.push('sse-8-open');
    log('STEP sse-8-open', `open=8`);

    const t9 = (await c1._req(WT.API.env, { m: 'stream.ticket' })).t;
    let ninth;
    try {
      ninth = await openStreamRaw(t9, pool >= 9 ? 15000 : 5000);
    } catch (e) {
      throw new Error(`第 9 条 SSE 请求未返回（线程池 ${pool}<9 时 8 条订阅会占满工作线程，503 分支不可达）: ${e.message}`);
    }
    const ninthBody = await ninth.res.text();
    assert(ninth.res.status === 503 && ninthBody.includes('too_many_streams'),
      `第 9 条 SSE 应为 503 too_many_streams，实际 ${ninth.res.status} ${ninthBody}`);
    state.steps.push('sse-9th-503');
    log('STEP sse-9th-503', `status=${ninth.res.status} body=${ninthBody}`);

    // 8 条长连接占用工作线程期间，其它 API 仍可响应（pool 至少余 1 条）
    const statusWhileFull = await withTimeout(realFetch(`${base}/api/status`), 10000, '满订阅时 /api/status');
    assert(statusWhileFull.status === 200, `8 条流在线时 /api/status 未响应: ${statusWhileFull.status}`);
    state.steps.push('api-alive-under-load');
    log('STEP api-alive-under-load', `status=${statusWhileFull.status}`);

    for (const ctrl of extraStreams) { try { ctrl.abort(); } catch {} }
    await sleep(500);

    // ==== 6. 第二条连接用 TOTP 登录（非回环）→ 票据可用 ====
    const c2 = newClient(base);
    await c2._handshake();
    const login2 = await c2._req(WT.API.login, { m: 'auth.login', b: { code: totp(claim.secret) } });
    assert(login2 && login2.t, `第二连接 TOTP 登录未返回票据: ${JSON.stringify(login2)}`);
    c2._ticket = login2.t;
    const state2 = await c2._req(WT.API.env, { m: 'state' });
    assert(state2 && state2.settings, `第二连接票据不可用: ${JSON.stringify(state2)}`);
    state.steps.push('second-conn-totp');
    log('STEP second-conn-totp', `ticket=${login2.t.slice(0, 12)}…`);

    // ==== 7. 网页 revoke → 其它连接失效（旧票据被拒）====
    await c1._req(WT.API.bindRevoke, { m: 'bind.revoke' });
    const revokedSelf = await expectErr(c1._req(WT.API.env, { m: 'state' }));
    assert(revokedSelf && revokedSelf.code === 'bad_ticket',
      `revoke 后发起端旧票据仍可用: ${JSON.stringify(revokedSelf)}`);
    const revokedOther = await expectErr(c2._req(WT.API.env, { m: 'state' }));
    assert(revokedOther && revokedOther.code === 'bad_ticket',
      `revoke 后其它连接旧票据仍可用: ${JSON.stringify(revokedOther)}`);
    const revokedInput = await expectErr(c2._sealed(WT.API.input, { m: 'input', b: { text: 'x' } }));
    assert(revokedInput && revokedInput.code === 'bad_ticket',
      `revoke 后其它连接输入未被拒: ${JSON.stringify(revokedInput)}`);
    state.steps.push('revoke-invalidates-others');
    log('STEP revoke-invalidates-others', `self=${revokedSelf.code} other=${revokedOther.code} input=${revokedInput.code}`);

    // ==== 8. 票据满：Http 200 + 密文内 409（回环登录签发 64 张）====
    const cCap = newClient(base);
    await cCap._handshake();
    hook.addr = '127.0.0.1';   // 回环免码登录只用于批量签发
    let issued = 0;
    let capErr = null;
    for (let i = 0; i < 66; i++) {
      const r = await expectErr(cCap._req(WT.API.login, { m: 'auth.login', b: { code: '' } }));
      if (r) { capErr = r; break; }
      issued++;
    }
    hook.addr = null;
    assert(issued === 64, `票据上限前成功签发数=${issued}（期望 64）`);
    assert(capErr && capErr.code === 'too_many_tickets' && capErr.inner === 409 && capErr.http === 200,
      `票据满口径异常: ${JSON.stringify(capErr)}`);
    state.steps.push('ticket-cap-409');
    log('STEP ticket-cap-409', `issued=${issued} next=${capErr.code}/inner${capErr.inner}/http${capErr.http}`);

    // ==== 9. 登录锁定：HTTP 200 + 密文内 429 + retryAfter ====
    const cLock = newClient(base);
    await cLock._handshake();
    hook.addr = '10.7.7.7';    // 非回环 IP，走真实 TOTP 校验分支
    let lockErr = null;
    for (let i = 0; i < 8; i++) {
      const r = await expectErr(cLock._req(WT.API.login, { m: 'auth.login', b: { code: '000000' } }));
      if (r && r.code === 'locked') { lockErr = r; break; }
    }
    hook.addr = null;
    assert(lockErr && lockErr.inner === 429 && lockErr.http === 200 && lockErr.retryAfter > 0,
      `登录锁定口径异常: ${JSON.stringify(lockErr)}`);
    state.steps.push('login-lockout-429');
    log('STEP login-lockout-429', `inner=${lockErr.inner} http=${lockErr.http} retryAfter=${lockErr.retryAfter}`);

    return 0;
  } finally {
    await stopApp(app);
  }
}

let code = 0;
try {
  code = await main();
  log('E2E_OK ' + state.steps.join(' -> '));
  log('UNVERIFIED 网页 revoke 不广播 revoke：其它 SSE 连接不主动断开（仅票据失效，已断言）');
} catch (err) {
  code = 1;
  log('E2E_FAIL ' + (err && err.message ? err.message : String(err)));
  if (state.appLog) log('APP_LOG_TAIL\n' + state.appLog.split('\n').slice(-12).join('\n'));
}
try {
  fs.writeFileSync(path.join(ART, 'auth_e2e.log'), logLines.join('\n') + '\n');
} catch (e) {
  console.log('WARN 证据写入失败', e.message);
}
console.log(JSON.stringify({ host: state.host, port: state.port, steps: state.steps }, null, 2));
process.exit(code);
