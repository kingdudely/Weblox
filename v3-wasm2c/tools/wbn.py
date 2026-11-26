#!/usr/bin/env python3
"""Minimal CBOR web-bundle reader: list entries, optionally extract one by path."""
import sys, json

class R:
    def __init__(self, b, i=0):
        self.b = b
        self.i = i

    def byte(self):
        v = self.b[self.i]; self.i += 1; return v

    def take(self, n):
        v = self.b[self.i:self.i + n]; self.i += n; return v

    def head(self):
        ib = self.byte()
        major, ai = ib >> 5, ib & 31
        if ai < 24: return major, ai
        if ai == 24: return major, self.byte()
        if ai == 25: return major, int.from_bytes(self.take(2), 'big')
        if ai == 26: return major, int.from_bytes(self.take(4), 'big')
        if ai == 27: return major, int.from_bytes(self.take(8), 'big')
        raise ValueError('bad ai %d' % ai)

    def value(self):
        major, n = self.head()
        if major == 0: return n
        if major == 1: return -1 - n
        if major == 2: return self.take(n)
        if major == 3: return self.take(n).decode('utf-8', 'replace')
        if major == 4: return [self.value() for _ in range(n)]
        if major == 5:
            d = {}
            for _ in range(n):
                k = self.value(); d[k if not isinstance(k, list) else str(k)] = self.value()
            return d
        if major == 7:
            if n in (20, 21, 22): return [False, True, None][n - 20]
            if n == 25:
                import struct; return struct.unpack('>e', self.take(2))[0]
            if n == 26:
                import struct; return struct.unpack('>f', self.take(4))[0]
            if n == 27:
                import struct; return struct.unpack('>d', self.take(8))[0]
            return None
        raise ValueError('major %d' % major)


def entries(path):
    b = open(path, 'rb').read()
    r = R(b)
    top = r.value()
    return top


def main():
    path, mode = sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else 'list'
    top = entries(path)
    res = top[2] if len(top) > 2 else []
    rows = []
    for e in res:
        hdrs, body = e[0], e[1]
        url = hdrs.get(':url') or hdrs.get(b':url')
        if isinstance(url, bytes):
            url = url.decode('utf-8', 'replace')
        rows.append((url or '?', len(body), body))
    if mode == 'list':
        for u, n, _ in rows:
            print(f'{n:>10} {u}')
    else:
        url = mode
        for u, n, body in rows:
            if u == url or u.endswith(url):
                sys.stdout.buffer.write(body)
                return
        sys.exit('not found: ' + url)


main()
