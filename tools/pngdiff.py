import sys, zlib, struct

def load(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, idat, w, h, depth, ctype = 8, b'', 0, 0, 0, 0
    while pos < len(data):
        length, = struct.unpack('>I', data[pos:pos+4])
        kind = data[pos+4:pos+8]
        body = data[pos+8:pos+8+length]
        if kind == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
        elif kind == b'IDAT':
            idat += body
        pos += 12 + length
    assert depth == 8 and ctype == 6, (depth, ctype)
    raw = zlib.decompress(idat)
    stride = w * 4
    out = bytearray(stride * h)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        if f == 1:
            for i in range(4, stride):
                line[i] = (line[i] + line[i-4]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                left = line[i-4] if i >= 4 else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i-4] if i >= 4 else 0
                b = prev[i]
                c = prev[i-4] if i >= 4 else 0
                pa, pb, pc = abs(b-c), abs(a-c), abs(a+b-2*c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        out[y*stride:(y+1)*stride] = line
        prev = line
    return w, h, out

a = load(sys.argv[1]); b = load(sys.argv[2])
assert a[0] == b[0] and a[1] == b[1], (a[:2], b[:2])
w, h = a[0], a[1]
diffs, maxd, total = 0, 0, 0
for i in range(w*h*4):
    d = abs(a[2][i] - b[2][i])
    if d:
        diffs += 1
        total += d
        maxd = max(maxd, d)
print(f"size={w}x{h} differing_bytes={diffs} of {w*h*4} ({100.0*diffs/(w*h*4):.3f}%) max_delta={maxd} mean_delta={total/max(1,diffs):.3f}")
