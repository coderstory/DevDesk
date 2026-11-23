/* 网页终端纯 JS 密码学（need#490 W5）。
 *
 * 局域网明文 HTTP 是非安全上下文，crypto.subtle 不存在（ADR 0010），因此
 * X25519(RFC 7748) / HKDF-SHA256(RFC 5869) / ChaCha20-Poly1305(RFC 8439) 全部自实现。
 * 随机数只允许 crypto.getRandomValues；本文件零 crypto.subtle 引用。
 */
(function (global) {
  'use strict';

  function utf8(s) { return new TextEncoder().encode(s); }
  function fromUtf8(b) { return new TextDecoder().decode(b); }
  function concat() {
    var n = 0, i;
    for (i = 0; i < arguments.length; i++) n += arguments[i].length;
    var out = new Uint8Array(n), o = 0;
    for (i = 0; i < arguments.length; i++) { out.set(arguments[i], o); o += arguments[i].length; }
    return out;
  }
  function bytesEqual(a, b) {
    if (a.length !== b.length) return false;
    var d = 0;
    for (var i = 0; i < a.length; i++) d |= a[i] ^ b[i];
    return d === 0;
  }
  function hexToBytes(hex) {
    hex = hex.replace(/\s+/g, '');
    var out = new Uint8Array(hex.length / 2);
    for (var i = 0; i < out.length; i++) out[i] = parseInt(hex.substr(i * 2, 2), 16);
    return out;
  }
  function bytesToHex(b) {
    var s = '';
    for (var i = 0; i < b.length; i++) s += (b[i] < 16 ? '0' : '') + b[i].toString(16);
    return s;
  }
  var B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  function b64encode(bytes) {
    var s = '', i;
    for (i = 0; i + 2 < bytes.length; i += 3) {
      var n = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
      s += B64[(n >> 18) & 63] + B64[(n >> 12) & 63] + B64[(n >> 6) & 63] + B64[n & 63];
    }
    var rem = bytes.length - i;
    if (rem === 1) {
      var a = bytes[i] << 16;
      s += B64[(a >> 18) & 63] + B64[(a >> 12) & 63] + '==';
    } else if (rem === 2) {
      var b = (bytes[i] << 16) | (bytes[i + 1] << 8);
      s += B64[(b >> 18) & 63] + B64[(b >> 12) & 63] + B64[(b >> 6) & 63] + '=';
    }
    return s;
  }
  function b64decode(str) {
    str = String(str).replace(/-/g, '+').replace(/_/g, '/').replace(/\s+/g, '');
    while (str.length % 4) str += '=';
    var pad = str.indexOf('=') >= 0 ? str.length - str.indexOf('=') : 0;
    var out = new Uint8Array((str.length / 4) * 3 - pad), o = 0;
    for (var i = 0; i < str.length; i += 4) {
      var n = (B64.indexOf(str[i]) << 18) | (B64.indexOf(str[i + 1]) << 12) |
        ((B64.indexOf(str[i + 2]) & 63) << 6) | (B64.indexOf(str[i + 3]) & 63);
      if (o < out.length) out[o++] = (n >> 16) & 255;
      if (o < out.length) out[o++] = (n >> 8) & 255;
      if (o < out.length) out[o++] = n & 255;
    }
    return out;
  }
  function b64urlEncode(bytes) { return b64encode(bytes).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, ''); }
  function b64urlDecode(str) { return b64decode(str); }
  function randomBytes(n) {
    var b = new Uint8Array(n);
    if (global.crypto && global.crypto.getRandomValues) global.crypto.getRandomValues(b);
    else throw new Error('getRandomValues unavailable');
    return b;
  }
  function u32be(n) {
    return new Uint8Array([(n >>> 24) & 255, (n >>> 16) & 255, (n >>> 8) & 255, n & 255]);
  }
  function u64be(n) {
    var hi = Math.floor(n / 0x100000000), lo = n >>> 0;
    return concat(u32be(hi), u32be(lo));
  }
  function u64le(n) {
    var hi = Math.floor(n / 0x100000000), lo = n >>> 0;
    return new Uint8Array([lo & 255, (lo >>> 8) & 255, (lo >>> 16) & 255, (lo >>> 24) & 255,
      hi & 255, (hi >>> 8) & 255, (hi >>> 16) & 255, (hi >>> 24) & 255]);
  }

  var K256 = new Uint32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
  ]);
  function rotr(x, n) { return (x >>> n) | (x << (32 - n)); }
  function rotl(x, n) { return (x << n) | (x >>> (32 - n)); }
  function sha256(data) {
    var h = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
    var len = data.length;
    var padded = new Uint8Array(((len + 9 + 63) >> 6) << 6);
    padded.set(data);
    padded[len] = 0x80;
    var bitLen = len * 8;
    var dv = new DataView(padded.buffer);
    dv.setUint32(padded.length - 4, bitLen >>> 0, false);
    dv.setUint32(padded.length - 8, Math.floor(bitLen / 0x100000000), false);
    var w = new Uint32Array(64);
    for (var off = 0; off < padded.length; off += 64) {
      for (var i = 0; i < 16; i++) w[i] = dv.getUint32(off + i * 4, false);
      for (i = 16; i < 64; i++) {
        var s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >>> 3);
        var s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >>> 10);
        w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
      }
      var a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
      for (i = 0; i < 64; i++) {
        var S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        var ch = (e & f) ^ (~e & g);
        var t1 = (hh + S1 + ch + K256[i] + w[i]) >>> 0;
        var S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        var maj = (a & b) ^ (a & c) ^ (b & c);
        var t2 = (S0 + maj) >>> 0;
        hh = g; g = f; f = e; e = (d + t1) >>> 0;
        d = c; c = b; b = a; a = (t1 + t2) >>> 0;
      }
      h[0] = (h[0] + a) >>> 0; h[1] = (h[1] + b) >>> 0; h[2] = (h[2] + c) >>> 0; h[3] = (h[3] + d) >>> 0;
      h[4] = (h[4] + e) >>> 0; h[5] = (h[5] + f) >>> 0; h[6] = (h[6] + g) >>> 0; h[7] = (h[7] + hh) >>> 0;
    }
    var out = new Uint8Array(32), odv = new DataView(out.buffer);
    for (i = 0; i < 8; i++) odv.setUint32(i * 4, h[i], false);
    return out;
  }

  function sha1(data) {
    var h = new Uint32Array([0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0]);
    var len = data.length;
    var padded = new Uint8Array(((len + 9 + 63) >> 6) << 6);
    padded.set(data); padded[len] = 0x80;
    var dv = new DataView(padded.buffer);
    dv.setUint32(padded.length - 4, len * 8 >>> 0, false);
    dv.setUint32(padded.length - 8, Math.floor(len * 8 / 0x100000000), false);
    var w = new Uint32Array(80);
    for (var off = 0; off < padded.length; off += 64) {
      for (var i = 0; i < 16; i++) w[i] = dv.getUint32(off + i * 4, false);
      for (i = 16; i < 80; i++) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
      var a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
      for (i = 0; i < 80; i++) {
        var f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        var t = (rotl(a, 5) + f + e + k + w[i]) >>> 0;
        e = d; d = c; c = rotl(b, 30); b = a; a = t;
      }
      h[0] = (h[0] + a) >>> 0; h[1] = (h[1] + b) >>> 0; h[2] = (h[2] + c) >>> 0;
      h[3] = (h[3] + d) >>> 0; h[4] = (h[4] + e) >>> 0;
    }
    var out = new Uint8Array(20), odv = new DataView(out.buffer);
    for (i = 0; i < 5; i++) odv.setUint32(i * 4, h[i], false);
    return out;
  }

  function hmac(hash, blockSize, key, data) {
    if (key.length > blockSize) key = hash(key);
    var k = new Uint8Array(blockSize);
    k.set(key);
    var ipad = new Uint8Array(blockSize), opad = new Uint8Array(blockSize);
    for (var i = 0; i < blockSize; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    return hash(concat(opad, hash(concat(ipad, data))));
  }
  function hmacSha256(key, data) { return hmac(sha256, 64, key, data); }
  function hmacSha1(key, data) { return hmac(sha1, 64, key, data); }

  function hkdfExtract(salt, ikm) {
    if (!salt || salt.length === 0) salt = new Uint8Array(32);
    return hmacSha256(salt, ikm);
  }
  function hkdfExpand(prk, info, length) {
    var out = new Uint8Array(0), t = new Uint8Array(0);
    for (var i = 1; out.length < length; i++) {
      t = hmacSha256(prk, concat(t, info || new Uint8Array(0), new Uint8Array([i])));
      out = concat(out, t);
    }
    return out.slice(0, length);
  }
  function hkdf(salt, ikm, info, length) { return hkdfExpand(hkdfExtract(salt, ikm), info, length); }

  var P25519 = (1n << 255n) - 19n;
  function modP(x) { x %= P25519; return x < 0n ? x + P25519 : x; }
  function modPow(base, exp) {
    var r = 1n; base = modP(base);
    while (exp > 0n) {
      if (exp & 1n) r = modP(r * base);
      base = modP(base * base); exp >>= 1n;
    }
    return r;
  }
  function leToBig(bytes) {
    var n = 0n;
    for (var i = bytes.length - 1; i >= 0; i--) n = (n << 8n) | BigInt(bytes[i]);
    return n;
  }
  function bigToLe(n, len) {
    var out = new Uint8Array(len);
    for (var i = 0; i < len; i++) { out[i] = Number(n & 255n); n >>= 8n; }
    return out;
  }
  function decodeScalar(bytes) {
    var b = bytes.slice();
    b[0] &= 248; b[31] &= 127; b[31] |= 64;
    return leToBig(b);
  }
  function decodeU(bytes) {
    var b = bytes.slice();
    b[31] &= 127;
    return modP(leToBig(b));
  }
  function x25519(privateKey, publicKey) {
    if (privateKey.length !== 32 || publicKey.length !== 32) throw new Error('x25519: bad key length');
    var k = decodeScalar(privateKey);
    var x1 = decodeU(publicKey);
    var x2 = 1n, z2 = 0n, x3 = x1, z3 = 1n, swap = 0n;
    var A, AA, B, BB, E, C, D, DA, CB, t;
    for (t = 254n; t >= 0n; t--) {
      var kt = (k >> t) & 1n;
      swap ^= kt;
      if (swap === 1n) {
        var tmp = x2; x2 = x3; x3 = tmp;
        tmp = z2; z2 = z3; z3 = tmp;
      }
      swap = kt;
      A = modP(x2 + z2); AA = modP(A * A);
      B = modP(x2 - z2); BB = modP(B * B);
      E = modP(AA - BB);
      C = modP(x3 + z3); D = modP(x3 - z3);
      DA = modP(D * A); CB = modP(C * B);
      x3 = modP((DA + CB) * (DA + CB));
      z3 = modP(x1 * modP((DA - CB) * (DA - CB)));
      x2 = modP(AA * BB);
      z2 = modP(E * (AA + modP(121665n * E)));
    }
    if (swap === 1n) {
      var t2 = x2; x2 = x3; x3 = t2;
      t2 = z2; z2 = z3; z3 = t2;
    }
    return bigToLe(modP(x2 * modPow(z2, P25519 - 2n)), 32);
  }
  function x25519Generate() {
    var priv = randomBytes(32);
    var pub = x25519(priv, bigToLe(9n, 32));
    return { privateKey: priv, publicKey: pub };
  }

  function chachaQR(x, a, b, c, d) {
    x[a] = (x[a] + x[b]) >>> 0; x[d] = rotl(x[d] ^ x[a], 16);
    x[c] = (x[c] + x[d]) >>> 0; x[b] = rotl(x[b] ^ x[c], 12);
    x[a] = (x[a] + x[b]) >>> 0; x[d] = rotl(x[d] ^ x[a], 8);
    x[c] = (x[c] + x[d]) >>> 0; x[b] = rotl(x[b] ^ x[c], 7);
  }
  function chachaBlock(key, counter, nonce) {
    var x = new Uint32Array(16);
    function rdLE(b, o) { return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0; }
    x[0] = 0x61707865; x[1] = 0x3320646e; x[2] = 0x79622d32; x[3] = 0x6b206574;
    for (var i = 0; i < 8; i++) x[4 + i] = rdLE(key, i * 4);
    x[12] = counter >>> 0;
    for (i = 0; i < 3; i++) x[13 + i] = rdLE(nonce, i * 4);
    var w = x.slice();
    for (i = 0; i < 10; i++) {
      chachaQR(w, 0, 4, 8, 12); chachaQR(w, 1, 5, 9, 13);
      chachaQR(w, 2, 6, 10, 14); chachaQR(w, 3, 7, 11, 15);
      chachaQR(w, 0, 5, 10, 15); chachaQR(w, 1, 6, 11, 12);
      chachaQR(w, 2, 7, 8, 13); chachaQR(w, 3, 4, 9, 14);
    }
    var out = new Uint8Array(64);
    for (i = 0; i < 16; i++) {
      var v = (w[i] + x[i]) >>> 0;
      out[i * 4] = v & 255; out[i * 4 + 1] = (v >>> 8) & 255;
      out[i * 4 + 2] = (v >>> 16) & 255; out[i * 4 + 3] = (v >>> 24) & 255;
    }
    return out;
  }
  function chacha20(key, counter, nonce, data) {
    var out = new Uint8Array(data.length);
    for (var off = 0; off < data.length; off += 64) {
      var ks = chachaBlock(key, (counter + (off / 64)) >>> 0, nonce);
      for (var i = 0; i < 64 && off + i < data.length; i++) out[off + i] = data[off + i] ^ ks[i];
    }
    return out;
  }
  var POLY_P = (1n << 130n) - 5n;
  function poly1305(key, msg) {
    var r = leToBig(key.slice(0, 16));
    r &= 0x0ffffffc0ffffffc0ffffffc0fffffffn;
    var s = leToBig(key.slice(16, 32));
    var acc = 0n;
    for (var off = 0; off < msg.length; off += 16) {
      var chunk = msg.slice(off, Math.min(off + 16, msg.length));
      var n = leToBig(chunk) + (1n << BigInt(8 * chunk.length));
      acc = ((acc + n) * r) % POLY_P;
    }
    return bigToLe((acc + s) & ((1n << 128n) - 1n), 16);
  }
  function pad16(b) {
    var r = b.length % 16;
    return r === 0 ? new Uint8Array(0) : new Uint8Array(16 - r);
  }
  function aeadSeal(key, nonce, plaintext, aad) {
    if (key.length !== 32) throw new Error('aead: bad key');
    if (nonce.length !== 12) throw new Error('aead: bad nonce');
    var a = aad || new Uint8Array(0);
    var polyKey = chachaBlock(key, 0, nonce).slice(0, 32);
    var ct = chacha20(key, 1, nonce, plaintext);
    var macData = concat(a, pad16(a), ct, pad16(ct), u64le(a.length), u64le(ct.length));
    return { ciphertext: ct, tag: poly1305(polyKey, macData) };
  }
  function aeadOpen(key, nonce, ciphertext, tag, aad) {
    var a = aad || new Uint8Array(0);
    var polyKey = chachaBlock(key, 0, nonce).slice(0, 32);
    var macData = concat(a, pad16(a), ciphertext, pad16(ciphertext), u64le(a.length), u64le(ciphertext.length));
    if (!bytesEqual(poly1305(polyKey, macData), tag)) throw new Error('aead: auth failed');
    return chacha20(key, 1, nonce, ciphertext);
  }

  function selfTest() {
    var results = [];
    function check(name, fn) {
      try { fn(); results.push({ name: name, ok: true }); }
      catch (e) { results.push({ name: name, ok: false, detail: String(e && e.message || e) }); }
    }
    function assertEq(actual, expected, label) {
      if (String(actual) !== String(expected)) throw new Error(label + ': got ' + actual + ' want ' + expected);
    }
    check('sha256 abc', function () {
      assertEq(bytesToHex(sha256(utf8('abc'))), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
    });
    check('hmac-sha256 rfc4231-1', function () {
      assertEq(bytesToHex(hmacSha256(new Uint8Array(20).fill(0x0b), utf8('Hi There'))),
        'b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7');
    });
    check('hmac-sha1 rfc2202-1', function () {
      assertEq(bytesToHex(hmacSha1(new Uint8Array(20).fill(0x0b), utf8('Hi There'))),
        'b617318655057264e28bc0b6fb378c8ef146be00');
    });
    check('hkdf-sha256 rfc5869-1', function () {
      var okm = hkdf(hexToBytes('000102030405060708090a0b0c'), new Uint8Array(22).fill(0x0b),
        hexToBytes('f0f1f2f3f4f5f6f7f8f9'), 42);
      assertEq(bytesToHex(okm), '3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865');
    });
    check('x25519 rfc7748-5.2', function () {
      var aPriv = hexToBytes('77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a');
      var aPub = bytesToHex(x25519(aPriv, bigToLe(9n, 32)));
      assertEq(aPub, '8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a');
      var bPriv = hexToBytes('5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb');
      var bPub = bytesToHex(x25519(bPriv, bigToLe(9n, 32)));
      assertEq(bPub, 'de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f');
      assertEq(bytesToHex(x25519(aPriv, hexToBytes(bPub))), '4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742');
    });
    check('chacha20-poly1305 rfc8439-2.8.2', function () {
      var key = hexToBytes('808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f');
      var nonce = hexToBytes('070000004041424344454647');
      var aad = hexToBytes('50515253c0c1c2c3c4c5c6c7');
      var pt = utf8("Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.");
      var r = aeadSeal(key, nonce, pt, aad);
      assertEq(bytesToHex(r.ciphertext),
        'd31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116');
      assertEq(bytesToHex(r.tag), '1ae10b594f09e26a7e902ecbd0600691');
      assertEq(fromUtf8(aeadOpen(key, nonce, r.ciphertext, r.tag, aad)), fromUtf8(pt));
    });
    check('aead tamper rejected', function () {
      var key = new Uint8Array(32).fill(7), nonce = new Uint8Array(12).fill(9);
      var r = aeadSeal(key, nonce, utf8('secret'), new Uint8Array(0));
      r.tag[0] ^= 1;
      var threw = false;
      try { aeadOpen(key, nonce, r.ciphertext, r.tag, new Uint8Array(0)); } catch (e) { threw = true; }
      if (!threw) throw new Error('tampered tag accepted');
    });
    check('base64url roundtrip', function () {
      var b = new Uint8Array([0xfb, 0xff, 0x00, 0x61]);
      assertEq(b64urlEncode(b), '-_8AYQ');
      if (bytesToHex(b64urlDecode('-_8AYQ')) !== 'fbff0061') throw new Error('decode mismatch');
    });
    return { ok: results.every(function (r) { return r.ok; }), results: results };
  }

  global.WTCrypto = {
    utf8: utf8, fromUtf8: fromUtf8, concat: concat, bytesEqual: bytesEqual,
    hexToBytes: hexToBytes, bytesToHex: bytesToHex,
    b64encode: b64encode, b64decode: b64decode, b64urlEncode: b64urlEncode, b64urlDecode: b64urlDecode,
    randomBytes: randomBytes, u64be: u64be, u64le: u64le,
    sha256: sha256, sha1: sha1, hmacSha256: hmacSha256, hmacSha1: hmacSha1,
    hkdf: hkdf, hkdfExtract: hkdfExtract, hkdfExpand: hkdfExpand,
    x25519: x25519, x25519Generate: x25519Generate, chacha20: chacha20, poly1305: poly1305,
    aeadSeal: aeadSeal, aeadOpen: aeadOpen, selfTest: selfTest
  };
})(typeof window !== 'undefined' ? window : globalThis);
