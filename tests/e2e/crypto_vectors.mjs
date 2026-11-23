// need#490 W5：crypto.js 的 Node 侧向量回归（RFC 7748/5869/8439 + Node 原生互通 + W3 端点/派生口径）。
// 运行：node tests/e2e/crypto_vectors.mjs
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import crypto from 'node:crypto';

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, '../../src/module_terminal/web/www/crypto.js'), 'utf8');
new Function('globalThis', source)(globalThis);
const C = globalThis.WTCrypto;

let failed = 0;
function check(name, fn) {
  try { fn(); console.log('PASS', name); }
  catch (e) { failed++; console.log('FAIL', name, '-', e && e.message || e); }
}
function eq(a, b, label) {
  if (String(a) !== String(b)) throw new Error(`${label}: got ${a} want ${b}`);
}

check('RFC 自检套件', () => {
  const r = C.selfTest();
  if (!r.ok) throw new Error(r.results.filter(x => !x.ok).map(x => x.name + ':' + x.detail).join('; '));
});

check('HKDF vs Node', () => {
  const ikm = Buffer.from('0b'.repeat(22), 'hex');
  const salt = Buffer.from('000102030405060708090a0b0c', 'hex');
  const info = Buffer.from('f0f1f2f3f4f5f6f7f8f9', 'hex');
  const node = Buffer.from(crypto.hkdfSync('sha256', ikm, salt, info, 42));
  const js = Buffer.from(C.hkdf(new Uint8Array(salt), new Uint8Array(ikm), new Uint8Array(info), 42));
  eq(js.toString('hex'), node.toString('hex'), 'okm');
});

check('X25519 vs Node', () => {
  const a = C.x25519Generate();
  const b = C.x25519Generate();
  const priv = (raw) => crypto.createPrivateKey({
    key: Buffer.concat([Buffer.from('302e020100300506032b656e04220420', 'hex'), Buffer.from(raw)]),
    format: 'der', type: 'pkcs8'
  });
  const pub = (raw) => crypto.createPublicKey({
    key: Buffer.concat([Buffer.from('302a300506032b656e032100', 'hex'), Buffer.from(raw)]),
    format: 'der', type: 'spki'
  });
  const nodeShared = crypto.diffieHellman({ privateKey: priv(a.privateKey), publicKey: pub(b.publicKey) });
  const jsShared = C.x25519(a.privateKey, b.publicKey);
  eq(Buffer.from(jsShared).toString('hex'), nodeShared.toString('hex'), 'shared');
});

check('ChaCha20-Poly1305 vs Node', () => {
  const key = crypto.randomBytes(32);
  const nonce = crypto.randomBytes(12);
  const aad = crypto.randomBytes(13);
  const pt = Buffer.from('DevDesk web terminal 加密帧互通 '.repeat(8));
  const cipher = crypto.createCipheriv('chacha20-poly1305', key, nonce, { authTagLength: 16 });
  cipher.setAAD(aad);
  const nodeCt = Buffer.concat([cipher.update(pt), cipher.final()]);
  const nodeTag = cipher.getAuthTag();
  const js = C.aeadSeal(new Uint8Array(key), new Uint8Array(nonce), new Uint8Array(pt), new Uint8Array(aad));
  eq(Buffer.from(js.ciphertext).toString('hex'), nodeCt.toString('hex'), 'ct');
  eq(Buffer.from(js.tag).toString('hex'), nodeTag.toString('hex'), 'tag');
  const opened = C.aeadOpen(new Uint8Array(key), new Uint8Array(nonce), new Uint8Array(nodeCt), new Uint8Array(nodeTag), new Uint8Array(aad));
  eq(Buffer.from(opened).toString('utf8'), pt.toString('utf8'), 'plaintext');
});

check('SHA-256 vs Node', () => {
  const data = crypto.randomBytes(1000);
  eq(Buffer.from(C.sha256(new Uint8Array(data))).toString('hex'), crypto.createHash('sha256').update(data).digest('hex'), 'digest');
});

check('W3 端点与协议源码一致', () => {
  const src = readFileSync(join(here, '../../src/module_terminal/web/www/protocol.js'), 'utf8');
  const required = ["'/api/hs'", "'/api/e'", "'/api/stream'", "'/api/input'", "'/api/settings'",
    "'/api/login'", "'/api/logout'", "'/api/bind/claim'", "'/api/bind/confirm'", "'/api/bind/revoke'",
    "'/api/status'", 'X-DevDesk-Session'];
  for (const needle of required) {
    if (!src.includes(needle)) throw new Error('missing ' + needle);
  }
  if (src.includes('/api/handshake')) throw new Error('stale /api/handshake');
});

check('HKDF 方向 info 与 W3 deriveInfo 一致', () => {
  const kp = C.x25519Generate();
  const spk = C.x25519Generate().publicKey;
  const salt = crypto.randomBytes(32);
  const hid = 'test-hid-22chars';
  const keys = C.deriveSessionKeys(kp.privateKey, kp.publicKey, hid, spk, new Uint8Array(salt));
  const shared = Buffer.from(C.x25519(kp.privateKey, spk));
  for (const dir of ['c2s', 's2c']) {
    const info = Buffer.from(`ddweb/v1/${dir}|${hid}|${C.b64urlEncode(kp.publicKey)}|${C.b64urlEncode(spk)}`);
    const node = Buffer.from(crypto.hkdfSync('sha256', shared, salt, info, 32));
    eq(Buffer.from(keys[dir]).toString('hex'), node.toString('hex'), dir);
  }
});

console.log(failed ? `FAILED ${failed}` : 'ALL PASS');
process.exit(failed ? 1 : 0);
