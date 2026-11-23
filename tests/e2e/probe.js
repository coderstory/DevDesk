// Capability probe: self-executing in any document, writes window.__CAP__ and #out.
// Standard Web APIs only; failures are recorded by e.name, never thrown.
(async () => {
  async function probe(fn) {
    try { return { ok: true, detail: await fn() }; }
    catch (e) {
      return { ok: false, error: (e && (e.name || 'Error')) + ': ' + (e && e.message ? e.message : String(e)) };
    }
  }

  const x25519 = () => probe(async () => {
    const kp = await crypto.subtle.generateKey({ name: 'X25519' }, true, ['deriveBits']);
    const peer = await crypto.subtle.generateKey({ name: 'X25519' }, true, ['deriveBits']);
    const bits = await crypto.subtle.deriveBits({ name: 'X25519', public: peer.publicKey }, kp.privateKey, 256);
    return bits.byteLength * 8 + '-bit shared secret';
  });

  const hkdf = () => probe(async () => {
    const ikm = crypto.getRandomValues(new Uint8Array(32));
    const key = await crypto.subtle.importKey('raw', ikm, 'HKDF', false, ['deriveBits']);
    const bits = await crypto.subtle.deriveBits(
      { name: 'HKDF', hash: 'SHA-256', salt: new Uint8Array(16), info: new Uint8Array(8) }, key, 256);
    return bits.byteLength * 8 + '-bit derived key';
  });

  const aesgcm = () => probe(async () => {
    const key = await crypto.subtle.generateKey({ name: 'AES-GCM', length: 256 }, true, ['encrypt', 'decrypt']);
    const iv = crypto.getRandomValues(new Uint8Array(12));
    const ct = await crypto.subtle.encrypt({ name: 'AES-GCM', iv }, key, new TextEncoder().encode('devdesk'));
    const pt = new TextDecoder().decode(await crypto.subtle.decrypt({ name: 'AES-GCM', iv }, key, ct));
    if (pt !== 'devdesk') throw new Error('roundtrip mismatch: ' + pt);
    return 'encrypt/decrypt roundtrip ok';
  });

  const clipboard = async () => {
    const out = { api: null, write: null, read: null };
    const c = navigator.clipboard;
    out.api = !!(c && typeof c.writeText === 'function' && typeof c.readText === 'function');
    if (out.api) {
      try { await c.writeText('devdesk-probe'); out.write = 'ok'; }
      catch (e) { out.write = (e && (e.name || 'Error')) + ': ' + (e && e.message ? e.message : ''); }
      try { await c.readText(); out.read = 'ok'; }
      catch (e) { out.read = (e && (e.name || 'Error')) + ': ' + (e && e.message ? e.message : ''); }
    }
    return out;
  };

  const subtle = !!(window.crypto && window.crypto.subtle);
  const vv = window.visualViewport;
  const result = {
    ua: navigator.userAgent,
    secureContext: window.isSecureContext,
    protocol: location.protocol,
    origin: location.origin,
    devicePixelRatio: window.devicePixelRatio,
    viewport: { w: window.innerWidth, h: window.innerHeight },
    visualViewport: vv
      ? { present: true, w: Math.round(vv.width), h: Math.round(vv.height), scale: vv.scale }
      : { present: false },
    subtlePresent: subtle,
    x25519: subtle ? await x25519() : { ok: false, error: 'crypto.subtle unavailable' },
    hkdf: subtle ? await hkdf() : { ok: false, error: 'crypto.subtle unavailable' },
    aesgcm: subtle ? await aesgcm() : { ok: false, error: 'crypto.subtle unavailable' },
    clipboard: await clipboard(),
  };
  window.__CAP__ = result;
  const pre = document.getElementById('out');
  if (pre) pre.textContent = JSON.stringify(result, null, 2);
})();
