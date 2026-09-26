#!/usr/bin/env python3
"""awenc.py - OOTW cutscene FMV encoder for Sega Master System.

Input : work/capture/<scene>/{frames.idx,palettes.bin}
Output: build/fmv/<scene>.awv + build/fmv/tiles.bin + tile_off.bin + fmv_index.h

Shared tile dictionary (all scenes):
  tile_off.bin : (NTILES+1) x u16 LE offsets into tiles.bin (even-step entries)
  tiles.bin    : concatenated compact tiles:
     form 0x0C|c       : solid color c (1 byte)
     form 0x10|n : 2-color (c0<nib0>,c1<nib1>) + u64 1bpp bitmask (1= c1) (9 bytes)
     form 0x20         : 4-color (4 nibbles c0..c3) + u128 2bpp (c0..c3) (18 bytes)
     form 0x30         : raw 32 bytes (64 nybbles, hi nibble first)
  Entry lengths: 1, 9, 18, 32. Z80 decompacts to 32-byte VRAM tile.

Scene stream (.awv):
  u32 magic 'AWV0', u16 nframes, u16 reserved
  frame: u8 flags, u16 dur_ticks(60Hz)
    flags bit0: 16 x u8 packed palette follows
    u16 nup  ; nup  x (u16 entry_id, u8 slot)
    u16 ncell; ncell x (u16 cell, u8 slot)
  end: u8 0xFF
Slot 0 = blank. 32x24 cells (768), cell = y*32+x.
"""
import os, sys, struct

W, H = 320, 200
DW, DH = 256, 192
TW = TH = 8
NX, NY = DW // TW, DH // TH
NC = NX * NY
N_SLOTS = 512

CAP = os.path.join(os.path.dirname(__file__), '..', 'work', 'capture')
OUT = os.path.join(os.path.dirname(__file__), '..', 'build', 'fmv')

SCENES = ['titles', 'intro', 'open16002', 'mut16002', 'open16003', 'cistern16003',
          'escape16003', 'open16004', 'exit16004', 'open16005', 'exit16005',
          'open16006', 'gate16006', 'ending', 'password']

TRIM_MARGIN = 6


