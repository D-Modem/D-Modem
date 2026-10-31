# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
# SRTP (RFC 3711) for the tests, in plain Python (CI need not have a
# crypto package): AES_CM_128_HMAC_SHA1_80 and _32, one master key, no
# MKI, key derivation rate 0.
import hashlib, hmac

def _xt(a):
    return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1

# the S-box, from GF(2^8) inverses and the affine map
_SBOX = [0] * 256
def _sbox():
    p = q = 1
    while True:
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1; q ^= q << 2; q ^= q << 4; q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        x = q ^ ((q << 1 | q >> 7) & 0xFF) ^ ((q << 2 | q >> 6) & 0xFF) ^ \
            ((q << 3 | q >> 5) & 0xFF) ^ ((q << 4 | q >> 4) & 0xFF)
        _SBOX[p] = x ^ 0x63
        if p == 1:
            break
    _SBOX[0] = 0x63
_sbox()

# AES-128, encryption only (all that counter mode needs)
class Aes128:
    def __init__(self, key):
        w = [list(key[i:i + 4]) for i in range(0, 16, 4)]
        rc = 1
        for i in range(4, 44):
            t = list(w[i - 1])
            if i % 4 == 0:
                t = [_SBOX[b] for b in t[1:] + t[:1]]
                t[0] ^= rc
                rc = _xt(rc)
            w.append([a ^ b for a, b in zip(w[i - 4], t)])
        self.rk = [sum(w[4 * r:4 * r + 4], []) for r in range(11)]
    def encrypt(self, block):
        s = [a ^ b for a, b in zip(block, self.rk[0])]
        for r in range(1, 11):
            s = [_SBOX[b] for b in s]
            # rows shifted (state column major: s[c*4 + row])
            s = [s[((c + row) % 4) * 4 + row] for c in range(4) for row in range(4)]
            if r < 10:
                m = []
                for c in range(4):
                    a = s[c * 4:c * 4 + 4]
                    t = a[0] ^ a[1] ^ a[2] ^ a[3]
                    m += [a[i] ^ t ^ _xt(a[i] ^ a[(i + 1) % 4]) for i in range(4)]
                s = m
            s = [a ^ b for a, b in zip(s, self.rk[r])]
        return bytes(s)

# n bytes of AES counter mode keystream from the 128 bit iv
def _keystream(aes, iv, n):
    out = b""
    for i in range((n + 15) // 16):
        out += aes.encrypt(((iv + i) & ((1 << 128) - 1)).to_bytes(16, "big"))
    return out[:n]

# one direction's session keys, from a 30 byte master key || salt
class Srtp:
    def __init__(self, master, tag=10):
        aes = Aes128(master[:16])
        salt = int.from_bytes(master[16:30], "big")
        def derive(label, n):
            return _keystream(aes, ((label << 48) ^ salt) << 16, n)
        self.aes = Aes128(derive(0, 16))
        self.auth = derive(1, 20)
        self.salt = int.from_bytes(derive(2, 14), "big")
        self.tag = tag
        self.roc, self.last = 0, None
    def _crypt(self, ssrc, index, data):
        iv = (self.salt << 16) ^ (ssrc << 64) ^ (index << 16)
        ks = _keystream(self.aes, iv, len(data))
        return bytes(a ^ b for a, b in zip(data, ks))
    def _mac(self, pkt, roc):
        return hmac.new(self.auth, pkt + roc.to_bytes(4, "big"),
                        hashlib.sha1).digest()[:self.tag]
    # an RTP packet (12 byte header, no CSRCs or extension) sent
    def protect(self, pkt):
        seq = int.from_bytes(pkt[2:4], "big")
        if self.last is not None and seq < self.last:
            self.roc += 1
        self.last = seq
        ssrc = int.from_bytes(pkt[8:12], "big")
        out = pkt[:12] + self._crypt(ssrc, self.roc << 16 | seq, pkt[12:])
        return out + self._mac(out, self.roc)
    # an SRTP packet received: its RTP packet, None if it fails its tag
    def unprotect(self, pkt):
        if len(pkt) < 12 + self.tag:
            return None
        body, tag = pkt[:-self.tag], pkt[-self.tag:]
        seq = int.from_bytes(body[2:4], "big")
        roc = self.roc
        if self.last is not None and seq < self.last and self.last - seq > 0x8000:
            roc += 1
        if not hmac.compare_digest(self._mac(body, roc), tag):
            return None
        if self.last is None or roc > self.roc or seq > self.last:
            self.roc, self.last = roc, seq
        ssrc = int.from_bytes(body[8:12], "big")
        return body[:12] + self._crypt(ssrc, roc << 16 | seq, body[12:])