def squish_frame(src):
    out = bytearray(DW * DH)
    for dy in range(DH):
        sy = dy * H // DH
        row = src[sy * W:(sy + 1) * W]
        base = dy * DW
        sx = 0
        acc = 0
        for dx in range(DW):
            out[base + dx] = row[dx * W // DW]
    return bytes(out)


def rgb6(r, g, b):
    return ((r >> 6) << 4) | ((g >> 6) << 2) | (b >> 6)


MINPX = 3
BUDGET = 500     # max distinct tiles per frame (VRAM has 512 slots, 0 = blank)


def merge_rare(tile, minpx=MINPX):
    """Fold colors with < minpx pixels into the nearest other color (|idx| distance)."""
    cnt = {}
    for v in tile:
        cnt[v] = cnt.get(v, 0) + 1
    rare = [c for c, n in cnt.items() if n < minpx]
    keep = [c for c, n in cnt.items() if n >= minpx]
    if not keep or not rare:
        return list(tile)
    remap = {}
    for c in rare:
        best = min(keep, key=lambda k: abs(k - c))
        remap[c] = best
    return [remap.get(v, v) for v in tile]


def frame_cells(frame):
    """Extract 768 tiles of a squished frame, adaptively merged to fit BUDGET."""
    raw = []
    for c in range(NC):
        tx, ty = c % NX, c // NX
        tile = []
        for y in range(TH):
            row = (ty * TH + y) * DW + tx * TW
            tile.extend(frame[row:row + TW])
        raw.append(tile)
    for minpx in (3, 4, 5, 6, 8, 12, 20, 32, 64):
        merged = [merge_rare(t, minpx) for t in raw]
        keys = set(compact_tile(bytes(t)) for t in merged)
        if len(keys) <= BUDGET or minpx == 64:
            return merged
    return merged


def compact_tile(tile):
    """tile: 64 nybbles (0-15). Return compact bytes."""
    cols = sorted(set(tile))
    n = len(cols)
    if n == 1:
        return bytes([cols[0]])                       # form 0x00 | c solid
    if n == 2:
        c0, c1 = cols
        bits = 0
        for i, v in enumerate(tile):
            if v == c1:
                bits |= 1 << i
        return bytes([0x10, (c1 << 4) | c0]) + bits.to_bytes(8, 'little')
    if n <= 4:
        cmap = cols + [cols[0]] * (4 - n)
        lut = {c: i for i, c in enumerate(cols)}
        val = 0
        for i, v in enumerate(tile):
            val |= lut[v] << (2 * i)
        return bytes([0x20, (cmap[1] << 4) | cmap[0], (cmap[3] << 4) | cmap[2]]) + \
               val.to_bytes(16, 'little')
    b = bytearray(32)
    for i in range(32):
        b[i] = (tile[2 * i] << 4) | tile[2 * i + 1]
    return bytes([0x30]) + bytes(b)


def decompact_tile(data, pos):
    """Return (tile nybbles, next_pos)."""
    form = data[pos]
    if form < 0x10:
        return [form] * 64, pos + 1
    if form & 0xF0 == 0x10:
        b = data[pos + 1]
        c0, c1 = b & 0x0F, b >> 4
        bits = int.from_bytes(data[pos + 2:pos + 10], 'little')
        t = [(c1 if (bits >> i) & 1 else c0) for i in range(64)]
        return t, pos + 10
    if form & 0xF0 == 0x20:
        c = [data[pos + 1] & 0x0F, data[pos + 1] >> 4, data[pos + 2] & 0x0F, data[pos + 2] >> 4]
        val = int.from_bytes(data[pos + 3:pos + 19], 'little')
        t = [c[(val >> (2 * i)) & 3] for i in range(64)]
        return t, pos + 19
    raw = data[pos + 1:pos + 33]
    t = []
    for b in raw:
        t.append(b >> 4)
        t.append(b & 0x0F)
    return t, pos + 33


FORM_LEN = {0x0C: 1, 0x10: 10, 0x20: 19, 0x30: 33}
# NOTE: form 2-color is 2 header bytes + 8 = 10; 4-color 3 + 16 = 19; raw 1 + 32 = 33.


class Dict:
    def __init__(self):
        self.entries = []   # compact bytes
        self.ids = {}

    def add(self, tile):
        k = compact_tile(bytes(tile))
        i = self.ids.get(k)
        if i is None:
            i = len(self.entries)
            self.entries.append(k)
            self.ids[k] = i
        return i


class LRUSim:
    def __init__(self):
        self.slot_of = {}
        self.owner = [None] * N_SLOTS
        self.stamp = [0] * N_SLOTS
        self.now = 0

    def resident(self, did):
        s = self.slot_of.get(did)
        if s is not None and self.owner[s] == did:
            return s
        return None

    def place(self, did):
        s = self.resident(did)
        if s is not None:
            self.stamp[s] = self.now
            return s, False
        free = None
        for i in range(1, N_SLOTS):
            if self.owner[i] is None:
                free = i
                break
        if free is None:
            free = min(range(1, N_SLOTS), key=lambda i: self.stamp[i])
            del self.slot_of[self.owner[free]]
        self.owner[free] = did
        self.slot_of[did] = free
        self.stamp[free] = self.now
        return free, True


def quantize_palette(pal48):
    return [rgb6(pal48[i * 3], pal48[i * 3 + 1], pal48[i * 3 + 2]) for i in range(16)]


def encode_scene(name, dct, report):
    d = os.path.join(CAP, name)
    frames = open(os.path.join(d, 'frames.idx'), 'rb').read()
    pals = open(os.path.join(d, 'palettes.bin'), 'rb').read()
    n = len(frames) // (W * H)

    sq = [squish_frame(frames[i * W * H:(i + 1) * W * H]) for i in range(n)]
    palb = [bytes(quantize_palette(pals[i * 48:(i + 1) * 48])) for i in range(n)]

    last = n - 1
    while last > 0 and sq[last] == sq[last - 1]:
        last -= 1
    nkeep = min(n, last + TRIM_MARGIN)

    runs = []
    for i in range(nkeep):
        if runs and sq[i] == sq[runs[-1][0]] and palb[i] == palb[runs[-1][0]]:
            runs[-1][1] += 1
        else:
            runs.append([i, 1])

    sim = LRUSim()
    out = bytearray()
    out += struct.pack('<IHH', 0x30565741, len(runs), 0)
    nt_prev = [0] * NC
    pal_prev = None
    total_up = total_cell = 0
    maxops = 0

    for fi, cnt in runs:
        frame = sq[fi]
        sim.now += 1
        uploads = []
        cellwrites = []
        slots_now = [0] * NC
        cells = frame_cells(frame)
        for c in range(NC):
            did = dct.add(cells[c])
            s = sim.resident(did)
            if s is None:
                s, ev = sim.place(did)
                uploads.append((did, s))
            else:
                sim.stamp[s] = sim.now
            slots_now[c] = s
            if nt_prev[c] != s:
                cellwrites.append((c, s))
        nt_prev = slots_now
        pal = palb[fi]
        flags = 0
        ops = bytearray()
        if pal != pal_prev:
            flags |= 1
            ops += pal
            pal_prev = pal
        ops += struct.pack('<H', len(uploads))
        for did, s in uploads:
            ops += struct.pack('<HH', did, s)
        ops += struct.pack('<H', len(cellwrites))
        for c, s in cellwrites:
            ops += struct.pack('<HH', c, s)
        dur = max(1, (cnt * 60 + 35) // 70)
        out += struct.pack('<BH', flags, dur) + ops
        total_up += len(uploads)
        total_cell += len(cellwrites)
        maxops = max(maxops, len(uploads) + len(cellwrites))
    out += struct.pack('<B', 0xFF)

    open(os.path.join(OUT, name + '.awv'), 'wb').write(out)
    report[name] = dict(runs=len(runs), uploads=total_up, cellwrites=total_cell,
                        bytes=len(out), maxops=maxops)


def write_dict(dct):
    tb = bytearray()
    offs = []
    for e in dct.entries:
        offs.append(len(tb))
        tb += e
        if len(tb) & 1:
            tb += b'\x00'
    offs.append(len(tb))
    ob = bytearray()
    for o in offs:
        assert o < (1 << 24)
        ob += bytes([o & 255, (o >> 8) & 255, (o >> 16) & 255])
    open(os.path.join(OUT, 'tiles.bin'), 'wb').write(bytes(tb))
    open(os.path.join(OUT, 'tile_off.bin'), 'wb').write(bytes(ob))
    return len(tb)


def main():
    os.makedirs(OUT, exist_ok=True)
    dct = Dict()
    report = {}
    for name in SCENES:
        encode_scene(name, dct, report)
        print('encoded', name)
    tbytes = write_dict(dct)
    obytes = 3 * (len(dct.entries) + 1)
    print('shared dict entries:', len(dct.entries), 'data bytes:', tbytes, 'offtab:', obytes)
    tot = sum(r['bytes'] for r in report.values())
    with open(os.path.join(OUT, 'report.txt'), 'w') as f:
        for k, v in report.items():
            f.write(f"{k}: {v}\n")
        f.write(f"tiles: {len(dct.entries)} tilebytes: {tbytes} total_video: {tot}\n")
    for k, v in report.items():
        print(f"  {k}: runs {v['runs']} up {v['uploads']} cell {v['cellwrites']} {v['bytes']}B maxops {v['maxops']}")
    print('total video bytes:', tot)
    with open(os.path.join(OUT, 'fmv_index.h'), 'w') as f:
        f.write('#ifndef FMV_INDEX_H\n#define FMV_INDEX_H\n')
        f.write(f'#define FMV_NTILES {len(dct.entries)}\n')
        f.write(f'#define FMV_TILEDATA_BYTES {tbytes}\n')
        f.write('#define FMV_NSCENES 15\n')
        f.write('#endif\n')


if __name__ == '__main__':
    main()
